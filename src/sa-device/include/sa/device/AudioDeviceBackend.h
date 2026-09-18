#pragma once

#include <sa/core/Result.h>
#include <sa/device/AudioDevice.h>
#include <sa/device/AudioDeviceDescription.h>

#include <memory>
#include <string_view>
#include <vector>

namespace sa::device {

/// One platform audio API: enumerates its devices and opens them.
///
/// Backends are the seam that keeps the platform out of everything above.
/// AudioDeviceManager owns a list of them and never knows which is which, so a
/// WASAPI backend appearing on Windows changes nothing for callers.
class AudioDeviceBackend {
public:
    virtual ~AudioDeviceBackend() = default;

    AudioDeviceBackend(const AudioDeviceBackend&) = delete;
    AudioDeviceBackend& operator=(const AudioDeviceBackend&) = delete;

    /// Short identifier, also used as the prefix of every id this backend
    /// mints, e.g. "null" or "wasapi".
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// Devices visible right now. Enumeration touches the OS and allocates;
    /// it belongs on a worker thread, never near the audio thread.
    [[nodiscard]] virtual std::vector<AudioDeviceDescription> enumerate() const = 0;

    /// Opens `description`, which must have come from this backend's
    /// enumerate(). `config` has already been negotiated against the
    /// description by the manager, but a backend may still clamp further.
    [[nodiscard]] virtual Result<std::unique_ptr<AudioDevice>>
    open(const AudioDeviceDescription& description, const AudioDeviceConfig& config) = 0;

protected:
    AudioDeviceBackend() = default;
};

} // namespace sa::device
