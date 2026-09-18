#include <sa/core/AudioBuffer.h>
#include <sa/core/RealtimeGuard.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/device/AudioDevice.h>
#include <sa/device/AudioDeviceManager.h>
#include <sa/device/NullAudioDevice.h>
#include <sa/device/WasapiAudioDevice.h>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <utility>

using namespace sa;
using namespace sa::device;

TEST_CASE("The default manager wires the backends this platform has", "[device][wasapi]") {
    // The one assertion that holds on every platform, and the one that would
    // actually break if a backend were registered on the wrong one: WASAPI ids
    // exist on Windows and nowhere else, and the null device is always there
    // whatever else is.
    AudioDeviceManager manager;

    bool sawWasapi = false;
    for (const AudioDeviceDescription& description : manager.devices()) {
        if (description.id.starts_with("wasapi:")) {
            sawWasapi = true;
        }
    }

#if defined(_WIN32)
    CHECK(sawWasapi);
#else
    CHECK_FALSE(sawWasapi);
#endif

    CHECK(manager.find(NullAudioBackend::kDeviceId) != nullptr);
    CHECK(manager.defaultDevice() != nullptr);
}

#if defined(_WIN32)

namespace {

void fillOutput(AudioBufferView output, float value) noexcept {
    for (int channel = 0; channel < output.channelCount(); ++channel) {
        float* samples = output.channel(channel);
        for (SampleCount i = 0; i < output.frames(); ++i) {
            samples[i] = value;
        }
    }
}

AudioDeviceDescription defaultDescription() {
    AudioDeviceDescription description;
    description.id = std::string{WasapiAudioBackend::kDefaultDeviceId};
    description.name = "Default output";
    description.maxInputChannels = 0;
    description.maxOutputChannels = 2;
    description.defaultBufferFrames = kDefaultBufferFrames;
    description.isDefault = true;
    return description;
}

AudioDeviceConfig testConfig(int frames = 128) {
    AudioDeviceConfig config;
    config.sampleRate = kSampleRate48000;
    config.inputChannels = 0;
    config.outputChannels = 2;
    config.bufferFrames = frames;
    return config;
}

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

} // namespace

TEST_CASE("WASAPI enumeration is shaped the way the manager expects", "[device][wasapi]") {
    const WasapiAudioBackend backend;
    const std::vector<AudioDeviceDescription> devices = backend.enumerate();

    // The synthetic default is offered even on a machine with no endpoint at
    // all: it is what a stream follows once one appears, and a build agent with
    // no sound card is exactly where that has to stay true.
    REQUIRE_FALSE(devices.empty());
    CHECK(devices.front().id == WasapiAudioBackend::kDefaultDeviceId);
    CHECK(devices.front().isDefault);

    int flaggedDefault = 0;
    for (const AudioDeviceDescription& description : devices) {
        CHECK(description.id.starts_with(WasapiAudioBackend::kIdPrefix));
        CHECK_FALSE(description.name.empty());
        CHECK(description.maxOutputChannels > 0);

        // Playback only. A caller asking to record must get a refusal from the
        // manager, not a stream that quietly records silence.
        CHECK(description.maxInputChannels == 0);

        if (description.isDefault) {
            ++flaggedDefault;
        }
    }
    CHECK(flaggedDefault == 1);
}

TEST_CASE("The WASAPI backend refuses what it cannot do", "[device][wasapi]") {
    WasapiAudioBackend backend;

    AudioDeviceDescription foreign = defaultDescription();
    foreign.id = "alsa:default";
    CHECK(backend.open(foreign, testConfig()).error().code() == ErrorCode::NotFound);

    AudioDeviceConfig captureOnly = testConfig();
    captureOnly.inputChannels = 2;
    captureOnly.outputChannels = 0;
    CHECK(backend.open(defaultDescription(), captureOnly).error().code() ==
          ErrorCode::InvalidArgument);

    AudioDeviceDescription unnamed = defaultDescription();
    unnamed.id = "wasapi:";
    CHECK(backend.open(unnamed, testConfig()).error().code() == ErrorCode::NotFound);
}

TEST_CASE("A WASAPI stream renders blocks of the negotiated size", "[device][wasapi]") {
    WasapiAudioBackend backend;
    auto opened = backend.open(defaultDescription(), testConfig());
    if (!opened.hasValue()) {
        // A headless build agent has no render endpoint at all. open() saying
        // so up front is the point of the check it does: it is what lets
        // openOrFallback move on to something that works instead of stopping
        // here and failing later from start().
        CHECK(opened.error().code() == ErrorCode::NotFound);
        SUCCEED("no WASAPI render endpoint is available on this machine");
        return;
    }

    std::unique_ptr<AudioDevice> device = std::move(opened).value();
    CHECK(device->outputChannels() == 2);
    CHECK(device->inputChannels() == 0);
    CHECK(device->bufferFrames() == 128);

    std::atomic<bool> wrongLength{false};
    std::atomic<std::size_t> allocations{0};
    const Status started = device->start([&](ConstAudioBufferView, AudioBufferView output) {
        const rt::AllocationScope scope;
        if (output.frames() != 128) {
            wrongLength.store(true, std::memory_order_relaxed);
        }
        fillOutput(output, 0.0f);
        allocations.fetch_add(scope.count(), std::memory_order_relaxed);
    });

    if (!started.ok()) {
        // A headless build agent has no render endpoint, and that is a fact
        // about the machine rather than a failure of this code. The refusal
        // still has to come back as an error rather than as a device that
        // reports success and never calls anything, which is what the assertion
        // below pins.
        CHECK_FALSE(device->isRunning());
        SUCCEED("no WASAPI render endpoint is available on this machine");
        return;
    }

    auto* threaded = dynamic_cast<ThreadedOutputDevice*>(device.get());
    REQUIRE(threaded != nullptr);
    REQUIRE(waitFor([&] { return threaded->blocksRendered() >= 8; }));
    device->stop();

    CHECK_FALSE(device->isRunning());
    CHECK_FALSE(wrongLength.load());

    // The endpoint's rate, not necessarily the one asked for: shared mode runs
    // at whatever the mixer is running at unless the resampler was accepted.
    CHECK(device->sampleRate().isValid());

    if (rt::checksEnabled()) {
        CHECK(allocations.load() == 0);
    }
}

#endif // _WIN32
