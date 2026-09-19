#pragma once

#include <sa/analysis/Decibels.h>
#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>

#include <vector>

/// What a room does to a sound, measured from its impulse response.
///
/// All of these are defined in ISO 3382, and all of them are derived from one
/// curve: the Schroeder integral, which is the energy still to come at each
/// moment rather than the energy at it. An impulse response is noisy sample by
/// sample -- it is noise, shaped -- so reading a decay slope off it directly
/// gives a different answer every time. Integrating backwards from the end
/// turns that into a smooth monotonic curve whose slope is the decay rate, and
/// that is the whole trick.
///
/// The measures are not claimed to be ISO 3382 *conformant*, for the same
/// reason nothing here claims BS.1770 conformance: the definitions are
/// implemented from their arithmetic, and no certified reference material has
/// been run against them. What is claimed is that on a synthetic decay whose
/// rate is known exactly, they recover that rate.
namespace sa::analysis {

struct RoomAcoustics {
    /// Early decay time: the 0 to -10 dB slope, extrapolated to 60 dB.
    ///
    /// Reported separately from the reverberation times because it is a
    /// different thing about the room. T20 and T30 describe the late tail,
    /// which is roughly exponential; EDT describes the first 10 dB, which is
    /// what a listener actually hears as reverberance. A room where they
    /// disagree is a room with strong early reflections, and that disagreement
    /// is the useful signal rather than an error.
    double earlyDecaySeconds = 0.0;

    /// Reverberation time from the -5 to -25 dB slope, extrapolated x3.
    ///
    /// T20 rather than a literal T60 because a genuine 60 dB of clean decay
    /// needs an impulse 60 dB above the room's noise floor, which almost no
    /// real measurement has. Starting at -5 skips the direct sound and the
    /// first reflections, which are not part of the exponential tail.
    double t20Seconds = 0.0;

    /// The same from -5 to -35 dB, extrapolated x2. Uses more of the decay and
    /// so is the better estimate when the measurement has the range for it.
    double t30Seconds = 0.0;

    /// Clarity: early energy over late energy, in decibels, split at 50 ms for
    /// speech and 80 ms for music. Positive means the direct sound and early
    /// reflections dominate, which is a room speech is intelligible in.
    double clarity50Db = kDecibelFloor;
    double clarity80Db = kDecibelFloor;

    /// Definition: the fraction of the energy arriving in the first 50 ms,
    /// between 0 and 1. The same information as C50 on a linear scale, and the
    /// one speech-intelligibility work is usually written in terms of.
    double definition50 = 0.0;

    /// Centre time: the centre of gravity of the energy, in seconds. A single
    /// number for "how smeared", with no arbitrary split point in it.
    double centreTimeSeconds = 0.0;

    /// How far the usable decay actually went, in dB below the peak. A
    /// measurement that only fell 22 dB before hitting its own noise floor can
    /// support T20 and cannot support T30, and this is what says so.
    double usableRangeDb = 0.0;

    /// Whether each figure had the decay range it needs. A value whose flag is
    /// false was not measurable and is left at zero rather than extrapolated
    /// from noise.
    bool hasEarlyDecay = false;
    bool hasT20 = false;
    bool hasT30 = false;

    /// Where the impulse actually starts, in samples from the beginning.
    /// Everything before it is discarded: silence ahead of the direct sound
    /// would otherwise count as early energy and flatter every measure.
    SampleIndex directSound = 0;

    /// False when the input was not an impulse response at all -- too short,
    /// silent, or with no discernible decay.
    bool valid = false;
};

/// Measure one channel of an impulse response.
[[nodiscard]] Result<RoomAcoustics> measureRoomAcoustics(ConstAudioBufferView impulse,
                                                         SampleRate rate, int channel = 0);

/// The backward-integrated decay curve, in dB relative to its own start.
///
/// Exposed because it is the thing to plot: every measure above is a slope or
/// a ratio taken from it, and a reading that looks wrong is usually obvious
/// from the curve. Index i is the energy remaining from sample i onward.
[[nodiscard]] Result<std::vector<float>> schroederCurveDb(ConstAudioBufferView impulse,
                                                          int channel = 0);

} // namespace sa::analysis
