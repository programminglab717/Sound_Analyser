#include <sa/core/AudioBuffer.h>
#include <sa/core/RealtimeGuard.h>
#include <sa/core/Types.h>
#include <sa/device/NullAudioDevice.h>
#include <sa/device/SpscRingBuffer.h>

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <string>
#include <thread>

using namespace sa;
using namespace sa::device;

namespace {

AudioDeviceDescription testDescription() {
    AudioDeviceDescription description;
    description.id = std::string{NullAudioBackend::kDeviceId};
    description.name = "Null device (silence)";
    description.maxInputChannels = 2;
    description.maxOutputChannels = 2;
    description.sampleRates = {kSampleRate48000};
    description.defaultBufferFrames = 64;
    return description;
}

AudioDeviceConfig testConfig(int inputChannels = 1, int outputChannels = 2, int frames = 64) {
    AudioDeviceConfig config;
    config.sampleRate = kSampleRate48000;
    config.inputChannels = inputChannels;
    config.outputChannels = outputChannels;
    config.bufferFrames = frames;
    return config;
}

/// Polls rather than uses a condition variable: the thing being waited on is
/// the device's own progress counter, and a test that hangs on a missed
/// notification is worse than one that takes a millisecond longer.
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

/// Fills every output channel with `value`, as a real callback would.
void fillOutput(AudioBufferView output, float value) noexcept {
    for (int channel = 0; channel < output.channelCount(); ++channel) {
        float* samples = output.channel(channel);
        for (SampleCount i = 0; i < output.frames(); ++i) {
            samples[i] = value;
        }
    }
}

} // namespace

TEST_CASE("The callback runs and is handed the configured block", "[device][null]") {
    NullAudioDevice device{testDescription(), testConfig(), NullAudioDevice::Pacing::FreeRun};

    std::atomic<int> inputChannels{-1};
    std::atomic<int> outputChannels{-1};
    std::atomic<SampleCount> inputFrames{-1};
    std::atomic<SampleCount> outputFrames{-1};
    std::atomic<bool> inputWasSilent{true};

    REQUIRE(device
                .start([&](ConstAudioBufferView input, AudioBufferView output) {
                    inputChannels.store(input.channelCount(), std::memory_order_relaxed);
                    outputChannels.store(output.channelCount(), std::memory_order_relaxed);
                    inputFrames.store(input.frames(), std::memory_order_relaxed);
                    outputFrames.store(output.frames(), std::memory_order_relaxed);

                    for (int channel = 0; channel < input.channelCount(); ++channel) {
                        const float* samples = input.channel(channel);
                        for (SampleCount i = 0; i < input.frames(); ++i) {
                            if (samples[i] != 0.0f) {
                                inputWasSilent.store(false, std::memory_order_relaxed);
                            }
                        }
                    }

                    fillOutput(output, 0.25f);
                })
                .ok());

    CHECK(device.isRunning());
    REQUIRE(waitFor([&] { return device.blocksRendered() >= 4; }));
    device.stop();

    CHECK(inputChannels.load() == 1);
    CHECK(outputChannels.load() == 2);
    CHECK(inputFrames.load() == 64);
    CHECK(outputFrames.load() == 64);
    CHECK(inputWasSilent.load());

    CHECK(device.sampleRate() == kSampleRate48000);
    CHECK(device.bufferFrames() == 64);
    CHECK(device.inputChannels() == 1);
    CHECK(device.outputChannels() == 2);
    CHECK(device.description().id == NullAudioBackend::kDeviceId);
}

TEST_CASE("A direction with no channels still reports the block length", "[device][null]") {
    // Otherwise an output-only callback has no way to learn how many frames it
    // was asked for when it happens to look at the input view first.
    NullAudioDevice device{testDescription(), testConfig(/*inputChannels=*/0),
                           NullAudioDevice::Pacing::FreeRun};

    std::atomic<SampleCount> inputFrames{-1};
    std::atomic<int> inputChannels{-1};

    REQUIRE(device
                .start([&](ConstAudioBufferView input, AudioBufferView output) {
                    inputChannels.store(input.channelCount(), std::memory_order_relaxed);
                    inputFrames.store(input.frames(), std::memory_order_relaxed);
                    fillOutput(output, 0.0f);
                })
                .ok());
    REQUIRE(waitFor([&] { return device.blocksRendered() >= 1; }));
    device.stop();

    CHECK(inputChannels.load() == 0);
    CHECK(inputFrames.load() == 64);
}

TEST_CASE("Starting an already-running device is refused", "[device][null]") {
    NullAudioDevice device{testDescription(), testConfig(), NullAudioDevice::Pacing::FreeRun};

    std::atomic<int> firstCallbackRuns{0};
    std::atomic<int> secondCallbackRuns{0};

    REQUIRE(device
                .start([&](ConstAudioBufferView, AudioBufferView output) {
                    firstCallbackRuns.fetch_add(1, std::memory_order_relaxed);
                    fillOutput(output, 0.0f);
                })
                .ok());
    REQUIRE(waitFor([&] { return firstCallbackRuns.load() >= 2; }));

    // The callback cannot be swapped under a live audio thread, so the call is
    // refused rather than succeeding while quietly ignoring what was passed.
    // Silently keeping the old callback is the failure mode that surfaces as
    // "my new callback never fires" long after the cause.
    CHECK_FALSE(device
                    .start([&](ConstAudioBufferView, AudioBufferView output) {
                        secondCallbackRuns.fetch_add(1, std::memory_order_relaxed);
                        fillOutput(output, 0.0f);
                    })
                    .ok());

    // The refusal must not disturb the stream that was already running.
    CHECK(device.isRunning());

    const int runsBefore = firstCallbackRuns.load();
    REQUIRE(waitFor([&] { return firstCallbackRuns.load() > runsBefore + 4; }));
    device.stop();

    CHECK(secondCallbackRuns.load() == 0);
}

TEST_CASE("Stopping is idempotent and safe before any start", "[device][null]") {
    NullAudioDevice device{testDescription(), testConfig(), NullAudioDevice::Pacing::FreeRun};

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

TEST_CASE("Stop waits for a callback that is already running", "[device][null]") {
    NullAudioDevice device{testDescription(), testConfig(), NullAudioDevice::Pacing::FreeRun};

    std::atomic<bool> insideCallback{false};
    std::atomic<bool> callbackFinished{false};
    std::atomic<bool> release{false};

    REQUIRE(device
                .start([&](ConstAudioBufferView, AudioBufferView output) {
                    insideCallback.store(true, std::memory_order_release);
                    // Deliberately illegal in a real callback -- this one exists
                    // to hold the render thread inside the callback while another
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
    device.stop(); // must not deadlock against the in-flight callback
    releaser.join();

    // The contract callers rely on to destroy what the callback captured: once
    // stop() has returned, the callback has finished and will not run again.
    CHECK(callbackFinished.load(std::memory_order_acquire));
    CHECK_FALSE(device.isRunning());

    const std::size_t blocks = device.blocksRendered();
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    CHECK(device.blocksRendered() == blocks);
}

TEST_CASE("A callback may stop its own device", "[device][null]") {
    // A real callback does this on a fatal stream error. Joining the render
    // thread from inside itself would deadlock, so stop() has to detect it.
    NullAudioDevice device{testDescription(), testConfig(), NullAudioDevice::Pacing::FreeRun};

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

    // The thread is unwinding but not yet joined; stopping again from here is
    // what actually joins it, and must not hang.
    device.stop();
    const std::size_t blocks = device.blocksRendered();
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    CHECK(device.blocksRendered() == blocks);
    CHECK(runs.load() >= 3);

    // And the device is reusable afterwards rather than wedged.
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

TEST_CASE("A device restarts after being stopped", "[device][null]") {
    NullAudioDevice device{testDescription(), testConfig(), NullAudioDevice::Pacing::FreeRun};

    std::atomic<int> firstRun{0};
    REQUIRE(device
                .start([&](ConstAudioBufferView, AudioBufferView output) {
                    firstRun.fetch_add(1, std::memory_order_relaxed);
                    fillOutput(output, 0.0f);
                })
                .ok());
    REQUIRE(waitFor([&] { return firstRun.load() >= 2; }));
    device.stop();

    const std::size_t afterFirstRun = device.blocksRendered();

    std::atomic<int> secondRun{0};
    REQUIRE(device
                .start([&](ConstAudioBufferView, AudioBufferView output) {
                    secondRun.fetch_add(1, std::memory_order_relaxed);
                    fillOutput(output, 0.0f);
                })
                .ok());
    CHECK(device.isRunning());
    REQUIRE(waitFor([&] { return secondRun.load() >= 2; }));
    device.stop();

    CHECK(device.blocksRendered() > afterFirstRun);
}

TEST_CASE("Destroying a running device stops it first", "[device][null]") {
    // The callback captures state that outlives the device by one scope. If the
    // destructor did not join, the render thread would touch it afterwards --
    // which is the use-after-free this test exists to pin.
    std::atomic<int> runs{0};
    std::atomic<bool> destroyed{false};
    std::atomic<bool> ranAfterDestruction{false};

    {
        NullAudioDevice device{testDescription(), testConfig(), NullAudioDevice::Pacing::FreeRun};
        // Catch2's assertion macros are not thread-safe, so the callback only
        // records; the main thread does the asserting once it has joined.
        REQUIRE(device
                    .start([&](ConstAudioBufferView, AudioBufferView output) {
                        if (destroyed.load(std::memory_order_acquire)) {
                            ranAfterDestruction.store(true, std::memory_order_release);
                        }
                        runs.fetch_add(1, std::memory_order_relaxed);
                        fillOutput(output, 0.0f);
                    })
                    .ok());
        REQUIRE(waitFor([&] { return runs.load() >= 4; }));
    }

    destroyed.store(true, std::memory_order_release);
    const int afterDestruction = runs.load();
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    CHECK(runs.load() == afterDestruction);
    CHECK_FALSE(ranAfterDestruction.load(std::memory_order_acquire));
}

TEST_CASE("Starting refuses a configuration it cannot honour", "[device][null]") {
    NullAudioDevice device{testDescription(), testConfig(/*inputChannels=*/0, /*outputChannels=*/0),
                           NullAudioDevice::Pacing::FreeRun};

    const Status noChannels = device.start([](ConstAudioBufferView, AudioBufferView) {});
    CHECK_FALSE(noChannels.ok());
    CHECK(noChannels.error().code() == ErrorCode::InvalidArgument);
    CHECK_FALSE(device.isRunning());

    NullAudioDevice usable{testDescription(), testConfig(), NullAudioDevice::Pacing::FreeRun};
    const Status noCallback = usable.start(nullptr);
    CHECK_FALSE(noCallback.ok());
    CHECK(noCallback.error().code() == ErrorCode::InvalidArgument);
    CHECK_FALSE(usable.isRunning());
}

TEST_CASE("Real-time pacing holds the callback to the simulated rate", "[device][null]") {
    // 64 frames at 48 kHz is 1.33 ms per block, so eight blocks cannot possibly
    // arrive in under ten milliseconds. Asserting the floor rather than a rate
    // keeps this from turning flaky on a loaded machine, where the only thing
    // that can happen is that blocks arrive later.
    NullAudioDevice device{testDescription(), testConfig(), NullAudioDevice::Pacing::RealTime};

    const auto startedAt = std::chrono::steady_clock::now();
    REQUIRE(
        device.start([](ConstAudioBufferView, AudioBufferView output) { fillOutput(output, 0.0f); })
            .ok());
    REQUIRE(waitFor([&] { return device.blocksRendered() >= 8; }));
    const auto elapsed = std::chrono::steady_clock::now() - startedAt;
    device.stop();

    CHECK(elapsed >= std::chrono::milliseconds{8});
}

TEST_CASE("A callback moving audio through a ring buffer allocates nothing", "[device][null][rt]") {
    if (!rt::checksEnabled()) {
        SUCCEED("SA_RT_SAFETY_CHECKS is off in this build; detection not compiled in");
        return;
    }

    // The shape the architecture prescribes: the callback fills its output and
    // publishes a copy through an SPSC ring for a worker thread to pick up,
    // touching nothing that could allocate (docs/03-architecture.md §3).
    NullAudioDevice device{testDescription(), testConfig(), NullAudioDevice::Pacing::FreeRun};
    SpscRingBuffer ring{4096};
    std::array<float, 64> scratch{};

    const rt::AllocationScope scope;
    REQUIRE(device
                .start([&](ConstAudioBufferView, AudioBufferView output) {
                    fillOutput(output, 0.5f);
                    if (output.channelCount() > 0) {
                        const float* samples = output.channel(0);
                        static_cast<void>(
                            ring.write(samples, static_cast<std::size_t>(output.frames())));
                    }
                    static_cast<void>(ring.read(scratch.data(), scratch.size()));
                })
                .ok());
    REQUIRE(waitFor([&] { return device.blocksRendered() >= 16; }));
    device.stop();

    // Only allocations made while marked as the audio thread are counted, so
    // everything this test itself does off that thread is invisible here.
    CHECK(scope.count() == 0);
}
