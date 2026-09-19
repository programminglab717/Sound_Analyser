#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/dsp/Window.h>

namespace sa::spectral {

/// How much of the late tail to take out, and how hard to hold the result up.
///
/// The defaults describe a small live room -- a meeting room, a bedroom used as
/// a booth -- because that is what a dialogue take that cannot be re-recorded
/// was usually made in.
struct DereverbSettings {
    /// How much of the estimated late energy to remove, in dB.
    ///
    /// A strength, not an output level: 10 dB removes 90% of the estimated late
    /// *power*, 20 dB removes 99%, and 0 removes none and is an exact no-op.
    /// What that does to any given bin depends on how much of that bin was late
    /// energy in the first place, which is the point -- a bin carrying only
    /// direct sound is not touched however high this goes.
    double reductionDb = 10.0;

    /// The reverberation time assumed for the room, in seconds.
    ///
    /// T60: the time the room takes to fall 60 dB. It is the only thing the
    /// estimate knows about the room, so it is the setting that matters. Too
    /// short and the tail is under-estimated and survives; too long and the
    /// estimate exceeds the tail and starts eating sustained material.
    double decaySeconds = 0.4;

    /// How far below the input a bin may be pushed, in dB. Must not be
    /// positive.
    ///
    /// The difference between a de-reverb and a gate. Without it a bin whose
    /// whole content is estimated to be tail goes to silence, and bins
    /// switching on and off frame by frame is the warble that spectral
    /// subtraction is known for. Dropping it to -60 dB was measured to remove
    /// *less* reverberation than the default does, and with 80% more
    /// frame-to-frame movement in the gain: letting a bin fall to nothing does
    /// not take out more of the room, it takes out the same amount of it more
    /// raggedly.
    double floorDb = -20.0;

    /// Where the late tail is taken to begin, in seconds after the direct
    /// sound. Zero picks two analysis windows, which is 85 ms at the defaults
    /// and 48 kHz.
    ///
    /// The early reflections before this point are left alone deliberately:
    /// they fuse with the direct sound rather than smearing it, and removing
    /// them is what makes a de-reverb sound like a telephone. This is also the
    /// setting that decides how much of genuinely dry material comes off with
    /// the room, because a source's own decay inside the window looks exactly
    /// like a tail. Measured on dry speech-like material at 48 kHz:
    ///
    ///     onset    dry level   dry residual   T20 change   C50 change
    ///     43 ms     -1.60 dB       -17.4 dB       -2.4%      +4.9 dB
    ///     85 ms     -0.55 dB       -21.0 dB      -12.7%      +2.5 dB
    ///     128 ms    -0.25 dB       -26.5 dB      -24.4%      +1.0 dB
    ///
    /// Two windows is the default because a de-reverb is reached for on
    /// material that may turn out not to need it, and a 1.6 dB bite out of a
    /// take that was dry all along is worse than leaving some room in one that
    /// was not. It is also past the 80 ms that C80 splits at, so what comes off
    /// is late by any of the conventional definitions.
    ///
    /// One window is the shortest split the estimate can use at all -- the
    /// frame it looks back at has to be clear of the frame it is correcting, or
    /// the late energy it reports contains that frame's own direct sound. A
    /// shorter setting is raised to it rather than refused.
    double lateOnsetSeconds = 0.0;

    /// How much of the previous frame's gain to carry forward, 0 to 1. Same
    /// purpose it has in the denoiser: a bin whose estimate crosses back and
    /// forth over its own magnitude gains and dips frame by frame, and that is
    /// heard as warbling rather than as less reverberation.
    double timeSmoothing = 0.5;

    /// Width in bins of the smoothing applied across frequency, 0 for none.
    /// The same mitigation in the other axis.
    int frequencySmoothingBins = 2;

    int fftSize = 2048;
    SampleCount hop = 512;
    dsp::WindowType window = dsp::WindowType::Hann;
};

/// Reduce the late reverberant energy in `audio`, in place.
///
/// ## The model
///
/// There is no measured impulse response here -- if there were, the problem
/// would be deconvolution and not this -- so the tail has to be estimated from
/// the material that produced it.
///
/// Treat the room as a decaying noise process: h(t) = b(t)e^(-dt), with b white
/// and d = 3*ln(10)/T60. Split that response at a time T after the direct
/// sound. Everything after T, shifted back by T, is statistically the whole
/// response again scaled by e^(-dT): the tail of a tail is a tail. So the late
/// energy arriving now is the energy that had already arrived T seconds ago,
/// decayed by e^(-2dT) -- and that is an estimate every STFT bin can make from
/// its own recent magnitude history, with no knowledge of the source material.
/// It is subtracted in the power domain, the gain is floored, smoothed in
/// frequency and in time, and the audio is resynthesised through the same
/// overlap-add the rest of this library uses. The estimate is built from the
/// observed magnitudes rather than from the ones already attenuated, so it does
/// not chase its own output downward.
///
/// After Lebart, Boucher and Denbigh, "A new method based on spectral
/// subtraction for speech dereverberation", Acta Acustica 87 (2001). The
/// derivation is theirs; the gain law, the floor and the smoothing are the
/// ordinary spectral-subtraction furniture and are not claimed to be.
///
/// ## What it assumes about the room
///
///  - **One decay rate, at every frequency.** Real rooms decay faster at the
///    top: air absorption and soft furnishings both take the treble first. A
///    single `decaySeconds` therefore over-removes somewhere and under-removes
///    somewhere else, and the error is largest on a room with a lively bottom
///    and a dead top.
///  - **A diffuse tail.** The estimate is right about the *average* energy in
///    the tail. That is only useful if the tail is dense enough that its
///    average and its instantaneous value are close, which a real room's late
///    field is and a small hard room's early field is not.
///  - **One room, not changing.** A source that moves, a door that opens, two
///    rooms cut together: each breaks the single decay the estimate is built
///    on, and it is simply wrong for as long as the break lasts.
///  - **The early part is wanted.** Nothing before `lateOnsetSeconds` is
///    touched. A recording whose problem is a boxy early pattern rather than a
///    long tail is not what this repairs.
///
/// ## What it does to material that breaks them
///
///  - **A discrete reflection is barely touched.** A slapback is not the
///    average of anything, and whether it is reduced at all comes down to
///    whether it happens to land past `lateOnsetSeconds`. Measured at the
///    defaults on two echoes that both went in 6.02 dB below the direct sound:
///    the one at 30 ms came back at 6.06 dB below, which is untouched, and the
///    one at 90 ms at 9.29 dB below, which is three decibels off an echo that
///    is still plainly an echo. Neither is a repair. This is the case it is
///    worst at and it is not marginal.
///  - **Sustained material loses a little level.** A steady tone is
///    indistinguishable from its own tail -- the energy 85 ms ago looks the
///    same either way -- so any estimator of this shape takes a bite out of it,
///    and the bite is the fraction of a room's energy the model says arrives
///    late. On a held 440 Hz tone that is 0.21 dB at `decaySeconds` 0.4, 1.00
///    dB at 0.8 and 2.29 dB at 1.5. Over-stating the room is therefore not a
///    free way to remove more; it is how this starts sounding like a gate.
///  - **A room far more reverberant than `decaySeconds` says** is
///    under-treated, and one far less reverberant is over-treated.
///
/// ## What it achieves, measured
///
/// A sweep through a synthetic room of T60 = 0.8 s, de-reverberated at the
/// defaults with `decaySeconds` set to the room's own T60, then deconvolved and
/// measured with `measureRoomAcoustics`. Three room seeds; the middle one:
///
///     measure   room     after    change
///     EDT       0.80 s   0.49 s   -38%
///     T20       0.81 s   0.71 s   -13%
///     T30       0.80 s   0.75 s    -6%
///     C50       2.6 dB   5.1 dB   +2.5 dB
///     D50       0.64     0.76     +0.12
///
/// The spread down that table is not noise and is worth understanding before
/// judging the repair by any one row. Removing a fixed proportion of the late
/// energy scales the decay curve down without tilting it: past the early/late
/// split the processed curve is the original minus a constant, so its slope --
/// which is what T20 and T30 are -- is very nearly unchanged, and what moves
/// them at all is the step at the split dragging the -5 dB end of the fit
/// earlier. EDT is fitted over the first ten decibels, where the step lives.
/// C50 and D50 are ratios of early energy to late, and moving energy across
/// 50 ms is exactly what has happened.
///
/// So this makes a room read clearer and start decaying faster; it does not
/// change the rate at which the tail then decays, and no setting here will.
/// During a tail that matches the model the estimate is the same fraction of
/// the bin's own energy at every instant, so the gain the subtraction settles
/// on is constant and the curve comes down parallel to itself. Moving
/// `lateOnsetSeconds` changes the T20 and T30 readings because it moves where
/// the step falls, not because anything tilts.
///
/// On dry speech-like material through a T60 = 1.5 s room, the difference from
/// the dry original falls from 2.04 dB above it to 0.48 dB below it: 2.5 dB
/// closer to dry, not clean. A shorter room gives less -- 1.4 dB at T60 0.8 --
/// because a shorter tail keeps more of its energy inside the untouched early
/// window. On genuinely dry material it removes 0.55 dB of broadband level and
/// leaves a residual 21.0 dB below the programme.
///
/// The warble is bounded but not silenced. Spectral subtraction's usual failure
/// is a bin surviving on one frame and not the next, so what is measured is the
/// repair's own gain: 2.04 dB RMS of movement per frame across a reverberant
/// gap between syllables, against 3.68 dB with the floor dropped to -60 dB and
/// both smoothers off, and 0.54 dB on dry material. Whether two decibels of
/// movement at a 10.7 ms hop is audible on any given material is a different
/// question and is not one this answers -- nothing here is weighted or masked.
///
/// This is a partial reduction and is not claimed to be anything else. It makes
/// a room sound smaller; it does not make a take sound dry, and past about
/// 10 dB of `reductionDb` it stops making much difference at all, because the
/// estimate is already removing everything it believes is there and the floor
/// is holding up the rest.
[[nodiscard]] Status reduceReverb(AudioBufferView audio, SampleRate rate,
                                  const DereverbSettings& settings = {});

} // namespace sa::spectral
