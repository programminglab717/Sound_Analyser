#pragma once

#include <sa/analysis/Decibels.h>
#include <sa/analysis/LoudnessMeter.h>
#include <sa/analysis/TruePeakMeter.h>
#include <sa/core/AudioBuffer.h>
#include <sa/core/ChannelLayout.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace sa::analysis {

/// Loudness over time, and the dynamics figures that fall out of it.
///
/// LoudnessMeter answers "how loud is this programme". This answers "how loud
/// is it *here*". An integrated figure says a master sits at -14 LUFS; the
/// contour says whether it sits there throughout or whether one loud chorus
/// carries the average while the verses run 6 dB below. The mastering decision
/// is made on the second, and the first cannot be made to yield it.
///
/// **There is no second BS.1770 implementation here.** The contour drives a
/// LoudnessMeter one sub-block at a time and records what it reads, so the
/// K-weighting, the block grid, the channel weighting and both gates are that
/// meter's. The integrated figure returned below is bit-identical to
/// LoudnessMeter::measure() on the same audio, because it is that meter's
/// figure, produced from the same samples in the same order -- the meter
/// guarantees its accumulators do not care how the input is chopped up.
///
/// **Conformance caveat, inherited whole.** Nothing here has been run against
/// the official EBU R128 / ITU-R BS.1770 vector set -- those files are not in
/// the repository -- and nothing here claims R128 conformance; see
/// LoudnessMeter's own caveat and docs/03-architecture.md §9. PSR and PLR are
/// not standardised at all. They are industry practice, implementations differ
/// over which windows they use, and the windows this module uses are stated
/// field by field below so that a reading is at least reproducible.

/// One instant of the contour: every reading that is defined there.
///
/// Readings are optional because a window either exists or it does not, and
/// those two cases must not be confusable. kDecibelFloor cannot carry the
/// distinction: once 400 ms have gone by, a momentary reading at the floor *is*
/// a measurement -- it says that window was silent -- while before 400 ms have
/// gone by there is no window and no number of any kind. So an engaged optional
/// means "measured", and its value may still be the floor; std::nullopt means
/// "not defined here", and cannot be read as a level by accident. A short-term
/// reading taken over half a second is not a short-term reading, and this is
/// what stops one being reported as though it were.
struct LoudnessPoint {
    /// When this reading was taken. Every window below *ends* at or just before
    /// it and runs backwards from there, which is the only attribution that
    /// does not require knowing the future -- and is how a meter is read.
    ///
    /// "Just before" because BS.1770's blocks advance on a 100 ms grid: a point
    /// sampled between two boundaries carries the value from the earlier one,
    /// exactly as a meter's display holds its last value. On the default
    /// interval every point lands on a boundary and the two coincide.
    ///
    /// Quantised to a whole sample, so the grid cannot drift across a long
    /// programme.
    double timeSeconds = 0.0;

    /// Loudness of the most recent 400 ms. Defined from 400 ms in.
    std::optional<double> momentaryLufs;

    /// Loudness of the most recent 3 s. Defined from 3 s in.
    std::optional<double> shortTermLufs;

    /// True peak over the same 3 s the short-term value covers -- not the peak
    /// at this instant, which would be a different measurement with a different
    /// use. It shares the short-term window so that psrDb below is a difference
    /// between two figures describing the same stretch of audio rather than two
    /// unrelated ones.
    ///
    /// Measured with the streaming TruePeakMeter, which under-reads on material
    /// with energy near Nyquist -- see its header for how much. There is no
    /// per-window equivalent of exactTruePeakDbtp() that would not re-transform
    /// every window.
    std::optional<double> truePeakDbtp;

    /// Peak to short-term loudness ratio: truePeakDbtp - shortTermLufs.
    ///
    /// This is the one that shows where a master has been squashed. Limiting
    /// raises loudness without moving the ceiling, so PSR falls wherever the
    /// limiter was working; a passage that reads 4 dB where the rest of the
    /// record reads 12 is a passage that was flattened, and no single-figure
    /// summary of the programme will say so.
    ///
    /// Empty where either operand is undefined, including where the window was
    /// silent: floor minus floor is zero, which would read as a perfectly
    /// limited passage rather than as no measurement.
    std::optional<double> psrDb;

    /// Sample peak over RMS across the same 400 ms the momentary value covers,
    /// in dB. Pooled over every channel, which is the definition
    /// SignalStatistics already uses.
    ///
    /// Sample peak, not true peak, and deliberately: a square wave's crest
    /// factor is 0 dB, but its reconstruction overshoots between samples, so a
    /// true-peak crest would not be 0 dB and would not be the figure anyone
    /// means by the term.
    std::optional<double> crestDb;
};

/// A contour plus the whole-programme figures it is read against.
struct LoudnessContour {
    /// One per sampling instant, in time order, starting at 0. The first
    /// several carry no loudness readings at all, which is not padding -- it is
    /// the part of the programme where the standard defines nothing.
    std::vector<LoudnessPoint> points;

    /// Spacing actually used. Rounded to a whole sample from the requested
    /// interval, so it may differ in the last decimal place from what was
    /// asked for.
    double intervalSeconds = 0.0;

    /// Copied verbatim from the driving meter -- the same number
    /// LoudnessMeter::measure() returns for this audio, not a recomputation.
    double integratedLufs = kDecibelFloor;
    /// EBU Tech 3342 loudness range, also the driving meter's. Note this is
    /// *not* the spread of the percentiles below: LRA gates at -20 LU relative
    /// first and samples short-term loudness once a second.
    double loudnessRangeLu = 0.0;
    /// 400 ms blocks that cleared the absolute gate. Zero means integratedLufs
    /// is the floor because nothing measurable was fed rather than because the
    /// programme is quiet -- the same disambiguation LoudnessMeasurement makes.
    std::int64_t gatedBlockCount = 0;

    /// Whole-programme true peak, from the same streaming meter as the
    /// per-point values, so that plrDb and the psrDb series are comparable. For
    /// a figure to quote in a compliance check, call exactTruePeakDbtp(), which
    /// reconstructs rather than interpolates and may read higher.
    double truePeakDbtp = kDecibelFloor;

    /// Peak to loudness ratio: truePeakDbtp - integratedLufs. The headroom that
    /// survives loudness normalisation. Empty when either operand is undefined.
    std::optional<double> plrDb;

    /// Extremes and percentiles of the short-term readings in `points`.
    ///
    /// Taken over the points that carry a short-term value *above the absolute
    /// gate*. Silence is not a quiet passage: a single second of digital black
    /// at the head of a file would otherwise be the quietest short-term value
    /// of every programme that has one, and -70 LUFS is already where BS.1770
    /// stops treating audio as programme material.
    ///
    /// These are computed over this contour, so they depend on
    /// intervalSeconds -- a coarse interval can step over a brief extreme.
    /// Empty when no point qualifies.
    std::optional<double> quietestShortTermLufs;
    std::optional<double> loudestShortTermLufs;
    /// The 10th and 95th percentiles of that same set, nearest-rank. The pair
    /// EBU Tech 3342 uses for LRA, so their spread is a familiar quantity --
    /// but ungated, so it is a plainer statement of "how consistent is this"
    /// and is not LRA. Use loudnessRangeLu for that.
    std::optional<double> shortTermPercentile10Lufs;
    std::optional<double> shortTermPercentile95Lufs;
};

/// Default contour spacing: one 100 ms sub-block, the grid BS.1770's blocks
/// already advance on. Asking for less resamples the same staircase.
inline constexpr double kDefaultContourIntervalSeconds = LoudnessMeter::kSubBlockSeconds;

/// Measure the contour of `audio`, sampling every `intervalSeconds`.
///
/// Fails for an unusable rate, an empty buffer, a buffer whose channel count
/// does not match `layout`, a non-positive interval, an interval shorter than
/// one sample, or audio shorter than one 400 ms block -- below which the
/// contour would consist entirely of points with nothing in them.
///
/// Readings change only on the 100 ms sub-block boundary, because that is where
/// BS.1770's blocks advance. An interval finer than that samples the same
/// staircase more densely; it does not interpolate between the steps, because
/// there is nothing defined between them.
[[nodiscard]] Result<LoudnessContour>
measureLoudnessContour(ConstAudioBufferView audio, SampleRate rate, const ChannelLayout& layout,
                       double intervalSeconds = kDefaultContourIntervalSeconds,
                       int oversampling = TruePeakMeter::kDefaultOversampling);

/// As above, with the layout inferred from the channel count: 1 is mono, 2 is
/// stereo, anything else is discrete.
///
/// Discrete rather than a guess at 5.1, because BS.1770's surround weighting
/// applies to channels *known* to be surrounds and a six-channel buffer is not
/// necessarily a 5.1 mix. Guessing wrong would either weight an ordinary
/// channel at 1.41 or drop one as an LFE, both of which change the answer on
/// material we cannot identify. Full weight on every channel is the only
/// assumption that cannot silently discard signal; callers that know the
/// layout should pass it.
[[nodiscard]] Result<LoudnessContour>
measureLoudnessContour(ConstAudioBufferView audio, SampleRate rate,
                       double intervalSeconds = kDefaultContourIntervalSeconds);

} // namespace sa::analysis
