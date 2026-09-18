#include <sa/core/AudioBuffer.h>
#include <sa/core/RealtimeGuard.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/device/AudioDevice.h>
#include <sa/device/InterleavedOutput.h>
#include <sa/device/ThreadedOutputDevice.h>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace sa;
using namespace sa::device;

namespace {

AudioDeviceDescription describe() {
    AudioDeviceDescription description;
    description.id = "fake:stream";
    description.name = "Fake stream";
    description.maxOutputChannels = 2;
    description.sampleRates = {kSampleRate48000};
    description.defaultBufferFrames = 64;
    return description;
}

AudioDeviceConfig testConfig(int outputChannels = 2, int frames = 64) {
    AudioDeviceConfig config;
    config.sampleRate = kSampleRate48000;
    config.inputChannels = 0;
    config.outputChannels = outputChannels;
    config.bufferFrames = frames;
    return config;
}

/// A backend with no platform behind it.
///
/// This is what makes the lifecycle testable at all: WASAPI and ALSA differ
/// only in the four calls below, and none of the behaviour worth pinning --
/// the start-up handshake, what stop() promises, a callback that stops its own
/// device, rebuilding after an endpoint is pulled -- is in those four. Driving
/// them from here proves the shared half on a machine with no sound card, and
/// on demand: a real device cannot be asked to be invalidated.
class FakeStreamDevice final : public ThreadedOutputDevice {
public:
    FakeStreamDevice(AudioDeviceConfig config, int chunkFrames)
        : ThreadedOutputDevice(describe(), config), channels_(config.outputChannels),
          chunkFrames_(chunkFrames), scratch_(static_cast<std::size_t>(chunkFrames) *
                                              static_cast<std::size_t>(config.outputChannels)) {}

    ~FakeStreamDevice() override {
        // Every concrete backend has to do this: the base destructor cannot,
        // because by the time it runs the overrides below are gone.
        stop();
    }

    std::atomic<int> openAttempts{0};
    std::atomic<int> closeCalls{0};
    std::atomic<int> configureCalls{0};
    std::atomic<int> releaseCalls{0};

    /// Makes the next N openStream() calls fail, as an endpoint that has been
    /// unplugged would.
    std::atomic<int> failNextOpens{0};

    /// Ends the current run once, as AUDCLNT_E_DEVICE_INVALIDATED does.
    std::atomic<bool> breakStream{false};

    std::atomic<double> negotiatedRateHz{48000.0};

protected:
    [[nodiscard]] Status openStream() override {
        openAttempts.fetch_add(1, std::memory_order_release);

        // Only the render thread ever calls this, so a plain read-then-write is
        // enough; there is no second writer to race with.
        if (failNextOpens.load(std::memory_order_acquire) > 0) {
            failNextOpens.fetch_sub(1, std::memory_order_acq_rel);
            return Error{ErrorCode::IoFailure, "fake stream refused to open"};
        }

        open_ = true;
        setActualSampleRate(SampleRate{negotiatedRateHz.load(std::memory_order_acquire)});
        return Status{};
    }

    void runStream() noexcept override {
        if (!open_) {
            return;
        }
        while (keepRunning()) {
            if (breakStream.exchange(false, std::memory_order_acq_rel)) {
                return;
            }
            blocker().render(callback(), scratch_.data(), static_cast<SampleCount>(chunkFrames_),
                             InterleavedFormat::Float32, channels_);

            // Stands in for the pacing a driver would impose. A real backend
            // blocks inside the platform instead.
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }

    void closeStream() noexcept override {
        open_ = false;
        closeCalls.fetch_add(1, std::memory_order_release);
    }

    void configureRenderThread() noexcept override {
        configureCalls.fetch_add(1, std::memory_order_release);
    }

    void releaseRenderThread() noexcept override {
        releaseCalls.fetch_add(1, std::memory_order_release);
    }

private:
    int channels_;
    int chunkFrames_;
    std::vector<float> scratch_;

    /// Touched only by the render thread, so it needs no synchronisation.
    bool open_ = false;
};

template <typename Predicate>
[[nodiscard]] bool waitFor(Predicate predicate,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return predicate();
}

void fillOutput(AudioBufferView output, float value) noexcept {
    for (int channel = 0; channel < output.channelCount(); ++channel) {
        float* samples = output.channel(channel);
        for (SampleCount i = 0; i < output.frames(); ++i) {
            samples[i] = value;
        }
    }
}

} // namespace

TEST_CASE("A threaded device starts, renders and stops", "[device][threaded]") {
    FakeStreamDevice device{testConfig(), /*chunkFrames=*/64};

    CHECK_FALSE(device.isRunning());
    CHECK(device.inputChannels() == 0);
    CHECK(device.outputChannels() == 2);
    CHECK(device.bufferFrames() == 64);
    CHECK(device.restartCount() == 0);

    std::atomic<SampleCount> seenFrames{-1};
    REQUIRE(device
                .start([&](ConstAudioBufferView, AudioBufferView output) {
                    seenFrames.store(output.frames(), std::memory_order_relaxed);
                    fillOutput(output, 0.1f);
                })
                .ok());

    CHECK(device.isRunning());
    REQUIRE(waitFor([&] { return device.blocksRendered() >= 4; }));
    device.stop();

    CHECK_FALSE(device.isRunning());
    CHECK(seenFrames.load() == 64);
    CHECK(device.openAttempts.load() == 1);
    CHECK(device.closeCalls.load() >= 1);

    // The scheduling hooks are a matched pair, taken out and put back on the
    // render thread itself -- which is the only thread that can do either.
    CHECK(device.configureCalls.load() == 1);
    CHECK(device.releaseCalls.load() == 1);
}

TEST_CASE("A driver chunk unlike the block size still feeds whole blocks", "[device][threaded]") {
    // 100 frames per driver chunk against a 64-frame block: the callback must
    // still see 64 every time, which is the promise a processor sized to its
    // block length depends on.
    FakeStreamDevice device{testConfig(2, 64), /*chunkFrames=*/100};

    std::atomic<bool> sawWrongLength{false};
    REQUIRE(device
                .start([&](ConstAudioBufferView, AudioBufferView output) {
                    if (output.frames() != 64) {
                        sawWrongLength.store(true, std::memory_order_relaxed);
                    }
                    fillOutput(output, 0.2f);
                })
                .ok());

    REQUIRE(waitFor([&] { return device.blocksRendered() >= 20; }));
    device.stop();

    CHECK_FALSE(sawWrongLength.load());
}

TEST_CASE("A stream that fails to open reports through start()", "[device][threaded]") {
    FakeStreamDevice device{testConfig(), 64};
    device.failNextOpens.store(1, std::memory_order_release);

    const Status started = device.start(
        [](ConstAudioBufferView, AudioBufferView output) { fillOutput(output, 0.0f); });

    CHECK_FALSE(started.ok());
    CHECK(started.error().code() == ErrorCode::IoFailure);
    CHECK_FALSE(device.isRunning());

    // One attempt and no more: a first open that fails is start() failing, and
    // retrying behind the caller's back would hand it a stream it was told it
    // would not get.
    CHECK(device.openAttempts.load() == 1);
    CHECK(device.restartCount() == 0);

    // And the device is reusable afterwards rather than wedged.
    REQUIRE(
        device.start([](ConstAudioBufferView, AudioBufferView output) { fillOutput(output, 0.0f); })
            .ok());
    REQUIRE(waitFor([&] { return device.blocksRendered() >= 2; }));
    device.stop();
}

TEST_CASE("An invalidated stream is rebuilt underneath the callback", "[device][threaded]") {
    FakeStreamDevice device{testConfig(), 64};

    std::atomic<int> calls{0};
    REQUIRE(device
                .start([&](ConstAudioBufferView, AudioBufferView output) {
                    calls.fetch_add(1, std::memory_order_relaxed);
                    fillOutput(output, 0.3f);
                })
                .ok());
    REQUIRE(waitFor([&] { return calls.load() >= 2; }));

    device.breakStream.store(true, std::memory_order_release);
    REQUIRE(waitFor([&] { return device.restartCount() >= 1; }));

    // Audio resumes on the new stream. That is the whole point: a user who
    // unplugs an interface should hear a gap, not silence until they restart.
    const int afterRestart = calls.load();
    REQUIRE(waitFor([&] { return calls.load() > afterRestart + 2; }));
    CHECK(device.isRunning());

    device.stop();
    CHECK(device.openAttempts.load() == 2);
}

TEST_CASE("A rebuild that keeps failing keeps trying", "[device][threaded]") {
    // An interface mid-reconnect fails to open several times running. Giving up
    // would leave an application permanently silent until it was restarted.
    FakeStreamDevice device{testConfig(), 64};

    REQUIRE(
        device.start([](ConstAudioBufferView, AudioBufferView output) { fillOutput(output, 0.4f); })
            .ok());
    REQUIRE(waitFor([&] { return device.blocksRendered() >= 2; }));

    device.failNextOpens.store(2, std::memory_order_release);
    device.breakStream.store(true, std::memory_order_release);

    REQUIRE(waitFor([&] { return device.restartCount() >= 1; }));
    CHECK(device.openAttempts.load() >= 4); // the first, two refusals, then the one that worked

    const std::size_t blocks = device.blocksRendered();
    REQUIRE(waitFor([&] { return device.blocksRendered() > blocks + 2; }));
    device.stop();
}

TEST_CASE("Every stream that opens is closed again", "[device][threaded]") {
    // A stop() that lands between the stream coming up and the first pass round
    // the render loop used to skip the close entirely: the PCM handle, or on
    // Windows a set of COM objects outliving their apartment. Starting and
    // stopping back to back is the way to land in that window, so do it enough
    // times to hit it.
    for (int attempt = 0; attempt < 32; ++attempt) {
        FakeStreamDevice device{testConfig(), 64};
        REQUIRE(device
                    .start([](ConstAudioBufferView, AudioBufferView output) {
                        fillOutput(output, 0.0f);
                    })
                    .ok());
        device.stop();

        CHECK(device.openAttempts.load() >= 1);
        CHECK(device.closeCalls.load() >= device.openAttempts.load());
        CHECK(device.releaseCalls.load() == 1);
    }
}

TEST_CASE("Starting an already-running threaded device is refused", "[device][threaded]") {
    FakeStreamDevice device{testConfig(), 64};

    std::atomic<int> firstRuns{0};
    std::atomic<int> secondRuns{0};

    REQUIRE(device
                .start([&](ConstAudioBufferView, AudioBufferView output) {
                    firstRuns.fetch_add(1, std::memory_order_relaxed);
                    fillOutput(output, 0.0f);
                })
                .ok());
    REQUIRE(waitFor([&] { return firstRuns.load() >= 2; }));

    CHECK_FALSE(device
                    .start([&](ConstAudioBufferView, AudioBufferView output) {
                        secondRuns.fetch_add(1, std::memory_order_relaxed);
                        fillOutput(output, 0.0f);
                    })
                    .ok());
    CHECK(device.isRunning());

    const int before = firstRuns.load();
    REQUIRE(waitFor([&] { return firstRuns.load() > before + 2; }));
    device.stop();

    CHECK(secondRuns.load() == 0);
}

TEST_CASE("Stopping a threaded device is idempotent and safe before any start",
          "[device][threaded]") {
    FakeStreamDevice device{testConfig(), 64};

    device.stop();
    CHECK_FALSE(device.isRunning());

    REQUIRE(
        device.start([](ConstAudioBufferView, AudioBufferView output) { fillOutput(output, 0.0f); })
            .ok());
    REQUIRE(waitFor([&] { return device.blocksRendered() >= 1; }));

    device.stop();
    device.stop();
    CHECK_FALSE(device.isRunning());
}

TEST_CASE("Stop waits for a callback that is already running", "[device][threaded]") {
    FakeStreamDevice device{testConfig(), 64};

    std::atomic<bool> insideCallback{false};
    std::atomic<bool> callbackFinished{false};
    std::atomic<bool> release{false};

    REQUIRE(device
                .start([&](ConstAudioBufferView, AudioBufferView output) {
                    insideCallback.store(true, std::memory_order_release);
                    // Deliberately illegal in a real callback. It exists to
                    // hold the render thread inside the callback while another
                    // thread tries to stop the device underneath it.
                    while (!release.load(std::memory_order_acquire)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds{1});
                    }
                    fillOutput(output, 0.5f);
                    callbackFinished.store(true, std::memory_order_release);
                })
                .ok());

    REQUIRE(waitFor([&] { return insideCallback.load(std::memory_order_acquire); }));

    std::thread releaser{[&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        release.store(true, std::memory_order_release);
    }};
    device.stop();
    releaser.join();

    // The guarantee callers rely on to destroy what the callback captured.
    CHECK(callbackFinished.load(std::memory_order_acquire));
    CHECK_FALSE(device.isRunning());
}

TEST_CASE("A callback may stop its own device", "[device][threaded]") {
    // A real callback does this on a fatal stream error. Joining the render
    // thread from inside itself would deadlock, so stop() has to detect it.
    FakeStreamDevice device{testConfig(), 64};

    std::atomic<int> runs{0};
    REQUIRE(device
                .start([&](ConstAudioBufferView, AudioBufferView output) {
                    fillOutput(output, 0.0f);
                    if (runs.fetch_add(1, std::memory_order_relaxed) == 2) {
                        device.stop();
                    }
                })
                .ok());

    REQUIRE(waitFor([&] { return !device.isRunning(); }));

    // The thread is unwinding but not yet joined; stopping again is what
    // actually joins it, and must not hang.
    device.stop();
    const std::size_t blocks = device.blocksRendered();
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    CHECK(device.blocksRendered() == blocks);
    CHECK(runs.load() >= 3);

    std::atomic<int> restartRuns{0};
    REQUIRE(device
                .start([&](ConstAudioBufferView, AudioBufferView output) {
                    restartRuns.fetch_add(1, std::memory_order_relaxed);
                    fillOutput(output, 0.0f);
                })
                .ok());
    REQUIRE(waitFor([&] { return restartRuns.load() >= 2; }));
    device.stop();
}

TEST_CASE("Destroying a running threaded device stops it first", "[device][threaded]") {
    // The callback captures state that outlives the device by one scope. If the
    // destructor did not join, the render thread would touch it afterwards --
    // which is the use-after-free this test exists to pin.
    std::atomic<int> runs{0};
    std::atomic<bool> destroyed{false};
    std::atomic<bool> ranAfterDestruction{false};

    {
        FakeStreamDevice device{testConfig(), 64};
        REQUIRE(device
                    .start([&](ConstAudioBufferView, AudioBufferView output) {
                        if (destroyed.load(std::memory_order_acquire)) {
                            ranAfterDestruction.store(true, std::memory_order_release);
                        }
                        runs.fetch_add(1, std::memory_order_relaxed);
                        fillOutput(output, 0.0f);
                    })
                    .ok());
        REQUIRE(waitFor([&] { return runs.load() >= 2; }));
    }
    destroyed.store(true, std::memory_order_release);

    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    CHECK_FALSE(ranAfterDestruction.load(std::memory_order_acquire));
}

TEST_CASE("The rate reported is the one the stream negotiated", "[device][threaded]") {
    // A shared-mode endpoint runs at the mixer's rate, not the one that was
    // asked for, and a stream rebuilt onto a different endpoint can land
    // somewhere else again. sampleRate() has to tell the truth about that.
    FakeStreamDevice device{testConfig(), 64};
    CHECK(device.sampleRate() == kSampleRate48000);

    device.negotiatedRateHz.store(44100.0, std::memory_order_release);
    REQUIRE(
        device.start([](ConstAudioBufferView, AudioBufferView output) { fillOutput(output, 0.0f); })
            .ok());
    CHECK(device.sampleRate() == kSampleRate44100);

    device.negotiatedRateHz.store(96000.0, std::memory_order_release);
    device.breakStream.store(true, std::memory_order_release);
    REQUIRE(waitFor([&] { return device.restartCount() >= 1; }));
    CHECK(device.sampleRate() == kSampleRate96000);

    device.stop();
}

TEST_CASE("A threaded device refuses a configuration it cannot honour", "[device][threaded]") {
    FakeStreamDevice captureOnly{testConfig(/*outputChannels=*/0), 64};
    const Status started = captureOnly.start(
        [](ConstAudioBufferView, AudioBufferView output) { fillOutput(output, 0.0f); });
    CHECK_FALSE(started.ok());
    CHECK(started.error().code() == ErrorCode::InvalidArgument);

    FakeStreamDevice device{testConfig(), 64};
    CHECK_FALSE(device.start(AudioCallback{}).ok());
    CHECK_FALSE(device.isRunning());
}

TEST_CASE("The threaded render path allocates nothing in the callback", "[device][threaded][rt]") {
    if (!rt::checksEnabled()) {
        SUCCEED("SA_RT_SAFETY_CHECKS is off in this build");
        return;
    }

    FakeStreamDevice device{testConfig(), /*chunkFrames=*/100};

    std::atomic<std::size_t> allocations{0};
    REQUIRE(device
                .start([&](ConstAudioBufferView, AudioBufferView output) {
                    const rt::AllocationScope scope;
                    fillOutput(output, 0.6f);
                    allocations.fetch_add(scope.count(), std::memory_order_relaxed);
                })
                .ok());

    REQUIRE(waitFor([&] { return device.blocksRendered() >= 20; }));

    // A rebuild is the other moment worth checking: it happens with the
    // callback still installed, and nothing about it may reach into the block
    // the callback is handed.
    device.breakStream.store(true, std::memory_order_release);
    REQUIRE(waitFor([&] { return device.restartCount() >= 1; }));
    const std::size_t blocks = device.blocksRendered();
    REQUIRE(waitFor([&] { return device.blocksRendered() > blocks + 4; }));
    device.stop();

    CHECK(allocations.load() == 0);
}
