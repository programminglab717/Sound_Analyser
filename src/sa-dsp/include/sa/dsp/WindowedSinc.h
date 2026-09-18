#pragma once

namespace sa::dsp {

/// The pieces every windowed-sinc filter in this module is built from.
///
/// A resampler, a fractional-delay interpolator and an inter-sample peak
/// detector are the same filter with different parameters, so the prototype is
/// written once here rather than three times. None of these are audio-thread
/// functions: they are evaluated while a table is being built, and the table is
/// what the audio thread reads.
///
/// The Kaiser window is the choice throughout because it is the only common
/// window with a continuous shape parameter. Hann or Blackman-Harris fix the
/// sidelobe level at whatever the coefficients happen to give; beta lets a
/// design ask for the stopband it actually needs and spend no more filter
/// length than that costs.

/// Normalised sinc: sin(pi x) / (pi x), and exactly 1 at x = 0.
[[nodiscard]] double sinc(double x) noexcept;

/// Zeroth-order modified Bessel function of the first kind.
///
/// The power series, not a rational approximation: this runs once per table
/// entry at construction, so the only thing worth optimising for is being
/// short enough to check by eye. The series converges quickly for the beta
/// range a window uses and is truncated on a relative-magnitude test rather
/// than a fixed term count.
[[nodiscard]] double besselI0(double x) noexcept;

/// Kaiser window at `position`, which runs from -1 at the left edge of the
/// window through 0 at its centre to +1 at the right. Zero outside that range.
///
/// Taking a normalised position rather than an index and a length is what lets
/// the same call serve a table sampled far more finely than one entry per
/// sample, which is what the resampler needs.
[[nodiscard]] double kaiserWindow(double position, double beta) noexcept;

/// Kaiser's empirical beta for a stopband attenuation, in positive decibels.
///
/// The published three-branch fit. Exact enough that a design lands within a
/// decibel or two of the attenuation asked for, which is why every preset in
/// this module still states the attenuation it measured rather than the one it
/// asked for.
[[nodiscard]] double kaiserBeta(double stopbandDb) noexcept;

/// Kaiser's empirical filter length, in samples, for a stopband attenuation in
/// positive decibels and a transition width normalised to the sample rate.
///
/// Returns the length as a real number, because the caller decides how to round
/// it and whether to spend more than the minimum.
[[nodiscard]] double kaiserLength(double stopbandDb, double transitionWidth) noexcept;

} // namespace sa::dsp
