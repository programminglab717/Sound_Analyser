#pragma once

#include <sa/core/Types.h>

#include <string>
#include <vector>

namespace sa::device {

/// What a backend can tell us about one device, before it is opened.
///
/// Populated by enumeration and safe to copy, store and show in a UI. Nothing
/// here is audio-thread material: it contains strings, and it is built on a
/// worker thread.
struct AudioDeviceDescription {
    /// Stable, backend-qualified identifier, e.g. "null:silence" or
    /// "wasapi:{0.0.0.00000000}.{...}". Sessions persist this, so it must not
    /// be a display name and must not change when devices are re-enumerated in
    /// a different order.
    std::string id;

    /// Name for humans. Not unique -- two identical interfaces present the same
    /// name -- which is precisely why `id` exists separately.
    std::string name;

    int maxInputChannels = 0;
    int maxOutputChannels = 0;

    /// Rates the device will accept, in the order the backend reports them.
    /// Empty means "unknown, try and see", which is how some WASAPI shared-mode
    /// endpoints behave.
    std::vector<SampleRate> sampleRates;

    /// The device's own preferred block size, in frames. Honouring it avoids a
    /// resampling or re-blocking layer in the driver, which is latency we do
    /// not control.
    int defaultBufferFrames = 0;

    /// True for the system's current default endpoint. The manager uses this to
    /// fall back when a remembered device has gone away.
    bool isDefault = false;

    [[nodiscard]] bool hasInput() const noexcept { return maxInputChannels > 0; }

    [[nodiscard]] bool hasOutput() const noexcept { return maxOutputChannels > 0; }

    /// True if `rate` is listed. An empty rate list answers true for any valid
    /// rate: an unknown constraint must not be treated as a prohibition, or we
    /// reject devices that would have worked.
    [[nodiscard]] bool supportsSampleRate(SampleRate rate) const noexcept;

    /// The listed rate closest to `preferred`, compared in octaves rather than
    /// hertz so that 44100 prefers 48000 over 96000 the way a human would.
    /// Returns `preferred` unchanged when the rate list is empty.
    [[nodiscard]] SampleRate closestSampleRate(SampleRate preferred) const noexcept;
};

} // namespace sa::device
