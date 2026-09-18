#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/device/AudioDevice.h>
#include <sa/device/AudioDeviceBackend.h>
#include <sa/device/AudioDeviceManager.h>
#include <sa/device/NullAudioDevice.h>

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace sa;
using namespace sa::device;

namespace {

AudioDeviceDescription describe(std::string id, int inputs, int outputs, bool isDefault = false) {
    AudioDeviceDescription description;
    description.id = std::move(id);
    description.name = "Test device";
    description.maxInputChannels = inputs;
    description.maxOutputChannels = outputs;
    description.sampleRates = {kSampleRate44100, kSampleRate48000};
    description.defaultBufferFrames = 128;
    description.isDefault = isDefault;
    return description;
}

/// A backend the tests drive directly: it can be told what to enumerate and
/// whether opening should fail, which is how the fallback chain gets exercised
/// without needing hardware that misbehaves on demand.
class FakeBackend final : public AudioDeviceBackend {
public:
    FakeBackend(std::vector<AudioDeviceDescription> devices, bool openSucceeds)
        : devices_(std::move(devices)), openSucceeds_(openSucceeds) {}

    [[nodiscard]] std::string_view name() const noexcept override { return "fake"; }

    [[nodiscard]] std::vector<AudioDeviceDescription> enumerate() const override {
        return devices_;
    }

    [[nodiscard]] Result<std::unique_ptr<AudioDevice>>
    open(const AudioDeviceDescription& description, const AudioDeviceConfig& config) override {
        openCalls.push_back(description.id);
        if (!openSucceeds_) {
            return Error{ErrorCode::IoFailure, "fake backend refuses to open " + description.id};
        }
        return std::unique_ptr<AudioDevice>{std::make_unique<NullAudioDevice>(
            description, config, NullAudioDevice::Pacing::FreeRun)};
    }

    /// Records which devices were tried, in order, so a test can assert on the
    /// fallback path rather than only on its result.
    std::vector<std::string> openCalls;

private:
    std::vector<AudioDeviceDescription> devices_;
    bool openSucceeds_;
};

std::vector<std::unique_ptr<AudioDeviceBackend>>
backendsOf(std::unique_ptr<AudioDeviceBackend> first, bool withNull = true) {
    std::vector<std::unique_ptr<AudioDeviceBackend>> backends;
    backends.push_back(std::move(first));
    if (withNull) {
        backends.push_back(std::make_unique<NullAudioBackend>(NullAudioDevice::Pacing::FreeRun));
    }
    return backends;
}

} // namespace

TEST_CASE("The default manager always offers something to open", "[device][manager]") {
    // The promise the null backend exists to keep: a machine with no audio
    // service still gets a device, so nothing above needs a "no device" path.
    AudioDeviceManager manager;

    REQUIRE_FALSE(manager.devices().empty());
    const AudioDeviceDescription* nullDevice = manager.find(NullAudioBackend::kDeviceId);
    REQUIRE(nullDevice != nullptr);
    CHECK(nullDevice->hasOutput());
    CHECK(manager.defaultDevice() != nullptr);
    CHECK(manager.find("nothing:here") == nullptr);
}

TEST_CASE("A device opened by id runs", "[device][manager]") {
    AudioDeviceManager manager;
    AudioDeviceConfig config;
    config.bufferFrames = 32;

    auto opened = manager.open(NullAudioBackend::kDeviceId, config);
    REQUIRE(opened.hasValue());

    std::unique_ptr<AudioDevice> device = std::move(opened).value();
    REQUIRE(device != nullptr);
    CHECK(device->outputChannels() == 2);
    CHECK(device->bufferFrames() == 32);
    CHECK_FALSE(device->isRunning());

    REQUIRE(device->start([](ConstAudioBufferView, AudioBufferView) {}).ok());
    CHECK(device->isRunning());
    device->stop();
    CHECK_FALSE(device->isRunning());
}

TEST_CASE("Opening an id that is not there fails rather than guessing", "[device][manager]") {
    AudioDeviceManager manager;

    auto opened = manager.open("wasapi:long-gone", AudioDeviceConfig{});
    REQUIRE_FALSE(opened.hasValue());
    CHECK(opened.error().code() == ErrorCode::NotFound);
}

TEST_CASE("A missing device falls back instead of refusing to start", "[device][manager]") {
    // The backend list is explicit rather than the platform's. With the real
    // one this asserted that the fallback lands on the null device, which is
    // only true on a machine that has no working audio: give it a sound card
    // and the fallback correctly lands on that instead, and the test fails for
    // being right. Naming the backends keeps the assertion at full strength and
    // makes it mean the same thing everywhere.
    std::vector<std::unique_ptr<AudioDeviceBackend>> backends;
    backends.push_back(std::make_unique<NullAudioBackend>());
    AudioDeviceManager manager{std::move(backends)};

    auto opened = manager.openOrFallback("wasapi:long-gone", AudioDeviceConfig{});
    REQUIRE(opened.hasValue());
    CHECK(std::move(opened).value()->description().id == NullAudioBackend::kDeviceId);

    auto noPreference = manager.openOrFallback("", AudioDeviceConfig{});
    CHECK(noPreference.hasValue());
}

TEST_CASE("The platform's own fallback opens something, whatever the machine has",
          "[device][manager]") {
    // The weaker claim the test above used to make by accident, stated
    // deliberately: with the real backends, asking for a device that is not
    // there must still hand back a working one. Which one depends on the
    // hardware, so that is exactly what is not asserted.
    AudioDeviceManager manager;

    auto opened = manager.openOrFallback("wasapi:long-gone", AudioDeviceConfig{});
    REQUIRE(opened.hasValue());
    CHECK(opened.value()->outputChannels() > 0);
}

TEST_CASE("A device that enumerates but will not open is skipped", "[device][manager]") {
    // Exclusive-mode contention and a driver that has gone away both look like
    // this: the device is listed, and opening it fails anyway.
    auto backend = std::make_unique<FakeBackend>(
        std::vector<AudioDeviceDescription>{describe("fake:broken", 0, 2, /*isDefault=*/true)},
        /*openSucceeds=*/false);
    FakeBackend* observer = backend.get();

    AudioDeviceManager manager{backendsOf(std::move(backend))};
    REQUIRE(manager.devices().size() == 2);
    REQUIRE(manager.defaultDevice() != nullptr);
    REQUIRE(manager.defaultDevice()->id == "fake:broken");

    auto opened = manager.openOrFallback("fake:broken", AudioDeviceConfig{});
    REQUIRE(opened.hasValue());
    CHECK(std::move(opened).value()->description().id == NullAudioBackend::kDeviceId);

    // Tried once as the named device, not again as the default: the fallback
    // chain must not retry what it has already ruled out.
    CHECK(observer->openCalls.size() == 1);
}

TEST_CASE("The flagged default wins over enumeration order", "[device][manager]") {
    std::vector<AudioDeviceDescription> devices{
        describe("fake:first", 2, 2),
        describe("fake:preferred", 0, 8, /*isDefault=*/true),
    };
    AudioDeviceManager manager{
        backendsOf(std::make_unique<FakeBackend>(std::move(devices), /*openSucceeds=*/true))};

    REQUIRE(manager.defaultDevice() != nullptr);
    CHECK(manager.defaultDevice()->id == "fake:preferred");

    auto opened = manager.openDefault(AudioDeviceConfig{});
    REQUIRE(opened.hasValue());
    CHECK(std::move(opened).value()->description().id == "fake:preferred");
}

TEST_CASE("A manager with no devices reports that plainly", "[device][manager]") {
    AudioDeviceManager manager{std::vector<std::unique_ptr<AudioDeviceBackend>>{}};

    CHECK(manager.devices().empty());
    CHECK(manager.defaultDevice() == nullptr);

    auto byDefault = manager.openDefault(AudioDeviceConfig{});
    CHECK_FALSE(byDefault.hasValue());
    CHECK(byDefault.error().code() == ErrorCode::NotFound);

    auto byFallback = manager.openOrFallback("anything", AudioDeviceConfig{});
    CHECK_FALSE(byFallback.hasValue());
    CHECK(byFallback.error().code() == ErrorCode::NotFound);
}

TEST_CASE("Refreshing picks up a device list that has changed", "[device][manager]") {
    AudioDeviceManager manager{
        backendsOf(std::make_unique<FakeBackend>(
                       std::vector<AudioDeviceDescription>{describe("fake:one", 0, 2)}, true),
                   /*withNull=*/false)};

    REQUIRE(manager.devices().size() == 1);
    CHECK(manager.refresh().ok());
    CHECK(manager.devices().size() == 1);
    CHECK(manager.find("fake:one") != nullptr);
}

TEST_CASE("Negotiation clamps a request to what the device offers", "[device][manager]") {
    const AudioDeviceDescription stereo = describe("fake:stereo", 2, 2);

    AudioDeviceConfig requested;
    requested.inputChannels = 8;
    requested.outputChannels = 8;
    requested.bufferFrames = 256;
    requested.sampleRate = kSampleRate48000;

    const AudioDeviceConfig effective = negotiateConfig(stereo, requested);
    CHECK(effective.inputChannels == 2);
    CHECK(effective.outputChannels == 2);
    CHECK(effective.bufferFrames == 256);
    CHECK(effective.sampleRate == kSampleRate48000);
}

TEST_CASE("Negotiation substitutes the nearest supported sample rate", "[device][manager]") {
    AudioDeviceDescription device = describe("fake:rates", 0, 2);
    device.sampleRates = {kSampleRate48000, kSampleRate96000};

    AudioDeviceConfig requested;
    requested.sampleRate = kSampleRate44100;

    // Nearest in octaves, not in hertz: 44100 is almost equidistant from 48000
    // and 96000 on a linear scale, and obviously closer to 48000 to a listener.
    CHECK(negotiateConfig(device, requested).sampleRate == kSampleRate48000);

    requested.sampleRate = SampleRate{88200.0};
    CHECK(negotiateConfig(device, requested).sampleRate == kSampleRate96000);
}

TEST_CASE("Negotiation fills in a block size nobody specified", "[device][manager]") {
    AudioDeviceDescription device = describe("fake:blocks", 0, 2);

    AudioDeviceConfig requested;
    requested.bufferFrames = 0;
    CHECK(negotiateConfig(device, requested).bufferFrames == device.defaultBufferFrames);

    device.defaultBufferFrames = 0;
    CHECK(negotiateConfig(device, requested).bufferFrames == kDefaultBufferFrames);

    requested.bufferFrames = 1;
    CHECK(negotiateConfig(device, requested).bufferFrames == kMinBufferFrames);

    requested.bufferFrames = 1 << 24;
    CHECK(negotiateConfig(device, requested).bufferFrames == kMaxBufferFrames);
}

TEST_CASE("Negotiation rescues a configuration with no usable sample rate", "[device][manager]") {
    AudioDeviceDescription openEnded = describe("fake:any", 0, 2);
    openEnded.sampleRates.clear(); // an endpoint that will not say what it takes

    AudioDeviceConfig requested;
    requested.sampleRate = SampleRate{};
    CHECK(negotiateConfig(openEnded, requested).sampleRate == kSampleRate48000);

    const AudioDeviceDescription listed = describe("fake:listed", 0, 2);
    CHECK(negotiateConfig(listed, requested).sampleRate == kSampleRate44100);
}

TEST_CASE("Asking for a direction a device does not have is an error", "[device][manager]") {
    // Not silently opened output-only: a caller that asked to record and got a
    // playback stream would record nothing and never know why.
    auto backend = std::make_unique<FakeBackend>(
        std::vector<AudioDeviceDescription>{describe("fake:outputonly", 0, 2)}, true);
    AudioDeviceManager manager{backendsOf(std::move(backend), /*withNull=*/false)};

    AudioDeviceConfig captureOnly;
    captureOnly.inputChannels = 2;
    captureOnly.outputChannels = 0;

    auto opened = manager.open("fake:outputonly", captureOnly);
    REQUIRE_FALSE(opened.hasValue());
    CHECK(opened.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("A description reports what it supports", "[device][manager]") {
    const AudioDeviceDescription device = describe("fake:desc", 1, 0);

    CHECK(device.hasInput());
    CHECK_FALSE(device.hasOutput());
    CHECK(device.supportsSampleRate(kSampleRate44100));
    CHECK_FALSE(device.supportsSampleRate(kSampleRate96000));
    CHECK_FALSE(device.supportsSampleRate(SampleRate{}));

    AudioDeviceDescription openEnded = device;
    openEnded.sampleRates.clear();

    // An unknown constraint must not read as a prohibition, or devices that
    // would have worked get rejected before they are ever tried.
    CHECK(openEnded.supportsSampleRate(kSampleRate96000));
    CHECK(openEnded.closestSampleRate(kSampleRate96000) == kSampleRate96000);
}
