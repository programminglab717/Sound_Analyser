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

/// Compiled only where libasound's headers were found, exactly as sa-ui is
/// compiled only where Qt was. The macro is set by the device layer's
/// CMakeLists and is PUBLIC, because a caller -- including the tests -- has to
/// be able to ask whether this backend is in the build at all.
#if defined(SA_DEVICE_HAVE_ALSA)

namespace sa::device {

/// The stream's ALSA half. Opaque on purpose: the PCM handle and its staging
/// buffer live in the source file, so nothing that includes this header needs
/// libasound's headers on its include path.
struct AlsaStream;

/// Playback through ALSA, so that the device layer is exercisable on Linux.
///
/// This is the backend that makes the WASAPI one reviewable: both sit on
/// ThreadedOutputDevice, so the lifecycle, the re-blocking and the restart
/// behaviour are the same code, and that code is tested here on a machine that
/// need not have a sound card -- ALSA's own "null" PCM is enough.
///
/// Shared access only. ALSA's exclusive-ish hw: devices are reachable by id if
/// a caller insists, but nothing here asks for them: taking a card away from
/// the rest of the desktop is a decision for the user, not a default.
class AlsaAudioDevice final : public ThreadedOutputDevice {
public:
    AlsaAudioDevice(AudioDeviceDescription description, AudioDeviceConfig config,
                    std::string pcmName);

    ~AlsaAudioDevice() override;

protected:
    [[nodiscard]] Status openStream() override;

    void runStream() noexcept override;

    void closeStream() noexcept override;

    void configureRenderThread() noexcept override;

private:
    /// Attempts the recovery ALSA offers for a transient write failure.
    /// Returns false when the stream has to be rebuilt instead.
    [[nodiscard]] bool recover(int error) noexcept;

    std::string pcmName_;
    std::unique_ptr<AlsaStream> stream_;
};

/// Enumerates and opens ALSA playback PCMs.
///
/// open() checks that the PCM will open before handing back a device. It has
/// to: AudioDeviceManager::openOrFallback stops walking the device list at
/// the first backend that says yes, so a backend that always says yes turns
/// the fallback chain into a single attempt.
class AlsaAudioBackend final : public AudioDeviceBackend {
public:
    static constexpr std::string_view kBackendName = "alsa";

    /// Prefix of every id this backend mints. What follows it is the ALSA PCM
    /// name verbatim -- "default", "sysdefault:CARD=PCH", "hw:0,0" -- which is
    /// stable across reboots and so safe for a session to remember.
    static constexpr std::string_view kIdPrefix = "alsa:";

    [[nodiscard]] std::string_view name() const noexcept override { return kBackendName; }

    [[nodiscard]] std::vector<AudioDeviceDescription> enumerate() const override;

    [[nodiscard]] Result<std::unique_ptr<AudioDevice>>
    open(const AudioDeviceDescription& description, const AudioDeviceConfig& config) override;
};

} // namespace sa::device

#endif // SA_DEVICE_HAVE_ALSA
