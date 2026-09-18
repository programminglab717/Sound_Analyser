#pragma once

#include <sa/core/Result.h>
#include <sa/device/AudioDevice.h>
#include <sa/device/AudioDeviceBackend.h>
#include <sa/device/AudioDeviceDescription.h>
#include <sa/device/ThreadedOutputDevice.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)

namespace sa::device {

/// The stream's Windows half. Opaque on purpose: every COM interface, handle
/// and WAVEFORMATEX lives in the source file, so nothing that includes this
/// header needs the Windows SDK on its include path or inherits <windows.h>'s
/// macros.
struct WasapiStream;

/// Playback through WASAPI, in shared mode, driven by the endpoint's own event.
///
/// Shared rather than exclusive because this is a repair and mastering
/// application, not a live rig: taking the endpoint away from the rest of the
/// desktop so that a notification cannot interrupt a render buys a millisecond
/// or two of latency and costs the user every other sound their machine makes.
/// Exclusive mode is a preference to add later, not a default.
///
/// Event-driven rather than polled because the alternative is a timer that is
/// either too slow -- and glitches -- or too fast, and burns a core to find an
/// empty buffer. The endpoint knows when it needs audio; waiting on it is both
/// lower latency and cheaper.
///
/// The stream rebuilds itself underneath a running callback when the endpoint
/// is invalidated (a device unplugged, a driver updated, the audio service
/// restarted) and, for the "default output" device, when Windows moves the
/// default somewhere else. Callers see nothing but a gap in the audio and a
/// bump in restartCount(); the block size and channel count they negotiated
/// never change, whatever the new endpoint turns out to be.
class WasapiAudioDevice final : public ThreadedOutputDevice {
public:
    /// `endpointId` is the WASAPI endpoint string, wide because that is what
    /// IMMDeviceEnumerator::GetDevice takes and round-tripping it through UTF-8
    /// twice would be two more chances to mangle it. Ignored, and may be empty,
    /// when `followsDefault` is set.
    WasapiAudioDevice(AudioDeviceDescription description, AudioDeviceConfig config,
                      std::wstring endpointId, bool followsDefault);

    ~WasapiAudioDevice() override;

protected:
    [[nodiscard]] Status openStream() override;

    void runStream() noexcept override;

    void closeStream() noexcept override;

    void wakeStream() noexcept override;

    void configureRenderThread() noexcept override;

    void releaseRenderThread() noexcept override;

private:
    /// Fills whatever the endpoint has free. False means the stream is finished
    /// and has to be rebuilt -- which is what AUDCLNT_E_DEVICE_INVALIDATED
    /// looks like from in here.
    [[nodiscard]] bool renderAvailable() noexcept;

    std::wstring endpointId_;

    /// True for the synthetic "default output" device, which is the only one
    /// that should move when Windows moves the default. A caller that named a
    /// specific endpoint asked for that endpoint and must keep getting it.
    bool followsDefault_ = false;

    std::unique_ptr<WasapiStream> stream_;
};

/// Enumerates and opens WASAPI render endpoints.
///
/// open() checks that the endpoint is still there before handing back a
/// device. It has to: AudioDeviceManager::openOrFallback stops walking the
/// device list at the first backend that says yes, so a backend that always
/// says yes turns the fallback chain into a single attempt.
class WasapiAudioBackend final : public AudioDeviceBackend {
public:
    static constexpr std::string_view kBackendName = "wasapi";

    /// Prefix of every id this backend mints. What follows is the endpoint's
    /// own identifier string, which Windows keeps stable across reboots and
    /// re-plugs, so a session may safely remember it.
    static constexpr std::string_view kIdPrefix = "wasapi:";

    /// The device that means "whatever Windows currently calls the default
    /// output, including after that changes". Offered as its own entry rather
    /// than inferred from a flag so that the choice is the user's: picking the
    /// interface that happens to be default today is a different intention from
    /// picking whatever is default tomorrow.
    static constexpr std::string_view kDefaultDeviceId = "wasapi:default";

    [[nodiscard]] std::string_view name() const noexcept override { return kBackendName; }

    [[nodiscard]] std::vector<AudioDeviceDescription> enumerate() const override;

    [[nodiscard]] Result<std::unique_ptr<AudioDevice>>
    open(const AudioDeviceDescription& description, const AudioDeviceConfig& config) override;
};

} // namespace sa::device

#endif // _WIN32
