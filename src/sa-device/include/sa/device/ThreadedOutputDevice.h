#pragma once

#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/device/AudioDevice.h>
#include <sa/device/AudioDeviceDescription.h>
#include <sa/device/InterleavedOutput.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>

namespace sa::device {

/// How long start() will wait for the render thread to report that the stream
/// is up. Generous, because opening a cold USB interface genuinely takes a
/// second or two; bounded, because a driver that never answers must not take
/// the application down with it.
inline constexpr int kStreamStartTimeoutMs = 5000;

/// How long a broken stream waits before it is rebuilt. Long enough that a
/// device which has been unplugged for good does not have us re-enumerating
/// flat out, short enough that plugging it back in feels immediate.
inline constexpr int kStreamRestartDelayMs = 250;

/// The half of a hardware playback device that has nothing to do with the
/// platform.
///
/// WASAPI and ALSA disagree about almost everything except their shape: a
/// thread of our own that builds a stream, pushes blocks at it until something
/// goes wrong, and rebuilds it when it does. Writing that shape once means the
/// awkward parts -- the start-up handshake, the promise stop() makes about the
/// callback having finished, a callback that stops its own device, rebuilding
/// after the endpoint is pulled -- are written once and can be tested on a
/// machine with no sound card at all. What is left for a backend to supply is
/// only the four calls below, and those are the only parts that need the
/// platform's headers.
///
/// Every backend here is playback-only, which is why inputChannels() is fixed
/// at zero. Capture is a separate exercise: it is not simply the same code with
/// the arrows reversed, because the two directions have to be locked together
/// on a shared clock to be useful, and getting that wrong is worse than not
/// offering it.
class ThreadedOutputDevice : public AudioDevice {
public:
    ~ThreadedOutputDevice() override;

    [[nodiscard]] const AudioDeviceDescription& description() const noexcept override {
        return description_;
    }

    [[nodiscard]] Status start(AudioCallback callback) override;

    void stop() noexcept override;

    [[nodiscard]] bool isRunning() const noexcept override {
        return running_.load(std::memory_order_acquire);
    }

    /// The rate the stream is really running at, which is not necessarily the
    /// one that was asked for and can change while the device is open: a stream
    /// that rebuilds itself onto a different endpoint lands on that endpoint's
    /// clock.
    [[nodiscard]] SampleRate sampleRate() const noexcept override {
        return SampleRate{actualSampleRateHz_.load(std::memory_order_acquire)};
    }

    /// Fixed for the life of the device, by design. The driver's own chunk size
    /// varies and may change across a rebuild; absorbing that in OutputBlocker
    /// rather than passing it on is what lets a callback size its state once.
    [[nodiscard]] int bufferFrames() const noexcept override { return config_.bufferFrames; }

    [[nodiscard]] int inputChannels() const noexcept override { return 0; }

    [[nodiscard]] int outputChannels() const noexcept override { return config_.outputChannels; }

    /// Blocks handed to the callback since the last start(). The only honest
    /// answer to "is audio actually flowing", and what a test waits on. It
    /// counts blocks, not driver chunks, so it is unaffected by however the
    /// platform happens to be sizing its buffers.
    [[nodiscard]] std::size_t blocksRendered() const noexcept { return blocker_.blocksRendered(); }

    /// Times the stream has been torn down and successfully rebuilt underneath
    /// a running callback. Zero is the normal number; anything else means the
    /// default device moved or an endpoint was invalidated.
    [[nodiscard]] std::size_t restartCount() const noexcept {
        return restarts_.load(std::memory_order_acquire);
    }

protected:
    ThreadedOutputDevice(AudioDeviceDescription description, AudioDeviceConfig config);

    /// Builds the platform stream. Always called on the render thread, so a
    /// backend never has to think about which thread owns its handles or, on
    /// Windows, which apartment they were created in.
    ///
    /// Must return rather than block indefinitely: start() is waiting on it.
    [[nodiscard]] virtual Status openStream() = 0;

    /// Pushes audio until keepRunning() goes false or the stream stops working.
    ///
    /// Returning while keepRunning() is still true is how a backend says "this
    /// stream is finished but the device is not" -- an invalidated endpoint, a
    /// default device that moved -- and the base class rebuilds. Must return
    /// promptly and harmlessly if openStream() never succeeded.
    virtual void runStream() noexcept = 0;

    /// Releases whatever openStream() acquired. Called for every openStream(),
    /// successful or not, so it has to tolerate a half-built stream.
    virtual void closeStream() noexcept = 0;

    /// Makes a blocked runStream() return. Called from stop(), on another
    /// thread, so it may only touch things that are safe to touch concurrently.
    /// A backend whose wait is already bounded can leave this empty.
    virtual void wakeStream() noexcept {}

    /// Raises the render thread to whatever priority the platform reserves for
    /// audio. Called once, on the render thread, before the first openStream().
    virtual void configureRenderThread() noexcept {}

    /// Undoes configureRenderThread(). Called once, on the render thread, just
    /// before it exits -- which is the only place it can be called, because a
    /// scheduling registration belongs to the thread that took it out and a
    /// destructor runs on somebody else's.
    virtual void releaseRenderThread() noexcept {}

    [[nodiscard]] bool keepRunning() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] const AudioDeviceConfig& config() const noexcept { return config_; }

    [[nodiscard]] OutputBlocker& blocker() noexcept { return blocker_; }

    /// The callback to hand to OutputBlocker::render. Stable for as long as the
    /// stream runs: start() refuses to replace it under a live thread.
    [[nodiscard]] const AudioCallback& callback() const noexcept { return callback_; }

    /// Records the rate the stream actually negotiated, for sampleRate().
    void setActualSampleRate(SampleRate rate) noexcept {
        actualSampleRateHz_.store(rate.hz(), std::memory_order_release);
    }

private:
    void renderThreadMain();

    void joinRenderThread() noexcept;

    void publishStartup(const Status& status) noexcept;

    /// Sleeps kStreamRestartDelayMs, cut short by stop().
    void waitBeforeRestart() noexcept;

    AudioDeviceDescription description_;
    AudioDeviceConfig config_;

    OutputBlocker blocker_;
    AudioCallback callback_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<std::size_t> restarts_{0};
    std::atomic<double> actualSampleRateHz_{0.0};

    /// Serialises start/stop against each other. Never taken by the render
    /// thread, so a callback calling stop() cannot deadlock against a join.
    mutable std::mutex lifecycle_;

    /// Carries the first openStream() result back to start(), and doubles as
    /// the timer a restart waits on so that stop() can cut it short.
    std::mutex handshake_;
    std::condition_variable handshakeSignal_;
    bool startupSettled_ = false;
    Status startupStatus_;
};

} // namespace sa::device
