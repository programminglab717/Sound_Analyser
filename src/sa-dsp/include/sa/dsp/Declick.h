#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>

namespace sa::dsp {

/// Finding and repairing clicks: the other half of restoration.
///
/// Broadband denoise takes out what is there all the time. This takes out what
/// is there for a moment and should not be -- a scratch, a splice, a dropped
/// packet, a bad edit, the tick a vinyl transfer is made of. The tool a user
/// reaches for after denoise, and the one a long archival job needs most,
/// because a record can have thousands of clicks and nobody is drawing a box
/// round each one by hand.
///
/// **How it decides.** A short linear-prediction model describes what the
/// signal has been doing; anything the signal's own recent history does not
/// explain shows up as a spike in the residual. That alone would be a crude
/// detector, because a click rings through the model for as many samples as it
/// has poles, so the residual is large for a stretch after every click and the
/// repair would eat good audio. Running the model backwards as well gives a
/// residual that rings *before* the click instead, and where both are large at
/// once is the click and nothing else. The two together are what keep a repair
/// the length of the damage.
///
/// **How it repairs.** The same model, used to answer a different question:
/// given the samples either side, which values for the damaged ones would the
/// model find least surprising? That is a least-squares problem with a closed
/// form and it is solved exactly, not iterated. A repair therefore continues a
/// held note in phase and a noise floor with the right spectrum, rather than
/// drawing a line across the gap.
///
/// **What it is not for.** A gap longer than `maximumGap` is not a click, it is
/// a dropout, and extrapolating a hundred milliseconds from a 32-pole model
/// invents rather than reconstructs. Those are counted and left alone.
///
/// **Measured.** On synthetic material with forty-three clicks in four seconds
/// it takes the error against the undamaged original down by 32 dB, and on the
/// same material undamaged it returns the samples bit-identical. A minute of
/// 48 kHz stereo takes about half a second, and finds nothing in it.
struct DeclickSettings {
    /// Poles in the model. 32 describes a spectral envelope in enough detail
    /// for music without following the signal so closely that a click starts
    /// to look like part of it.
    int order = 32;

    /// How often the model is refitted, in samples. The model has to hold still
    /// over the block, so this trades off against how fast the material
    /// changes: 4096 is about 85 ms at 48 kHz, short enough for speech and
    /// most music.
    int blockSize = 4096;

    /// How far above the passage's own residual a sample has to be before it
    /// counts as damage, in robust standard deviations. The scale is a median,
    /// not a mean, so the clicks themselves do not inflate the threshold they
    /// are measured against.
    ///
    /// Lower finds more and repairs more that did not need it. Measured on
    /// clean material, 5 finds nothing in a tone, in noise or in a mixture;
    /// at 3 it starts to find the loudest few percent of ordinary noise.
    double threshold = 5.0;

    /// The longest run of samples that will be repaired. In samples rather
    /// than milliseconds because everything here is, but the figure worth
    /// knowing is that 64 samples is 1.3 ms at 48 kHz, which is already a
    /// large click.
    int maximumGap = 64;

    /// Samples added either side of what was detected. A click's very edges
    /// are often a sample or two below the threshold while still being wrong,
    /// and repairing one sample too many costs nothing.
    int guard = 1;

    /// Detections closer together than this are treated as one piece of
    /// damage.
    ///
    /// Damage is rarely a single clean run of surprising samples. A four-sample
    /// click can easily have a sample in the middle that the model happens to
    /// find plausible, and a burst of noise is surprising in patches rather
    /// than throughout. Without merging, both come out as a scatter of tiny
    /// repairs: the click is only partly mended, and -- worse -- a long dropout
    /// is quietly repaired in forty little pieces instead of being recognised
    /// as too long to repair at all. Both were measured before this existed.
    ///
    /// Merging two genuinely separate clicks that happen to be this close is
    /// not a loss: repairing them as one piece is what a person would do.
    int mergeDistance = 8;
};

/// Longest repair the solver will accept, whatever the settings say. The solve
/// is cubic in the gap and a gap this long is not a click.
inline constexpr int kMaximumDeclickGap = 512;

struct DeclickReport {
    /// Runs of damage found and repaired.
    int clicks = 0;
    SampleCount samplesRepaired = 0;
    SampleCount longestRepair = 0;
    /// Runs found but left alone for being longer than `maximumGap`. A large
    /// count here means the threshold is too low or the material is not what
    /// this tool is for -- either way it is worth surfacing rather than
    /// silently not doing anything.
    int tooLong = 0;
    /// Runs the solver could not resolve, which happens where the surrounding
    /// audio is silent and there is no model to interpolate with.
    int unsolved = 0;
};

/// Find and repair clicks in `audio`, in place. Each channel is treated
/// separately, because a click on a vinyl transfer is usually on one of them.
[[nodiscard]] Result<DeclickReport> declick(AudioBufferView audio,
                                            const DeclickSettings& settings = {});

} // namespace sa::dsp
