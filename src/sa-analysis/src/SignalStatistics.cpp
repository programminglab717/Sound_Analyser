#include <sa/analysis/SignalStatistics.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace sa::analysis {

double peakToLoudnessRatioDb(double truePeakDbtp, double integratedLufs) noexcept {
    if (truePeakDbtp <= kDecibelFloor || integratedLufs <= kDecibelFloor) {
        return kDecibelFloor;
    }
    return truePeakDbtp - integratedLufs;
}

SignalStatisticsMeter::SignalStatisticsMeter(int channelCount) : channelCount_(channelCount) {
    const auto channels = static_cast<std::size_t>(channelCount_);
    peaks_.assign(channels, 0.0);
    sumsOfSquares_.assign(channels, 0.0);
    sums_.assign(channels, 0.0);
}

Result<SignalStatisticsMeter> SignalStatisticsMeter::create(int channelCount) {
    if (channelCount <= 0 || channelCount > kMaxChannels) {
        return Error{ErrorCode::InvalidArgument, "statistics need 1..kMaxChannels channels"};
    }
    return SignalStatisticsMeter{channelCount};
}

Result<SignalStatistics> SignalStatisticsMeter::measure(ConstAudioBufferView audio,
                                                        double truePeakDbtp,
                                                        double integratedLufs) {
    auto meter = create(audio.channelCount());
    if (!meter) {
        return meter.error();
    }
    SignalStatisticsMeter& instance = meter.value();
    instance.process(audio);
    return instance.statistics(truePeakDbtp, integratedLufs);
}

void SignalStatisticsMeter::process(ConstAudioBufferView block) noexcept {
    if (block.channelCount() != channelCount_ || block.frames() <= 0) {
        return;
    }

    for (int channel = 0; channel < channelCount_; ++channel) {
        const auto index = static_cast<std::size_t>(channel);
        const float* samples = block.channel(channel);

        double peak = peaks_[index];
        double sumOfSquares = sumsOfSquares_[index];
        double sum = sums_[index];

        for (SampleCount i = 0; i < block.frames(); ++i) {
            const double sample = static_cast<double>(samples[i]);
            peak = std::max(peak, std::abs(sample));
            sumOfSquares += sample * sample;
            sum += sample;
        }

        peaks_[index] = peak;
        sumsOfSquares_[index] = sumOfSquares;
        sums_[index] = sum;
    }

    frames_ += block.frames();
}

void SignalStatisticsMeter::reset() noexcept {
    std::fill(peaks_.begin(), peaks_.end(), 0.0);
    std::fill(sumsOfSquares_.begin(), sumsOfSquares_.end(), 0.0);
    std::fill(sums_.begin(), sums_.end(), 0.0);
    frames_ = 0;
}

SignalStatistics SignalStatisticsMeter::statistics(double truePeakDbtp,
                                                   double integratedLufs) const noexcept {
    SignalStatistics result;
    result.channels = channelCount_;
    result.frames = frames_;
    result.peakToLoudnessRatioDb = peakToLoudnessRatioDb(truePeakDbtp, integratedLufs);

    if (frames_ <= 0) {
        return result;
    }

    double totalSquares = 0.0;
    for (int channel = 0; channel < channelCount_; ++channel) {
        const auto index = static_cast<std::size_t>(channel);
        result.samplePeak = std::max(result.samplePeak, peaks_[index]);
        totalSquares += sumsOfSquares_[index];

        const double mean = sums_[index] / static_cast<double>(frames_);
        if (std::abs(mean) > std::abs(result.dcOffset)) {
            result.dcOffset = mean;
        }
    }

    const double samples = static_cast<double>(frames_) * static_cast<double>(channelCount_);
    result.rms = std::sqrt(totalSquares / samples);
    result.samplePeakDbfs = amplitudeToDecibels(result.samplePeak);
    result.rmsDbfs = amplitudeToDecibels(result.rms);

    // Crest factor of silence is 0/0. Reporting 0 dB continues the value any
    // constant signal has -- peak equals RMS -- and keeps a finite number in a
    // field the UI divides and averages.
    result.crestFactorDb =
        result.rms > 0.0 ? amplitudeToDecibels(result.samplePeak / result.rms) : 0.0;
    return result;
}

} // namespace sa::analysis
