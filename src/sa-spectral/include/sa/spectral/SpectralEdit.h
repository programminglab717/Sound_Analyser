#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/dsp/Window.h>

namespace sa::spectral {

/// A time-frequency rectangle: what the user drew on the spectrogram.
struct SpectralRegion {
    SampleIndex startSample = 0;
    SampleIndex endSample = 0;
    double lowHz = 0.0;
    double highHz = 0.0;

    [[nodiscard]] bool isEmpty() const noexcept {
        return endSample <= startSample || highHz <= lowHz;
    }
};

/// Analysis settings for a spectral edit, and the edges it leaves behind.
///
/// Feathering is not decoration. A rectangular hole in a spectrogram is
/// audible: the abrupt frequency edge rings, and the abrupt time edge clicks.
/// Tapering the mask at both spreads the transition over enough bins and frames
/// that the ear stops hearing an event and starts hearing an absence.
struct SpectralEditSettings {
    int fftSize = 4096;
    int hopSize = 1024;
    dsp::WindowType window = dsp::WindowType::Hann;

    /// Width of the taper at each frequency edge, in Hz. Zero picks three bins,
    /// which is the narrowest that does not ring for a Hann window.
    double frequencyFeatherHz = 0.0;

    /// Width of the taper at each time edge, in samples. Zero picks one hop.
    SampleCount timeFeather = 0;
};

/// Attenuate everything inside `region` by `decibels`.
///
/// Negative attenuates; -120 or below removes. This is the workhorse: a mains
/// hum, a squeak, a siren bleeding through a take. It edits `audio` in place and
/// touches samples outside the region only where the analysis windows overlap
/// it, which is unavoidable and is what the feathering is sized against.
[[nodiscard]] Status attenuateRegion(AudioBufferView audio, SampleRate rate,
                                     const SpectralRegion& region, double decibels,
                                     const SpectralEditSettings& settings = {});

/// Replace `region` with content interpolated across it in time.
///
/// The repair operation: a click, a cough, a chair scrape. Magnitude is
/// interpolated between the frames on either side and phase is advanced from
/// the left edge at each bin's own frequency, so a steady tone running through
/// the gap continues in phase rather than restarting.
///
/// It reconstructs, it does not invent. Where the neighbours carry nothing --
/// a gap wider than the event, or a region at the very start -- the result
/// fades toward silence rather than inventing material, because a plausible
/// fabrication in a repair tool is worse than an audible absence.
[[nodiscard]] Status healRegion(AudioBufferView audio, SampleRate rate,
                                const SpectralRegion& region,
                                const SpectralEditSettings& settings = {});

} // namespace sa::spectral
