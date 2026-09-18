#include <sa/core/AudioBuffer.h>
#include <sa/core/RealtimeGuard.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/device/AlsaAudioDevice.h>
#include <sa/device/AudioDevice.h>
#include <sa/device/AudioDeviceManager.h>

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

namespace {

/// ALSA's own discard-everything PCM. It is a real ALSA device -- it negotiates
/// a format, accepts writes and reports errors like any other -- but it needs
/// no sound card, which is what makes the whole render path testable in CI and
/// in a container. The backend deliberately leaves it out of enumerate(); a
/// test asks for it by name.
constexpr std::string_view kNullPcmId = "alsa:null";

AudioDeviceDescription nullPcmDescription() {
    AudioDeviceDescription description;
    description.id = std::string{kNullPcmId};
    description.name = "ALSA null PCM";
    description.maxOutputChannels = 2;
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

TEST_CASE("The ALSA backend opens a PCM and runs the callback", "[device][alsa]") {
    AlsaAudioBackend backend;
    auto opened = backend.open(nullPcmDescription(), testConfig());
    REQUIRE(opened.hasValue());

    std::unique_ptr<AudioDevice> device = std::move(opened).value();
    REQUIRE(device != nullptr);
    CHECK(device->outputChannels() == 2);
    CHECK(device->inputChannels() == 0);
    CHECK(device->bufferFrames() == 64);
    CHECK_FALSE(device->isRunning());

    std::atomic<int> outputChannels{-1};
    std::atomic<SampleCount> outputFrames{-1};
    std::atomic<SampleCount> inputFrames{-1};
    std::atomic<int> inputChannels{-1};

    REQUIRE(device
                ->start([&](ConstAudioBufferView input, AudioBufferView output) {
                    outputChannels.store(output.channelCount(), std::memory_order_relaxed);
                    outputFrames.store(output.frames(), std::memory_order_relaxed);
                    inputChannels.store(input.channelCount(), std::memory_order_relaxed);
                    inputFrames.store(input.frames(), std::memory_order_relaxed);
                    fillOutput(output, 0.25f);
                })
                .ok());
    CHECK(device->isRunning());

    auto* threaded = dynamic_cast<ThreadedOutputDevice*>(device.get());
    REQUIRE(threaded != nullptr);
    REQUIRE(waitFor([&] { return threaded->blocksRendered() >= 8; }));
    device->stop();
    CHECK_FALSE(device->isRunning());

    CHECK(outputChannels.load() == 2);
    CHECK(outputFrames.load() == 64);

    // A direction the device does not have still reports the block length, so
    // an output-only callback can size its work from either view.
    CHECK(inputChannels.load() == 0);
    CHECK(inputFrames.load() == 64);

    CHECK(device->sampleRate() == kSampleRate48000);
}

TEST_CASE("The ALSA callback allocates nothing", "[device][alsa][rt]") {
    if (!rt::checksEnabled()) {
        SUCCEED("SA_RT_SAFETY_CHECKS is off in this build");
        return;
    }

    AlsaAudioBackend backend;
    auto opened = backend.open(nullPcmDescription(), testConfig());
    REQUIRE(opened.hasValue());
    std::unique_ptr<AudioDevice> device = std::move(opened).value();

    // Counted inside the callback rather than around it: the driver calls that
    // bracket it are the operating system's business and allocate freely, and
    // what this test is about is the path from the render loop into the caller.
    std::atomic<std::size_t> allocations{0};
    REQUIRE(device
                ->start([&](ConstAudioBufferView, AudioBufferView output) {
                    const rt::AllocationScope scope;
                    fillOutput(output, 0.5f);
                    allocations.fetch_add(scope.count(), std::memory_order_relaxed);
                })
                .ok());

    auto* threaded = dynamic_cast<ThreadedOutputDevice*>(device.get());
    REQUIRE(threaded != nullptr);
    REQUIRE(waitFor([&] { return threaded->blocksRendered() >= 16; }));
    device->stop();

    CHECK(allocations.load() == 0);
}

TEST_CASE("The ALSA backend refuses what it cannot do", "[device][alsa]") {
    AlsaAudioBackend backend;

    AudioDeviceDescription foreign = nullPcmDescription();
    foreign.id = "wasapi:something";
    CHECK(backend.open(foreign, testConfig()).error().code() == ErrorCode::NotFound);

    AudioDeviceConfig captureOnly = testConfig();
    captureOnly.inputChannels = 2;
    captureOnly.outputChannels = 0;
    CHECK(backend.open(nullPcmDescription(), captureOnly).error().code() ==
          ErrorCode::InvalidArgument);

    AudioDeviceDescription unnamed = nullPcmDescription();
    unnamed.id = "alsa:";
    CHECK(backend.open(unnamed, testConfig()).error().code() == ErrorCode::NotFound);
}

TEST_CASE("Opening a PCM that is not there fails at open, not at start", "[device][alsa]") {
    // It has to fail here rather than later: AudioDeviceManager::openOrFallback
    // walks the device list until something opens, and a backend that always
    // says yes stops that walk at the first candidate -- leaving the failure to
    // surface from start(), with the fallback chain already spent.
    AlsaAudioBackend backend;
    AudioDeviceDescription missing = nullPcmDescription();
    missing.id = "alsa:sa-no-such-pcm";

    auto opened = backend.open(missing, testConfig());
    REQUIRE_FALSE(opened.hasValue());
    CHECK(opened.error().code() == ErrorCode::NotFound);
}

TEST_CASE("ALSA enumeration never offers the discard PCM", "[device][alsa]") {
    // Listing it would mean a user picking a device that looks healthy and
    // plays nothing, with no way to tell from the UI that it never could.
    const AlsaAudioBackend backend;
    for (const AudioDeviceDescription& description : backend.enumerate()) {
        CHECK(description.id != kNullPcmId);
        CHECK(description.id.starts_with(AlsaAudioBackend::kIdPrefix));
        CHECK(description.maxInputChannels == 0);
    }
}
