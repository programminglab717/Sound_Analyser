#include <sa/analysis/LoudnessContour.h>
#include <sa/analysis/SignalStatistics.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sa::analysis {

namespace {

/// Number of sub-blocks the ring has to hold: the longest window anything here
/// asks about.
constexpr int kRingSubBlocks = LoudnessMeter::kSubBlocksPerShortTerm;

/// Unweighted statistics of one 100 ms sub-block.
///
/// These sit beside the meter rather than inside it because none of them is
/// K-weighted: a crest factor and a true peak describe the signal, not what a
/// listener would call its loudness, so the meter has no reason to carry them
/// and adding them would change what LoudnessMeter is for.
struct SubBlockStatistics {
    /// Inter-sample, linear, worst channel.
    double truePeak = 0.0;
    /// Linear, worst channel.
    double samplePeak = 0.0;
    /// Summed over every channel, as SignalStatistics pools them for RMS.
    double sumOfSquares = 0.0;
};

ChannelLayout layoutForChannelCount(int channels) noexcept {
    if (channels == 1) {
        return ChannelLayout::mono();
    }
    if (channels == 2) {
        return ChannelLayout::stereo();
    }
    return ChannelLayout::discrete(channels);
}

/// Nearest-rank percentile of an already sorted, non-empty list. The same rule
/// LoudnessMeter's gating histogram applies, so the two cannot end up meaning
/// different things by "the 95th percentile".
double percentileOfSorted(const std::vector<double>& sorted, double fraction) noexcept {
    const auto count = static_cast<std::int64_t>(sorted.size());
    const auto rank = static_cast<std::int64_t>(static_cast<double>(count - 1) * fraction + 0.5);
    const std::int64_t clamped = std::clamp(rank, std::int64_t{0}, count - 1);
    return sorted[static_cast<std::size_t>(clamped)];
}

} // namespace

Result<LoudnessContour> measureLoudnessContour(ConstAudioBufferView audio, SampleRate rate,
                                               const ChannelLayout& layout, double intervalSeconds,
                                               int oversampling) {
    if (audio.isEmpty()) {
        return Error{ErrorCode::InvalidArgument, "no audio to measure a contour over"};
    }
    // Written as a negated comparison so that a NaN interval is refused too,
    // rather than producing a point grid that never advances.
    if (!(intervalSeconds > 0.0)) {
        return Error{ErrorCode::InvalidArgument, "contour interval must be positive"};
    }

    auto meterResult = LoudnessMeter::create(rate, layout);
    if (!meterResult) {
        return meterResult.error();
    }
    LoudnessMeter& meter = meterResult.value();

    if (audio.channelCount() != layout.count()) {
        return Error{ErrorCode::InvalidArgument, "buffer channel count does not match the layout"};
    }

    auto peakResult = TruePeakMeter::create(audio.channelCount(), oversampling);
    if (!peakResult) {
        return peakResult.error();
    }
    TruePeakMeter& peakMeter = peakResult.value();

    const SampleCount pointStep = secondsToSamples(intervalSeconds, rate);
    if (pointStep < 1) {
        return Error{ErrorCode::InvalidArgument, "contour interval is shorter than one sample"};
    }

    const int channels = audio.channelCount();
    const SampleCount frames = audio.frames();
    const SampleCount subBlockFrames = meter.samplesPerSubBlock();
    const SampleCount blockFrames =
        subBlockFrames * static_cast<SampleCount>(LoudnessMeter::kSubBlocksPerBlock);
    if (frames < blockFrames) {
        return Error{ErrorCode::InvalidArgument,
                     "audio is shorter than one 400 ms block, so no loudness is defined anywhere"};
    }

    std::vector<SubBlockStatistics> ring(static_cast<std::size_t>(kRingSubBlocks));
    double programmeTruePeak = 0.0;

    LoudnessContour contour;
    contour.intervalSeconds = samplesToSeconds(pointStep, rate);
    contour.points.reserve(static_cast<std::size_t>(frames / pointStep + 1));

    // Short-term readings above the absolute gate, for the summary statistics.
    std::vector<double> gatedShortTerm;

    SampleCount nextPointFrame = 0;

    auto emitPoints = [&]() {
        // The meter is the authority on how far the measurement has got; the
        // ring is filled in step with it, so one index serves both.
        const std::int64_t completed = meter.completedSubBlocks();

        // Called once before the first sub-block and once after each, so the
        // first `completed` to reach a point's own sub-block index is the one
        // that emits it -- which is the state that point describes. A point
        // falling inside the trailing partial sub-block is emitted by the last
        // whole one, since nothing after that changes any reading.
        while (nextPointFrame <= frames && nextPointFrame / subBlockFrames <= completed) {
            LoudnessPoint point;
            point.timeSeconds = samplesToSeconds(nextPointFrame, rate);

            if (meter.momentaryAvailable()) {
                point.momentaryLufs = meter.momentaryLufs();

                double sumOfSquares = 0.0;
                double samplePeak = 0.0;
                for (int back = 0; back < LoudnessMeter::kSubBlocksPerBlock; ++back) {
                    const auto slot =
                        static_cast<std::size_t>((completed - 1 - back) % kRingSubBlocks);
                    sumOfSquares += ring[slot].sumOfSquares;
                    samplePeak = std::max(samplePeak, ring[slot].samplePeak);
                }

                const double windowSamples =
                    static_cast<double>(blockFrames) * static_cast<double>(channels);
                const double rms = std::sqrt(sumOfSquares / windowSamples);
                // Crest of a silent window is 0/0. Reporting 0 dB continues the
                // value every constant signal has -- peak equals RMS -- which is
                // the convention SignalStatistics already set for the same
                // quantity.
                point.crestDb = rms > 0.0 ? amplitudeToDecibels(samplePeak / rms) : 0.0;
            }

            if (meter.shortTermAvailable()) {
                const double shortTerm = meter.shortTermLufs();
                point.shortTermLufs = shortTerm;

                double windowPeak = 0.0;
                for (int back = 0; back < LoudnessMeter::kSubBlocksPerShortTerm; ++back) {
                    const auto slot =
                        static_cast<std::size_t>((completed - 1 - back) % kRingSubBlocks);
                    windowPeak = std::max(windowPeak, ring[slot].truePeak);
                }
                const double windowPeakDbtp = amplitudeToDecibels(windowPeak);
                point.truePeakDbtp = windowPeakDbtp;

                // Guard on the operands rather than on the difference: floor
                // minus floor is zero, and zero is a plausible-looking PSR for a
                // brickwalled master rather than the absence of a reading.
                if (windowPeakDbtp > kDecibelFloor && shortTerm > kDecibelFloor) {
                    point.psrDb = peakToLoudnessRatioDb(windowPeakDbtp, shortTerm);
                }
                if (shortTerm > LoudnessMeter::kAbsoluteGateLufs) {
                    gatedShortTerm.push_back(shortTerm);
                }
            }

            contour.points.push_back(point);
            nextPointFrame += pointStep;
        }
    };

    emitPoints();

    const SampleCount wholeSubBlocks = frames / subBlockFrames;
    for (SampleCount index = 0; index < wholeSubBlocks; ++index) {
        const ConstAudioBufferView view = audio.subRange(index * subBlockFrames, subBlockFrames);

        // Feeding exactly one sub-block at a time is what makes this a reading
        // of the meter rather than a second implementation of it: every value
        // the meter can produce is produced, and none is interpolated.
        meter.process(view);

        peakMeter.process(view);
        SubBlockStatistics statistics;
        statistics.truePeak = peakMeter.truePeak();
        peakMeter.resetPeaks();

        for (int channel = 0; channel < channels; ++channel) {
            const float* samples = view.channel(channel);
            for (SampleCount i = 0; i < subBlockFrames; ++i) {
                const double sample = static_cast<double>(samples[i]);
                statistics.samplePeak = std::max(statistics.samplePeak, std::abs(sample));
                statistics.sumOfSquares += sample * sample;
            }
        }

        programmeTruePeak = std::max(programmeTruePeak, statistics.truePeak);
        ring[static_cast<std::size_t>(index % kRingSubBlocks)] = statistics;

        emitPoints();
    }

    const SampleCount tailFrames = frames - wholeSubBlocks * subBlockFrames;
    if (tailFrames > 0) {
        // Fed so that the integrated figure and the programme true peak cover
        // every sample, exactly as a one-shot measurement of the whole buffer
        // would. An incomplete sub-block produces no reading, so no point of
        // the contour depends on it.
        const ConstAudioBufferView view =
            audio.subRange(wholeSubBlocks * subBlockFrames, tailFrames);
        meter.process(view);
        peakMeter.process(view);
        programmeTruePeak = std::max(programmeTruePeak, peakMeter.truePeak());
    }

    const LoudnessMeasurement programme = meter.measurement();
    contour.integratedLufs = programme.integratedLufs;
    contour.loudnessRangeLu = programme.loudnessRangeLu;
    contour.gatedBlockCount = programme.gatedBlockCount;
    contour.truePeakDbtp = amplitudeToDecibels(programmeTruePeak);

    if (contour.truePeakDbtp > kDecibelFloor && contour.integratedLufs > kDecibelFloor) {
        contour.plrDb = peakToLoudnessRatioDb(contour.truePeakDbtp, contour.integratedLufs);
    }

    if (!gatedShortTerm.empty()) {
        std::sort(gatedShortTerm.begin(), gatedShortTerm.end());
        contour.quietestShortTermLufs = gatedShortTerm.front();
        contour.loudestShortTermLufs = gatedShortTerm.back();
        contour.shortTermPercentile10Lufs =
            percentileOfSorted(gatedShortTerm, LoudnessMeter::kRangeLowPercentile);
        contour.shortTermPercentile95Lufs =
            percentileOfSorted(gatedShortTerm, LoudnessMeter::kRangeHighPercentile);
    }

    return contour;
}

Result<LoudnessContour> measureLoudnessContour(ConstAudioBufferView audio, SampleRate rate,
                                               double intervalSeconds) {
    return measureLoudnessContour(audio, rate, layoutForChannelCount(audio.channelCount()),
                                  intervalSeconds);
}

} // namespace sa::analysis
