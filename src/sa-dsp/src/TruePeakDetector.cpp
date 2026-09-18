#include <sa/dsp/TruePeakDetector.h>
#include <sa/dsp/WindowedSinc.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace sa::dsp {

namespace {

/// Kaiser beta for the interpolation prototype.
///
/// Low, deliberately. A high beta buys stopband depth by widening the
/// transition, and the transition has nowhere to go but down into the top of
/// the audio band -- where a peak that reads low is exactly the failure this
/// class exists to prevent. Beta 5 holds the worst phase within 0.03 dB to 0.45
/// of the sample rate and still puts the images around 54 dB down, far enough
/// that they cannot invent a peak that is not there.
///
/// Measured worst-phase magnitude at 0.45 of the sample rate, 33 taps:
/// beta 3 gives 0.00 dB, beta 5 gives -0.03 dB, beta 8 gives -0.4 dB. The
/// stopband moves the other way, so this is the middle of a real trade rather
/// than a free choice.
constexpr double kWindowBeta = 5.0;

} // namespace

TruePeakDetector::TruePeakDetector(int oversampling) : oversampling_(oversampling) {
    const auto taps = static_cast<std::size_t>(kTapsPerPhase);

    // Prototype: a sinc cut off at Nyquist, of length oversampling * (taps - 1)
    // + 1, windowed. The odd length puts the centre exactly on tap
    // (kTapsPerPhase - 1) / 2 of phase 0, which is what makes phase 0 a unit
    // impulse and the remaining phases land on exact p/oversampling offsets.
    const int prototypeLength = oversampling_ * (kTapsPerPhase - 1) + 1;
    const double centre = static_cast<double>(prototypeLength - 1) / 2.0;
    const double halfWidth = static_cast<double>(kTapsPerPhase - 1) / 2.0;

    phases_.assign(static_cast<std::size_t>(oversampling_) * taps, 0.0);
    for (int phase = 0; phase < oversampling_; ++phase) {
        double sum = 0.0;
        for (int tap = 0; tap < kTapsPerPhase; ++tap) {
            const int index = phase + tap * oversampling_;
            if (index >= prototypeLength) {
                continue;
            }
            const double offset =
                (static_cast<double>(index) - centre) / static_cast<double>(oversampling_);
            const double value = sinc(offset) * kaiserWindow(offset / halfWidth, kWindowBeta);
            phases_[static_cast<std::size_t>(phase) * taps + static_cast<std::size_t>(tap)] = value;
            sum += value;
        }
        // Unity gain at DC for every phase separately. Without it a constant
        // input reads above its own value, and every recording with a DC offset
        // would look as though it were clipping between the samples.
        if (sum != 0.0) {
            for (int tap = 0; tap < kTapsPerPhase; ++tap) {
                phases_[static_cast<std::size_t>(phase) * taps + static_cast<std::size_t>(tap)] /=
                    sum;
            }
        }
    }

    delay_.assign(taps * 2, 0.0);
}

Result<TruePeakDetector> TruePeakDetector::create(int oversampling) {
    if (oversampling < kMinimumOversampling || oversampling > kMaximumOversampling) {
        return Error{ErrorCode::OutOfRange, "oversampling must be 2..16"};
    }
    return TruePeakDetector{oversampling};
}

void TruePeakDetector::reset() noexcept {
    std::fill(delay_.begin(), delay_.end(), 0.0);
    writeIndex_ = 0;
}

double TruePeakDetector::process(float input) noexcept {
    const auto taps = static_cast<std::size_t>(kTapsPerPhase);
    writeIndex_ = (writeIndex_ + 1) % kTapsPerPhase;
    const auto write = static_cast<std::size_t>(writeIndex_);

    const double sample = static_cast<double>(input);
    delay_[write] = sample;
    delay_[write + taps] = sample;

    // The newest sample sits at write + taps and x[n - k] one step down from
    // it, so the whole window is contiguous whatever the write index is.
    const double* history = &delay_[write + taps];

    // The sample the filter is centred on is a candidate in its own right.
    // Phase 0 reproduces it exactly, but taking it directly is what guarantees
    // the answer is never below the sample peak even while the delay line is
    // still filling.
    double peak = std::abs(history[-(kTapsPerPhase - 1) / 2]);

    for (int phase = 0; phase < oversampling_; ++phase) {
        const double* coefficients = &phases_[static_cast<std::size_t>(phase) * taps];
        double accumulator = 0.0;
        for (int tap = 0; tap < kTapsPerPhase; ++tap) {
            accumulator += coefficients[tap] * history[-tap];
        }
        peak = std::max(peak, std::abs(accumulator));
    }
    return peak;
}

} // namespace sa::dsp
