#pragma once

#include <sa/analysis/Decibels.h>
#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>

#include <vector>

/// Energy in octave and third-octave bands.
///
/// The oldest way of describing a spectrum and still the one people talk in.
/// A thirty-one band display is the language of room correction, of noise
/// measurement, and of every conversation that contains the phrase "too much
/// at 200". An FFT answers a finer question and is harder to read: a bin is
/// 5.9 Hz wide everywhere, which is a hundredth of an octave at the top and
/// most of one at the bottom, so a flat noise spectrum slopes on a log axis
/// and a listener's "bass" spans four hundred bins.
///
/// Bands are integrated from the FFT rather than from a filter bank. The
/// difference matters and is worth stating: a filter bank is what IEC 61260
/// specifies and what a certified sound level meter contains, with tolerance
/// masks this does not claim to meet. Summing bins is exact for stationary
/// material, cheap, and free of the settling time a bank of steep filters has
/// -- which is the right trade for looking at a recording, and the wrong one
/// for certifying a measurement. Nothing here says "IEC 61260" and nothing
/// should until a bank exists and has been checked against the masks.
namespace sa::analysis {

/// One band's worth of answer.
struct Band {
    /// Nominal centre, as it is written on a display: 1000, 1250, 1600. These
    /// are the preferred numbers from the standard series, which are rounded,
    /// not the exact geometric centres.
    double centreHz = 0.0;
    /// The edges actually integrated, which are exact rather than rounded.
    double lowHz = 0.0;
    double highHz = 0.0;
    /// Energy in the band, in dBFS, corrected by the analysis window's noise
    /// bandwidth so that a full-scale sine inside one band reads 0 dBFS
    /// however many bins it covers -- and so that broadband noise reads its
    /// real power per band rather than a figure that depends on the transform
    /// size. One correction serves both; see noiseBandwidthBins.
    double levelDb = kDecibelFloor;
};

enum class BandWidth {
    /// Ten bands over the audio range, at 31.5 Hz to 16 kHz.
    Octave,
    /// Thirty-one bands, at 20 Hz to 20 kHz. The usual display.
    ThirdOctave,
};

struct OctaveBandSettings {
    BandWidth width = BandWidth::ThirdOctave;
    /// Transform size. 8192 gives 5.9 Hz bins at 48 kHz, which puts three bins
    /// in the narrowest third-octave band that matters (the 25 Hz band is
    /// 5.8 Hz wide, and is reported with a warning rather than silently).
    int fftSize = 8192;
};

/// The bands `settings` describes, before any audio is measured.
///
/// Exposed because a display needs the axis before it has anything to draw on
/// it, and because the band edges are worth being able to check directly.
[[nodiscard]] std::vector<Band> bandLayout(SampleRate rate,
                                           const OctaveBandSettings& settings = {});

/// Measure `audio` into bands.
///
/// The first channel only, as elsewhere: a spectrum of a stereo sum shows a
/// comb wherever the channels disagree in phase, which is a picture of the
/// summing rather than of the material.
[[nodiscard]] Result<std::vector<Band>> measureBands(ConstAudioBufferView audio, SampleRate rate,
                                                     const OctaveBandSettings& settings = {});

} // namespace sa::analysis
