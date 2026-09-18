#include <sa/analysis/TruePeakMeter.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace sa::analysis {

namespace {

double sinc(double x) noexcept {
    if (x == 0.0) {
        return 1.0;
    }
    const double pix = std::numbers::pi * x;
    return std::sin(pix) / pix;
}

/// Four-term Blackman-Harris. Its ~-92 dB sidelobes keep the images that
/// oversampling creates far below the peak we are trying to find.
double blackmanHarris(int index, int length) noexcept {
    constexpr double a0 = 0.35875;
    constexpr double a1 = 0.48829;
    constexpr double a2 = 0.14128;
    constexpr double a3 = 0.01168;
    const double t = 2.0 * std::numbers::pi * static_cast<double>(index) /
                     static_cast<double>(length - 1);
    return a0 - a1 * std::cos(t) + a2 * std::cos(2.0 * t) - a3 * std::cos(3.0 * t);
}

} // namespace

TruePeakMeter::TruePeakMeter(int channelCount, int oversampling)
    : channelCount_(channelCount), oversampling_(oversampling) {
    const auto channels = static_cast<std::size_t>(channelCount_);
    const auto taps = static_cast<std::size_t>(kTapsPerPhase);

    // Prototype: a sinc interpolator of length oversampling * (taps - 1) + 1,
    // windowed. The odd length puts the centre exactly on tap (kTapsPerPhase-1)/2
    // of phase 0, so phase 0 is a unit impulse and the remaining phases sit at
    // exact p/oversampling sample offsets.
    const int prototypeLength = oversampling_ * (kTapsPerPhase - 1) + 1;
    const double centre = static_cast<double>(prototypeLength - 1) / 2.0;

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
            const double value = sinc(offset) * blackmanHarris(index, prototypeLength);
            phases_[static_cast<std::size_t>(phase) * taps + static_cast<std::size_t>(tap)] = value;
            sum += value;
        }
        // Normalise each phase to unity DC gain. Without this a constant input
        // reads above its own value, which would make every DC-offset recording
        // look like it was clipping.
        if (sum != 0.0) {
            for (int tap = 0; tap < kTapsPerPhase; ++tap) {
                phases_[static_cast<std::size_t>(phase) * taps + static_cast<std::size_t>(tap)] /=
                    sum;
            }
        }
    }

    delay_.assign(channels * taps * 2, 0.0);
    peaks_.assign(channels, 0.0);
}

Result<TruePeakMeter> TruePeakMeter::create(int channelCount, int oversampling) {
    if (channelCount <= 0 || channelCount > kMaxChannels) {
        return Error{ErrorCode::InvalidArgument, "true peak needs 1..kMaxChannels channels"};
    }
    if (oversampling < kMinimumOversampling || oversampling > kMaximumOversampling) {
        return Error{ErrorCode::OutOfRange,
                     "oversampling must be 4..16 -- BS.1770-4 sets 4x as the floor"};
    }
    return TruePeakMeter{channelCount, oversampling};
}

Result<double> TruePeakMeter::measureDbtp(ConstAudioBufferView audio, int oversampling) {
    auto meter = create(audio.channelCount(), oversampling);
    if (!meter) {
        return meter.error();
    }
    TruePeakMeter& instance = meter.value();
    instance.process(audio);
    return instance.truePeakDbtp();
}

void TruePeakMeter::process(ConstAudioBufferView block) noexcept {
    if (block.channelCount() != channelCount_ || block.frames() <= 0) {
        return;
    }

    const auto taps = static_cast<std::size_t>(kTapsPerPhase);
    const auto span = taps * 2;

    for (SampleCount frame = 0; frame < block.frames(); ++frame) {
        writeIndex_ = (writeIndex_ + 1) % kTapsPerPhase;
        const auto write = static_cast<std::size_t>(writeIndex_);

        for (int channel = 0; channel < channelCount_; ++channel) {
            const auto base = static_cast<std::size_t>(channel) * span;
            const double sample = static_cast<double>(block.channel(channel)[frame]);

            delay_[base + write] = sample;
            delay_[base + write + taps] = sample;

            // The raw sample is a peak candidate in its own right. Phase 0 does
            // reproduce it, but only once it has travelled half the filter, so
            // taking it here is what guarantees true peak >= sample peak even
            // for a buffer shorter than the filter.
            double peak = std::max(peaks_[static_cast<std::size_t>(channel)], std::abs(sample));

            // Newest sample sits at write + taps; x[n - k] is one step down.
            const double* history = &delay_[base + write + taps];
            for (int phase = 0; phase < oversampling_; ++phase) {
                const double* coefficients = &phases_[static_cast<std::size_t>(phase) * taps];
                double accumulator = 0.0;
                for (int tap = 0; tap < kTapsPerPhase; ++tap) {
                    accumulator += coefficients[tap] * history[-tap];
                }
                peak = std::max(peak, std::abs(accumulator));
            }
            peaks_[static_cast<std::size_t>(channel)] = peak;
        }
    }

    framesProcessed_ += block.frames();
}

void TruePeakMeter::reset() noexcept {
    std::fill(delay_.begin(), delay_.end(), 0.0);
    std::fill(peaks_.begin(), peaks_.end(), 0.0);
    writeIndex_ = 0;
    framesProcessed_ = 0;
}

double TruePeakMeter::truePeak() const noexcept {
    double peak = 0.0;
    for (double value : peaks_) {
        peak = std::max(peak, value);
    }
    return peak;
}

double TruePeakMeter::channelTruePeak(int channel) const noexcept {
    if (channel < 0 || channel >= channelCount_) {
        return 0.0;
    }
    return peaks_[static_cast<std::size_t>(channel)];
}

} // namespace sa::analysis
