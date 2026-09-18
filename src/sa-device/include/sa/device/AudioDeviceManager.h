#pragma once

#include <sa/core/Result.h>
#include <sa/device/AudioDevice.h>
#include <sa/device/AudioDeviceBackend.h>
#include <sa/device/AudioDeviceDescription.h>

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

namespace sa::device {

/// Reconciles a requested configuration with what a device actually offers.
///
/// Clamping beats failing here: a saved session asking for eight channels on a
/// stereo interface should open in stereo, not refuse to open. The returned
/// config is what the caller will really get, so it can be shown or stored.
[[nodiscard]] AudioDeviceConfig negotiateConfig(const AudioDeviceDescription& description,
                                                const AudioDeviceConfig& requested) noexcept;

/// Enumerates devices across every backend and opens them.
///
/// The single entry point to the device layer. Callers name a device by id and
/// receive an AudioDevice; which backend served it, and whether that backend
/// talks to WASAPI or to nothing at all, is not visible from here.
class AudioDeviceManager {
public:
    /// Registers the backends this build has, ending with the null backend so
    /// that there is always at least one device to open.
    AudioDeviceManager();

    /// Uses exactly the backends given. The seam tests open a manager through,
    /// and the one a future "ASIO only" preference would use.
    explicit AudioDeviceManager(std::vector<std::unique_ptr<AudioDeviceBackend>> backends);

    /// Re-enumerates every backend. Call after a device-change notification;
    /// allocates and talks to the OS, so keep it off the audio thread.
    Status refresh();

    [[nodiscard]] const std::vector<AudioDeviceDescription>& devices() const noexcept {
        return devices_;
    }

    /// The description for `id`, or nullptr. The pointer is invalidated by the
    /// next refresh().
    [[nodiscard]] const AudioDeviceDescription* find(std::string_view id) const noexcept;

    /// The device flagged default, else the first with an output, else the
    /// first of any kind, else nullptr.
    [[nodiscard]] const AudioDeviceDescription* defaultDevice() const noexcept;

    /// Opens `id`. Fails with NotFound if it is gone -- use openOrFallback()
    /// when a missing device should not be fatal.
    [[nodiscard]] Result<std::unique_ptr<AudioDevice>> open(std::string_view id,
                                                            const AudioDeviceConfig& config);

    [[nodiscard]] Result<std::unique_ptr<AudioDevice>> openDefault(const AudioDeviceConfig& config);

    /// Opens `id` if it is present and opens, otherwise the default device.
    /// This is what application startup should call: the interface a session
    /// remembers is routinely unplugged, and the right answer is to come up on
    /// something else, not to refuse to start.
    [[nodiscard]] Result<std::unique_ptr<AudioDevice>> openOrFallback(std::string_view id,
                                                                      const AudioDeviceConfig& config);

private:
    [[nodiscard]] Result<std::unique_ptr<AudioDevice>> openAt(std::size_t index,
                                                              const AudioDeviceConfig& config);

    std::vector<std::unique_ptr<AudioDeviceBackend>> backends_;
    std::vector<AudioDeviceDescription> devices_;

    /// Parallel to devices_: which backend produced each one. Cheaper and less
    /// fragile than re-deriving it from the id prefix at open time.
    std::vector<std::size_t> deviceBackend_;
};

} // namespace sa::device
