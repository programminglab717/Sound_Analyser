#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>

namespace sa::dsp {

/// Putting back the peaks a converter or a recorder took off.
///
/// Clipping is the most common damage there is and the one people notice last:
/// a level set too high somewhere upstream flattens every peak against the
/// ceiling, and what arrives has square tops where it should have curves. The
/// audible result is a harsh edge on loud moments that no amount of gain riding
/// afterwards will remove, because the information is genuinely gone.
///
/// It cannot be recovered, only estimated -- but estimating it is exactly the
/// problem the declicker already solves. A flat top is a run of samples whose
/// true values are unknown; the model that knows what the signal was doing
/// either side can say what they most likely were. The only difference is that
/// here something is known about the answer: a clipped sample was *at least* as
/// large as the ceiling it hit, so a restored value that comes back smaller is
/// wrong by construction and is pushed back out.
///
/// **It will not fit in the file afterwards, and that is the point.** Peaks put
/// back above a ceiling the material was already touching go over full scale by
/// definition. Leaving that for the user to discover on export would make the
/// repair worse than useless, so by default the result is brought down to fit
/// and the gain applied is reported rather than hidden.
///
/// **Measured.** Material peaking at 0.976, hard clipped at a fraction of that
/// and then restored. "Shape" is the error against the undamaged original with
/// the level difference taken out, so it measures the waveform rather than the
/// gain:
///
///     clipped at   samples pinned   shape before   after    peak restored to
///        90%            1.9%          -19.3 dB   -51.0 dB        0.974
///        80%            6.1%          -12.7 dB   -40.0 dB        0.984
///        70%           12.9%           -8.7 dB   -40.1 dB        0.975
///        60%           22.3%           -5.8 dB   -26.1 dB        0.985
///        50%           32.6%           -3.5 dB   -21.2 dB        0.956
///        40%           44.9%           -1.7 dB   -18.3 dB        0.865
///
/// The figure to read is the last column: even with nearly half the samples
/// pinned, the peak comes back to within a decibel of where it started. What
/// degrades as the clipping deepens is the fine shape, which is exactly what
/// one would expect, because there is progressively less of it left to infer
/// from.
///
/// **Where it cannot work, and why.** If the model is long enough to span a
/// useful fraction of the material's own period, it stops treating the flat top
/// as damage and starts treating it as the shape -- because on periodic
/// material that is exactly what it is, repeated every cycle, with no unclipped
/// example anywhere for the model to learn the real peak from. Measured on
/// plain tones clipped at 70% of 0.5, restored peak by order:
///
///     tone     period      4      8     12     16     24     32     48
///      180 Hz   267     0.500  0.500  0.500  0.500  0.500  0.500  0.500
///      440 Hz   109     0.500  0.500  0.500  0.500  0.500  0.463  0.407
///     1000 Hz    48     0.500  0.500  0.500  0.405  0.350  0.350  0.350
///     2000 Hz    24     0.500  0.369  0.350  0.350  0.350  0.350  0.350
///     4000 Hz    12     0.356  0.350  0.350  0.350  0.350  0.350  0.350
///
/// The rule the table shows is that restoration holds while the order stays
/// under about a quarter of the period, and collapses once it passes it.
///
/// This sounds worse than it is, and the reason is worth stating: what saves
/// real material is that it is not periodic. Four tones between 1 and 3.7 kHz
/// -- every one of them individually past the limit above -- restore to within
/// 38 to 42 dB at *every* order from 4 to 48, because their sum does not repeat
/// and the model cannot learn a flat top that never looks the same twice. The
/// default order of 32 is chosen on that material, where it is the best of the
/// orders tried; a lower one is worse on it by up to 19 dB. A recording that is
/// genuinely a single sustained tone, clipped, is the case this cannot help
/// with, and it reports restoring nothing rather than pretending.
struct DeclipSettings {
    /// How close to the loudest sample counts as "at the ceiling", as a
    /// fraction. The default is a thousandth, which is 0.009 dB -- tight
    /// enough that ordinary peaks are not caught and loose enough to survive
    /// a file that was clipped and then converted through 16-bit.
    double tolerance = 0.001;

    /// Shortest run of samples at the ceiling that counts as clipping. One
    /// sample at the peak is just the peak; two in a row already is not, for
    /// anything but a synthetic square wave.
    int minimumRun = 2;

    /// How far above the ceiling a restoration has to reach before it is kept,
    /// as a fraction of the ceiling.
    ///
    /// This is what makes the detection trustworthy rather than merely
    /// plausible, and it is worth explaining. Flatness alone does not separate
    /// a clipped peak from an ordinary one: near its own crest a sine is
    /// genuinely flat, and the lower the note the flatter it is. At 48 kHz a
    /// 50 Hz tone changes by two parts in a hundred thousand from one sample to
    /// the next at the top of its arc, which is flatter than most real clipping
    /// survives a conversion to 16-bit. No threshold on flatness can tell those
    /// apart, because they are not different.
    ///
    /// What *is* different is what the model says should have been there. Run
    /// the restoration and ask how high it reached: a clipped peak was cut off,
    /// so the signal either side implies something well above the ceiling,
    /// while an ordinary crest implies almost exactly what is already there --
    /// the model fits it, because there is nothing wrong with it. Anything that
    /// does not reach this far above the ceiling is put back as it was and not
    /// counted, which costs only the restoration of peaks that were barely
    /// clipped at all, and those are the ones nobody can hear.
    double minimumRecovery = 0.005;

    /// Longest run that will be restored. A long flat top is not a clipped
    /// peak, it is a passage driven so hard there is no shape left to infer,
    /// and a model asked to fill a thousand samples composes rather than
    /// restores.
    int maximumRun = 512;

    /// Poles in the model, and how often it is refitted. As in Declick.
    int order = 32;
    int blockSize = 4096;

    /// Bring the result down so the restored peaks fit.
    ///
    /// On by default because the alternative is a file that clips on export,
    /// which is the fault this was meant to repair. Turn it off where the
    /// caller is going to limit or normalise afterwards anyway and wants the
    /// restored shape at its own level.
    bool fitToCeiling = true;

    /// Where `fitToCeiling` puts the loudest restored peak, in dBFS.
    double ceilingDb = -0.1;
};

struct DeclipReport {
    /// Flat tops found and restored.
    int runs = 0;
    SampleCount samplesRestored = 0;
    SampleCount longestRun = 0;
    /// Flat tops left alone for being longer than `maximumRun`.
    int tooLong = 0;
    /// Runs the model could not resolve, which happens at the very start or
    /// end of a file where there is not enough either side to interpolate
    /// from.
    int unsolved = 0;

    /// The level the clipping was detected at, per channel's loudest sample.
    /// Zero where nothing was found.
    double detectedCeiling = 0.0;

    /// Highest sample in the result before any fitting, as a linear value.
    /// Above 1.0 means the restoration genuinely went over full scale.
    double restoredPeak = 0.0;

    /// Gain applied to fit the result, in dB. Zero or negative; zero means
    /// nothing needed doing or `fitToCeiling` was off.
    double gainDb = 0.0;
};

/// Restore clipped peaks in `audio`, in place.
[[nodiscard]] Result<DeclipReport> declip(AudioBufferView audio,
                                          const DeclipSettings& settings = {});

} // namespace sa::dsp
