#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>

/// Measuring an impulse response with a swept sine.
///
/// Firing a starting pistol in a room and recording it is the obvious way to
/// get an impulse response, and it is a poor one: all the energy arrives at
/// once, so the signal-to-noise ratio is whatever a single instant can manage,
/// and any loudspeaker asked to reproduce a true impulse distorts badly.
///
/// A sweep spreads the same measurement over seconds. Every frequency is
/// visited in turn at a comfortable level, so the energy delivered is orders of
/// magnitude greater without anything clipping, and the impulse response is
/// recovered afterwards by convolving what came back with a filter matched to
/// what went out.
///
/// The sweep is exponential rather than linear, which buys the property the
/// technique is known for: a loudspeaker's harmonic distortion products
/// deconvolve to *negative* times, arriving before the linear response instead
/// of on top of it. Windowing them off is then trivial, where with a linear
/// sweep they are smeared through the answer and cannot be separated at all.
///
/// Only the arithmetic lives here. Playing the sweep and recording the room
/// needs hardware this container does not have; what can be checked without it
/// is that a sweep convolved with a known response and then deconvolved gives
/// that response back, and that is what the tests do.
namespace sa::analysis {

struct SweepSettings {
    /// Where the sweep starts and ends. Below about 20 Hz most loudspeakers
    /// contribute only distortion, and above Nyquist there is nothing to
    /// measure; both are clamped to what the rate can carry.
    double startHz = 20.0;
    double endHz = 20000.0;

    /// How long the sweep lasts. Longer is quieter for the same energy and
    /// pushes the noise floor down; five seconds is a reasonable room
    /// measurement and ten is a careful one.
    double seconds = 5.0;

    double amplitude = 0.5;

    /// Fades at each end. Without them the sweep starts and stops at an
    /// arbitrary phase, and the step that leaves is a click -- which is a
    /// broadband impulse, which is exactly what the measurement is trying to
    /// isolate.
    double fadeSeconds = 0.02;
};

/// The sweep to play.
[[nodiscard]] Result<AudioBuffer> generateSweep(SampleRate rate,
                                                const SweepSettings& settings = {});

/// The filter that turns a recording of that sweep back into an impulse.
///
/// The time-reversed sweep with an amplitude envelope that undoes its own
/// spectral tilt. An exponential sweep spends logarithmically less time per
/// hertz as it rises, so its energy density falls as 1/f -- 3 dB per octave --
/// and its magnitude as 1/sqrt(f). Reversing it alone gives the conjugate, so
/// the pair would multiply out to 1/f and deconvolve to a low-pass rather than
/// an impulse; the envelope supplies the gain proportional to f that cancels
/// that, decaying across the filter so the high frequencies at its front keep
/// full weight and the low ones at its back are held down.
[[nodiscard]] Result<AudioBuffer> sweepInverseFilter(SampleRate rate,
                                                     const SweepSettings& settings = {});

/// Recover an impulse response from a recording of the sweep.
///
/// `recorded` is what came back: the sweep after passing through whatever is
/// being measured. The result holds the linear impulse response starting at
/// index zero, with everything that deconvolved to negative time -- the
/// harmonic distortion products -- discarded.
///
/// `keepSeconds` bounds the length kept. An impulse response is as long as the
/// reverberation and no longer; keeping the whole deconvolution would keep
/// seconds of nothing.
///
/// The result is never longer than the recording supports either. A recording
/// of length R holds a sweep of length N through a response of length L, so
/// R = N + L - 1 and only R - N + 1 samples of it are a measurement; whichever
/// of that and `keepSeconds` is shorter is what comes back. A recording shorter
/// than the sweep is refused outright rather than deconvolved into something
/// that would look like an answer.
[[nodiscard]] Result<AudioBuffer> deconvolveSweep(ConstAudioBufferView recorded, SampleRate rate,
                                                  const SweepSettings& settings = {},
                                                  double keepSeconds = 0.0, int channel = 0);

} // namespace sa::analysis
