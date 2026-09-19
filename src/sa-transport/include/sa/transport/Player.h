#pragma once

#include <sa/core/AudioProcessor.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/device/AudioDevice.h>
#include <sa/device/SpscRingBuffer.h>
#include <sa/io/AudioSource.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace sa::transport {

/// Streams an audio source to an output device.
///
/// Two threads and a ring between them. A worker reads the source -- which for
/// this product means rendering a document, a thing that allocates, locks and
/// touches files -- and the audio callback does nothing but drain the ring. That
/// split is the whole design: the render can take as long as it likes as long as
/// it stays ahead, and the callback's work is bounded and identical every block.
///
/// Playback position is published as an atomic counted by the callback, so the
/// number the user sees is frames that have actually left, not frames that have
/// been queued.
class Player {
public:
    /// Ring capacity, in seconds of audio. Long enough that a slow render or a
    /// scheduling hiccup on the worker does not reach the callback; short
    /// enough that stopping is not audibly late.
    static constexpr double kBufferSeconds = 0.75;

    /// Takes ownership of an open device. The device must not be running.
    [[nodiscard]] static Result<Player> create(std::unique_ptr<device::AudioDevice> device);

    Player(Player&&) noexcept;
    Player& operator=(Player&&) noexcept;
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    /// Play [from, to) of `source`. Any current playback stops first.
    ///
    /// The source is read on the worker thread and held for the duration, so it
    /// must remain valid -- hence the shared_ptr rather than a reference.
    /// `processor`, if given, runs inside the audio callback on each block just
    /// before it reaches the device, and must stay alive until playback stops
    /// -- see AudioProcessor. It is taken here rather than through a setter
    /// because that is the lifetime it needs: stop() joins the worker and stops
    /// the device, so once it returns the callback is not running. A processor
    /// that has to be reconfigured while playing does that by publishing to
    /// itself, not by being swapped.
    [[nodiscard]] Status play(std::shared_ptr<const io::AudioSource> source, SampleIndex from,
                              SampleIndex to, AudioProcessor* processor = nullptr);

    /// Stop and join the worker. Idempotent.
    void stop();

    [[nodiscard]] bool isPlaying() const noexcept;

    /// Frames played so far, as an absolute position in the source.
    [[nodiscard]] SampleIndex position() const noexcept;

    /// Blocks the callback could not fill because the worker fell behind.
    /// Non-zero means the render is not keeping up and the user heard a gap.
    [[nodiscard]] std::uint64_t underruns() const noexcept;

    [[nodiscard]] const device::AudioDevice& device() const noexcept { return *device_; }

private:
    explicit Player(std::unique_ptr<device::AudioDevice> device);

    void runWorker(std::shared_ptr<const io::AudioSource> source, SampleIndex from, SampleIndex to);

    std::unique_ptr<device::AudioDevice> device_;
    std::unique_ptr<device::SpscRingBuffer> ring_;

    /// De-interleaving scratch for the callback. Sized in play(), on the
    /// calling thread, because the callback may not allocate -- and it cannot
    /// borrow the output buffer for this: that is planar, so channel 0 holds
    /// one block of one channel, not one block of all of them.
    std::vector<float> scratch_;

    /// Read by the callback only, and only while playing. Set in play() before
    /// the device is started and cleared by stop() after it has stopped, both
    /// on the calling thread, so it is never written while the callback could
    /// read it and needs no atomic.
    AudioProcessor* processor_ = nullptr;

    std::thread worker_;
    std::atomic<bool> running_{false};

    /// Where the callback has reached, as an offset into the played range.
    std::atomic<std::int64_t> framesPlayed_{0};
    std::atomic<std::int64_t> rangeStart_{0};
    std::atomic<std::int64_t> rangeLength_{0};
    std::atomic<std::uint64_t> underruns_{0};

    /// Set by the worker once it has queued the last frame, so the callback can
    /// tell "the render is late" from "there is nothing left", and only the
    /// first of those is an underrun.
    std::atomic<bool> sourceExhausted_{false};
};

} // namespace sa::transport
