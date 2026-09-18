#pragma once

#include <sa/core/Result.h>
#include <sa/core/Types.h>

namespace sa::dsp {

/// True peak by band-limited reconstruction, offline and without a filter.
///
/// A polyphase interpolator -- what every real-time true-peak meter uses, ours
/// included -- is a finite filter, and a finite filter droops near Nyquist. Ours
/// reads up to 0.44 dB low at 4x on bright transients, which is documented in
/// sa/analysis/TruePeakMeter.h and is the dangerous direction for a meter.
///
/// This has no filter to droop. It transforms a block, zero-pads the spectrum,
/// and transforms back, which is exact band-limited interpolation: the result is
/// the reconstruction, not an approximation of it. Validated against tones whose
/// true peak is known analytically -- a quarter-rate tone whose samples reach
/// only A/sqrt(2) recovers A to five decimal places.
///
/// The cost is why this is not the meter: it is O(n log n) over the whole
/// signal, allocates, and cannot run on the audio thread or on a stream. Use it
/// where exactness is worth that -- verifying a master, and holding a limiter to
/// the ceiling it promised.
///
/// `factor` is how finely the reconstruction is sampled. 16 resolves a steady
/// tone to within +0.002 to +0.041 dB across the band -- always slightly high,
/// never low, which is the safe direction for anything that has to stay under a
/// ceiling.
///
/// The signal is treated as silent outside the range given, which is what a file
/// is. That has a consequence worth knowing: a file whose first sample is at
/// full level is a step, and the band-limited reconstruction of a step
/// overshoots, so such a file genuinely reads above its sample peak at its own
/// beginning. Measured at about 1.15 dB for a full-scale tone starting cold.
/// That is the true answer for that file, not an artefact.
[[nodiscard]] Result<double> exactTruePeak(const float* samples, SampleCount count,
                                           int factor = 16);

/// The same, in dBTP. Silence reports the decibel floor rather than -infinity.
[[nodiscard]] Result<double> exactTruePeakDbtp(const float* samples, SampleCount count,
                                               int factor = 16);

} // namespace sa::dsp
