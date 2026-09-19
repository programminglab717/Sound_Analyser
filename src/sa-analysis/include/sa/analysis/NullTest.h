#pragma once

#include <sa/analysis/Decibels.h>
#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>

#include <vector>

/// Subtract one recording from another and describe what is left.
///
/// The forensic question, and the only one this answers: did something change
/// the audio, and if so by how much and where. Two recordings that are meant
/// to be the same are aligned, gain-matched, subtracted, and what survives is
/// the answer. Nothing else settles the argument the same way -- a spectrum of
/// each can be compared by eye and a level meter can be read off both, but
/// only the residual is evidence, because there is nothing left to interpret
/// when the difference is silence.
///
/// What it does not claim. The alignment is a whole number of samples and the
/// gain is one figure for the whole comparison, so anything that varies over
/// time -- a drifting clock, a fader move, a compressor -- will not null
/// however right those two numbers are. Neither will a fractional-sample
/// offset or a difference in group delay: an all-pass filter changes no
/// magnitude at any frequency and can still leave a residual louder than the
/// material it came from. A loud residual is therefore evidence of *a*
/// difference and not evidence that anything was damaged, which is why where
/// it sits in time and in frequency is reported alongside how big it is.
///
/// It is also not a perceptual measure. Nothing here is weighted, gated or
/// masked, so a residual sixty decibels down may be inaudible or may be one
/// obvious click, and the two read the same.
namespace sa::analysis {

/// Where a residual stops being a difference and becomes arithmetic.
///
/// float32 carries a half-ulp of 2^-24, which is 144 dB below whatever sample
/// it is attached to, so a residual that is only the rounding of a gain change
/// and a subtraction sits below -140 dB relative to the material. A genuine
/// difference of one step at 24 bits sits near -138 dBFS, which on programme
/// material peaking well below full scale is somewhere above -120 dB relative
/// to it. The line is drawn where nothing that is merely float rounding can
/// reach it and nothing that is a real difference at 24 bits falls below it.
inline constexpr double kNullFloorDb = -120.0;

/// The answer in one word.
enum class NullVerdict {
    /// The two channels are the same bytes. Checked, not measured, and the
    /// only one of the three that is a statement about the files rather than
    /// about a comparison of them.
    BitIdentical,

    /// Not the same bytes, but what is left after aligning and gain-matching
    /// is below kNullFloorDb. The two carry the same audio; they differ by no
    /// more than the arithmetic used to get here.
    WithinFloatFloor,

    /// A difference that is really there. How much it matters is not a
    /// question this answers.
    Different,
};

/// How much of the reference survives in one octave band.
///
/// The bands are the octave layout of OctaveBands.h, measured the same way, so
/// a band here and a band on a spectrum display mean the same thing.
struct NullBand {
    /// Nominal centre, as it is written on a report: 125, 250, 500.
    double centreHz = 0.0;

    /// The aligned reference's level in the band, in dBFS.
    double referenceDb = kDecibelFloor;

    /// The residual's level in the same band, in dBFS.
    double residualDb = kDecibelFloor;

    /// residualDb - referenceDb: how far down the difference is *in this
    /// band*. The useful column, and the reason the other two are kept.
    /// A chain that only touched the top octave shows it here and nowhere
    /// else, while the overall residual figure would only say that something
    /// happened.
    double relativeDb = kDecibelFloor;
};

struct NullSettings {
    /// Search for the sample offset between the two before comparing.
    ///
    /// Turning it off is an assertion rather than an optimisation: with it off
    /// a copy that is one sample late reports a residual louder than the
    /// material instead of a delay of one, which is the right answer to the
    /// question "is this the same file" and the wrong one to "is this the same
    /// audio".
    bool alignDelay = true;

    /// Scale `other` to best fit `reference` before subtracting.
    ///
    /// Off asks the narrower question, where a level change is itself the
    /// difference being looked for. The level difference is measured either
    /// way; this decides only whether the subtraction corrects for it.
    bool matchGain = true;

    /// Largest offset to consider, in samples either way. Zero -- or anything
    /// negative -- considers every offset at which the two overlap at all.
    ///
    /// Two reasons to bound it. Periodic material correlates at every period,
    /// so an unbounded search over a loop can settle a bar away from the truth
    /// and null just as well there; and the transform is sized by the range
    /// searched, so an unbounded search across two long recordings is the
    /// expensive case rather than the safe default it looks like.
    SampleCount maxDelaySamples = 0;
};

struct NullResult {
    /// How much later `other` is than `reference`, in samples. Negative when
    /// the second recording starts earlier. Zero when alignment is off.
    SampleIndex delaySamples = 0;

    /// The level of `other` relative to `reference`, in decibels, so a copy at
    /// half amplitude reads 20*log10(0.5) = -6.02 dB.
    ///
    /// This is the least-squares fit <other, reference> / <reference,
    /// reference>: how much of the reference is present in the second
    /// recording, and at what level. It sits at the decibel floor when none of
    /// it is, which is what two unrelated recordings read.
    ///
    /// The scale actually applied before subtracting is the matching fit the
    /// other way round, <reference, other> / <other, other>, which is the
    /// single number that minimises the residual. The two are reciprocals
    /// exactly when the second recording really is a scaled copy of the first
    /// -- the case this figure is for -- and where they part company they part
    /// by how much the two have in common at all.
    ///
    /// Measured whether or not it was applied: NullSettings::matchGain decides
    /// only whether the subtraction corrects for it.
    double gainDb = 0.0;

    /// True when that least-squares fit came out negative, which is one of the
    /// two with its polarity flipped.
    ///
    /// Separate from gainDb because gainDb is a magnitude and would hide it:
    /// an inverted copy gain-matches at 0.00 dB and then nulls perfectly, so
    /// without this flag the report would read exactly like two identical
    /// files.
    bool polarityInverted = false;

    /// The residual's RMS relative to the reference's RMS over the compared
    /// region, in decibels.
    ///
    /// The headline. The floor is a perfect null, -60 is a difference a
    /// thousandth the size of the material, 0 dB is a difference as big as it.
    double residualDb = kDecibelFloor;

    /// The loudest single sample of the residual, in dBFS -- absolute, not
    /// relative to anything.
    ///
    /// A ratio against the reference's peak would be a ratio between two
    /// moments with nothing to do with each other, since the reference peaks
    /// where the music is loud and the residual peaks where the difference is.
    double peakResidualDb = kDecibelFloor;

    /// When that sample happens, in seconds on the reference's timeline.
    ///
    /// One sample, so it points at the worst moment rather than describing it.
    /// A single click and a difference spread over a minute can give the same
    /// figure, and only the first is a place worth looking; the band
    /// breakdown and residualDb are what tell the two apart.
    double worstTimeSeconds = 0.0;

    /// How many frames the two had in common once aligned.
    SampleCount comparedFrames = 0;

    /// Where the difference sits in frequency. Every band of the layout is
    /// present; bands read the decibel floor when the overlap was shorter than
    /// one analysis window.
    std::vector<NullBand> bands;

    NullVerdict verdict = NullVerdict::Different;

    /// Every byte of the two channels equal, compared before anything is
    /// aligned or scaled -- a delayed or rescaled copy nulls, and is still not
    /// the same recording.
    bool bitIdentical = false;

    /// False when the reference had no energy in the compared region and the
    /// other did, so there is nothing for the residual to be relative to and
    /// none of the decibel figures mean anything. Two silences are a valid
    /// perfect null, not an invalid one.
    bool valid = false;
};

/// Compare one channel of two recordings.
///
/// One channel, as elsewhere in this module: a null test of a stereo sum would
/// hide a difference that exists only in the side signal, which is precisely
/// the difference a processing chain is most likely to have introduced.
[[nodiscard]] Result<NullResult> nullTest(ConstAudioBufferView reference,
                                          ConstAudioBufferView other, SampleRate rate,
                                          const NullSettings& settings = {}, int channel = 0);

} // namespace sa::analysis
