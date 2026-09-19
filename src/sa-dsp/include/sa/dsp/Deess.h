#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>

/// Taking the edge off sibilance.
///
/// "S" and "sh" carry most of their energy between about four and ten
/// kilohertz, and a chain that suits the rest of a voice -- a close microphone,
/// a compressor, a high shelf for air -- makes them worse at every step. The
/// result is a recording that is comfortable everywhere except on the letter s,
/// where it is painful, and turning the whole thing down does not help because
/// everything else was already right.
///
/// What works is a compressor that only listens to, and only acts on, the band
/// the sibilance is in. That is what this is: the signal is split, the high
/// band is compressed against a detector fed from the high band alone, and the
/// two are added back together. The low band is not touched at all, which is
/// the whole point -- the vowels come through exactly as they went in, and can
/// be checked to the sample.
///
/// Split-band rather than the other common approach, a dynamic notch swept to
/// the loudest sibilant frequency. The notch is more surgical when it lands and
/// worse when it does not, and deciding where it should land is a pitch
/// detection problem on a signal that has no pitch. A band is the duller
/// instrument and the one that does not fail strangely.
namespace sa::dsp {

struct DeessSettings {
    /// Where sibilance is taken to start. Lower catches more of a bright voice
    /// and starts eating consonants like "t"; higher misses the low end of a
    /// deep one.
    double frequencyHz = 5000.0;

    /// Level above which the high band is compressed, in dBFS. The control
    /// that actually decides how much happens.
    double thresholdDb = -30.0;

    /// How hard. 4:1 is gentle, 10:1 is firm, and above about 20 it is a
    /// limiter on the band.
    double ratio = 6.0;

    /// Fast, because a sibilant is 50 to 150 ms long and an attack slower than
    /// that lets the front of it through untouched, which is the part that
    /// hurts.
    double attackSeconds = 0.001;

    /// Short, so the band comes back before the next vowel, but not so short
    /// that the gain moves inside one sibilant and modulates it.
    double releaseSeconds = 0.040;

    /// Most it will ever pull the band down, in dB. A stop rather than a
    /// setting: past about 12 dB a de-esser stops removing sibilance and starts
    /// removing the consonant, which is worse than the problem.
    double maximumReductionDb = 12.0;
};

/// What it did, so a caller can say something more useful than "done".
struct DeessReport {
    /// Most the high band was pulled down at any moment, in dB.
    double peakReductionDb = 0.0;
    /// Fraction of the material where the band was reduced by more than a
    /// decibel. A figure near zero on a recording with obvious sibilance means
    /// the threshold is too high; a figure near one means it is too low and the
    /// whole top end is being compressed.
    double fractionReduced = 0.0;
};

/// De-ess `audio` in place.
///
/// `runUp` frames at the start are processed to settle the envelope and are
/// then the caller's to discard, as with the other offline dynamics.
[[nodiscard]] Result<DeessReport> deess(AudioBufferView audio, SampleRate rate,
                                        const DeessSettings& settings = {}, SampleCount runUp = 0);

} // namespace sa::dsp
