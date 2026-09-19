#include <sa/dsp/Biquad.h>
#include <sa/dsp/Deess.h>
#include <sa/dsp/Dynamics.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace sa::dsp {

namespace {

/// A Linkwitz-Riley split: two Butterworth sections in series per band.
///
/// Chosen because its two halves sum back to the original with a flat
/// magnitude, which is the property this needs and which a single Butterworth
/// pair does not have -- two Butterworth halves summed are 3 dB up at the
/// crossover, so a de-esser built on one would put a permanent bump at 5 kHz
/// into every recording it touched, whether or not anything was compressed.
///
/// The phase response is not flat, so the sum is not the input sample for
/// sample. It is the input with an all-pass through it, which is inaudible and
/// is the price of splitting at all.
struct Crossover {
    Biquad lowA;
    Biquad lowB;
    Biquad highA;
    Biquad highB;

    void reset() noexcept {
        lowA.reset();
        lowB.reset();
        highA.reset();
        highB.reset();
    }
};

[[nodiscard]] Result<Crossover> makeCrossover(SampleRate rate, double frequency) {
    FilterSpec low;
    low.type = FilterType::LowPass;
    low.frequency = frequency;
    low.q = kButterworthQ;

    FilterSpec high = low;
    high.type = FilterType::HighPass;

    auto lowCoefficients = BiquadCoefficients::design(rate, low);
    if (!lowCoefficients) {
        return lowCoefficients.error();
    }
    auto highCoefficients = BiquadCoefficients::design(rate, high);
    if (!highCoefficients) {
        return highCoefficients.error();
    }

    Crossover crossover;
    crossover.lowA.setCoefficients(lowCoefficients.value());
    crossover.lowB.setCoefficients(lowCoefficients.value());
    crossover.highA.setCoefficients(highCoefficients.value());
    crossover.highB.setCoefficients(highCoefficients.value());
    return crossover;
}

} // namespace

Result<DeessReport> deess(AudioBufferView audio, SampleRate rate, const DeessSettings& settings,
                          SampleCount runUp) {
    if (audio.channelCount() <= 0 || audio.frames() <= 0) {
        return DeessReport{};
    }
    if (runUp < 0 || runUp > audio.frames()) {
        return Error{ErrorCode::InvalidArgument, "the run-up must fit inside the buffer"};
    }
    if (!(settings.frequencyHz > 0.0) || settings.frequencyHz >= rate.hz() * 0.5) {
        return Error{ErrorCode::InvalidArgument, "the split frequency must be under Nyquist"};
    }
    if (!(settings.maximumReductionDb >= 0.0)) {
        return Error{ErrorCode::InvalidArgument, "the reduction limit cannot be negative"};
    }

    CompressorSettings compressor;
    compressor.thresholdDb = settings.thresholdDb;
    compressor.ratio = settings.ratio;
    compressor.attackSeconds = settings.attackSeconds;
    compressor.releaseSeconds = settings.releaseSeconds;
    compressor.kneeDb = 3.0;

    DeessReport report;
    SampleCount reducedFrames = 0;
    SampleCount countedFrames = 0;

    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        auto crossover = makeCrossover(rate, settings.frequencyHz);
        if (!crossover) {
            return crossover.error();
        }
        auto band = Compressor::create(rate, compressor);
        if (!band) {
            return band.error();
        }

        float* samples = audio.channel(channel);
        for (SampleCount i = 0; i < audio.frames(); ++i) {
            const float input = samples[i];

            const float low =
                crossover.value().lowB.processSample(crossover.value().lowA.processSample(input));
            const float high =
                crossover.value().highB.processSample(crossover.value().highA.processSample(input));

            const float compressed = band.value().processSample(high);

            // The reduction the compressor asked for, clamped. Applied as a
            // blend back towards the untouched band rather than as a gain, so
            // the limit means what it says however the compressor is set: at
            // the limit exactly `maximumReductionDb` is removed and no more.
            double reductionDb = band.value().gainReductionDb();
            float bandOutput = compressed;
            if (reductionDb > settings.maximumReductionDb) {
                const double excess =
                    std::pow(10.0, (reductionDb - settings.maximumReductionDb) / 20.0);
                bandOutput = static_cast<float>(compressed * excess);
                reductionDb = settings.maximumReductionDb;
            }

            samples[i] = low + bandOutput;

            if (i >= runUp) {
                ++countedFrames;
                report.peakReductionDb = std::max(report.peakReductionDb, reductionDb);
                if (reductionDb > 1.0) {
                    ++reducedFrames;
                }
            }
        }
    }

    if (countedFrames > 0) {
        report.fractionReduced =
            static_cast<double>(reducedFrames) / static_cast<double>(countedFrames);
    }
    return report;
}

} // namespace sa::dsp
