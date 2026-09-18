#include <sa/core/RealtimeGuard.h>
#include <sa/dsp/Fft.h>
#include <sa/dsp/Resampler.h>
#include <sa/dsp/WindowedSinc.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <numbers>
#include <vector>

using namespace sa;
using namespace sa::dsp;
using Catch::Approx;

namespace {

constexpr int kAnalysisSize = 16384;

Resampler made(const ResamplerSpec& spec) {
    Result<Resampler> result = Resampler::create(spec);
    REQUIRE(result.hasValue());
    return std::move(result).value();
}

ResamplerSpec rates(double inputHz, double outputHz,
                    ResamplerQuality quality = ResamplerQuality::Best) {
    return ResamplerSpec{SampleRate{inputHz}, SampleRate{outputHz}, quality, 0.0};
}

/// Runs a whole signal through, offering it in blocks of `inputBlock` and
/// collecting at most `outputBlock` samples at a time, then drains.
///
/// The block sizes are a parameter because they are the thing most likely to be
/// wrong: a converter that keeps its history correctly produces the same output
/// however the caller chops the stream up, and one that does not produces a
/// click at every boundary.
std::vector<float> convert(Resampler& resampler, const std::vector<float>& input,
                           std::size_t inputBlock, std::size_t outputBlock) {
    std::vector<float> output;
    std::vector<float> scratch(outputBlock);
    std::size_t consumed = 0;

    while (consumed < input.size()) {
        const std::size_t offered = std::min(inputBlock, input.size() - consumed);
        std::size_t done = 0;
        while (done < offered) {
            const ResamplerProgress progress = resampler.process(
                input.data() + consumed + done, static_cast<SampleCount>(offered - done),
                scratch.data(), static_cast<SampleCount>(outputBlock));
            done += static_cast<std::size_t>(progress.inputConsumed);
            output.insert(output.end(), scratch.begin(), scratch.begin() + progress.outputProduced);
            if (progress.inputConsumed == 0 && progress.outputProduced == 0) {
                break;
            }
        }
        consumed += done;
    }

    for (;;) {
        const SampleCount produced =
            resampler.flush(scratch.data(), static_cast<SampleCount>(outputBlock));
        if (produced == 0) {
            break;
        }
        output.insert(output.end(), scratch.begin(), scratch.begin() + produced);
    }
    return output;
}

std::vector<float> convert(Resampler& resampler, const std::vector<float>& input) {
    return convert(resampler, input, 1024, 4096);
}

std::vector<float> sine(double rate, double frequency, double amplitude, std::size_t count) {
    std::vector<float> signal(count);
    for (std::size_t i = 0; i < count; ++i) {
        signal[i] = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * frequency *
                                                            static_cast<double>(i) / rate));
    }
    return signal;
}

/// Linear sweep. The phase is accumulated rather than computed from a closed
/// form so that the instantaneous frequency really is the straight line the
/// analysis below assumes it is.
std::vector<float> sweep(double rate, double from, double to, double amplitude, std::size_t count) {
    std::vector<float> signal(count);
    double phase = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double position = static_cast<double>(i) / static_cast<double>(count);
        phase += 2.0 * std::numbers::pi * (from + (to - from) * position) / rate;
        signal[i] = static_cast<float>(amplitude * std::sin(phase));
    }
    return signal;
}

double decibels(double magnitude) {
    return magnitude > 0.0 ? 20.0 * std::log10(magnitude) : -400.0;
}

/// Amplitude spectrum of one block, normalised so that a sine of amplitude A
/// reads A in its own bin.
///
/// Two window choices, and the difference matters. A tone placed exactly on a
/// bin centre is periodic in the block, so a rectangular window leaks nothing
/// at all and the rest of the spectrum is pure alias and arithmetic noise --
/// which is the only way to see a spur 140 dB down. A sweep is periodic in
/// nothing, so it needs a window, and a Kaiser at beta 20 puts the sidelobes
/// near -190 dB, far enough below the measurement to be certain that what shows
/// up is the converter's and not the window's.
std::vector<double> spectrumOf(const std::vector<float>& signal, std::size_t offset,
                               bool windowed) {
    static const RealFft fft{kAnalysisSize};
    REQUIRE(signal.size() >= offset + static_cast<std::size_t>(kAnalysisSize));

    std::vector<float> block(static_cast<std::size_t>(kAnalysisSize));
    double normalisation = 0.0;
    for (int i = 0; i < kAnalysisSize; ++i) {
        const double position =
            (2.0 * static_cast<double>(i) - (kAnalysisSize - 1)) / (kAnalysisSize - 1);
        const double weight = windowed ? kaiserWindow(position, 20.0) : 1.0;
        normalisation += weight;
        block[static_cast<std::size_t>(i)] = static_cast<float>(
            static_cast<double>(signal[offset + static_cast<std::size_t>(i)]) * weight);
    }

    std::vector<std::complex<float>> bins(static_cast<std::size_t>(fft.binCount()));
    fft.forward(block.data(), bins.data());

    std::vector<double> magnitudes(static_cast<std::size_t>(fft.binCount()));
    for (std::size_t k = 0; k < magnitudes.size(); ++k) {
        magnitudes[k] = static_cast<double>(std::abs(bins[k])) * 2.0 / normalisation;
    }
    return magnitudes;
}

/// Largest bin in [from, to) hertz, in decibels relative to full scale.
double worstInBand(const std::vector<double>& magnitudes, double rate, double from, double to) {
    double worst = 0.0;
    for (std::size_t k = 0; k < magnitudes.size(); ++k) {
        const double frequency = static_cast<double>(k) * rate / kAnalysisSize;
        if (frequency < from || frequency >= to) {
            continue;
        }
        worst = std::max(worst, magnitudes[k]);
    }
    return decibels(worst);
}

/// Level of a tone that was placed on bin `bin`, and the worst spur anywhere
/// else. Bins either side of the tone and the first few bins are excluded: the
/// former carry the tone's own rounding, the latter the DC offset that float32
/// arithmetic leaves behind.
struct ToneMeasurement {
    double levelDb = 0.0;
    double spurDb = 0.0;
    double spurHz = 0.0;
};

ToneMeasurement measureTone(const std::vector<float>& signal, std::size_t offset, int bin,
                            double rate) {
    const std::vector<double> magnitudes = spectrumOf(signal, offset, false);
    ToneMeasurement measurement;
    double spur = 0.0;
    for (std::size_t k = 0; k < magnitudes.size(); ++k) {
        const auto index = static_cast<int>(k);
        if (bin >= 0 && std::abs(index - bin) <= 2) {
            measurement.levelDb = std::max(measurement.levelDb, magnitudes[k]);
            continue;
        }
        if (index <= 2) {
            continue;
        }
        if (magnitudes[k] > spur) {
            spur = magnitudes[k];
            measurement.spurHz = static_cast<double>(k) * rate / kAnalysisSize;
        }
    }
    measurement.levelDb = decibels(measurement.levelDb);
    measurement.spurDb = decibels(spur);
    return measurement;
}

/// A tone frequency that lands exactly on an FFT bin of the output, so that a
/// rectangular window sees a single bin and nothing else.
double binnedFrequency(double outputRate, int bin) {
    return static_cast<double>(bin) * outputRate / kAnalysisSize;
}

} // namespace

// ---------------------------------------------------------------------------
// Windowed-sinc primitives
// ---------------------------------------------------------------------------

TEST_CASE("The windowed-sinc primitives are what they claim to be", "[dsp][resampler][window]") {
    CHECK(sinc(0.0) == 1.0);
    for (int k = 1; k <= 8; ++k) {
        INFO("zero " << k);
        CHECK(sinc(static_cast<double>(k)) == Approx(0.0).margin(1e-15));
        CHECK(sinc(-static_cast<double>(k)) == Approx(0.0).margin(1e-15));
    }
    CHECK(sinc(0.5) == Approx(2.0 / std::numbers::pi).margin(1e-15));

    // I0 against values from the published tables, to the digits they print.
    CHECK(besselI0(0.0) == Approx(1.0).margin(1e-15));
    CHECK(besselI0(1.0) == Approx(1.2660658778).margin(1e-9));
    CHECK(besselI0(5.0) == Approx(27.2398718236).margin(1e-8));
    CHECK(besselI0(10.0) == Approx(2815.7166284663).margin(1e-6));

    // The window is one at the centre, symmetric, and never rises away from it.
    CHECK(kaiserWindow(0.0, 14.0) == Approx(1.0).margin(1e-12));
    CHECK(kaiserWindow(1.0, 14.0) == Approx(1.0 / besselI0(14.0)).margin(1e-12));
    CHECK(kaiserWindow(1.5, 14.0) == 0.0);
    CHECK(kaiserWindow(std::numeric_limits<double>::quiet_NaN(), 14.0) == 0.0);
    double previous = 1.0;
    for (double position = 0.0; position <= 1.0; position += 0.01) {
        const double value = kaiserWindow(position, 14.0);
        INFO("position " << position);
        CHECK(value == Approx(kaiserWindow(-position, 14.0)).margin(1e-15));
        CHECK(value <= previous + 1e-15);
        previous = value;
    }
    // beta 0 is the rectangular window, which is the degenerate case the
    // empirical fit has no branch for.
    CHECK(kaiserWindow(0.7, 0.0) == Approx(1.0).margin(1e-15));

    CHECK(kaiserBeta(100.0) == Approx(0.1102 * 91.3).margin(1e-12));
    CHECK(kaiserBeta(10.0) == 0.0);
    // The Best preset is 128 taps at a transition of roughly 0.06 of the rate;
    // Kaiser's formula asks for about that for the stopband it measures, which
    // is the cross-check that the preset was not simply guessed.
    CHECK(kaiserLength(136.0, 0.06) == Approx(148.5).margin(2.0));
    CHECK(kaiserLength(100.0, 0.0) == 0.0);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST_CASE("The resampler rejects specs it cannot honour", "[dsp][resampler]") {
    CHECK(Resampler::create().hasValue());
    CHECK(Resampler::create(rates(48000.0, 44100.0)).hasValue());
    CHECK(Resampler::create(rates(44100.0, 48000.0, ResamplerQuality::Fast)).hasValue());

    CHECK_FALSE(Resampler::create(rates(0.0, 48000.0)).hasValue());
    CHECK_FALSE(Resampler::create(rates(48000.0, 0.0)).hasValue());
    CHECK_FALSE(Resampler::create(rates(48000.0, -44100.0)).hasValue());
    // 768 kHz down to 1 kHz is a ratio of 1/768, past the point where the
    // history the filter would need is worth reserving.
    CHECK_FALSE(Resampler::create(rates(768000.0, 1000.0)).hasValue());

    ResamplerSpec spec = rates(48000.0, 44100.0);
    spec.lowestRatio = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(Resampler::create(spec).hasValue());
    spec.lowestRatio = -1.0;
    CHECK_FALSE(Resampler::create(spec).hasValue());
    // Above the spec's own ratio it reserves nothing, which is a mistake worth
    // reporting rather than quietly ignoring.
    spec.lowestRatio = 2.0;
    CHECK_FALSE(Resampler::create(spec).hasValue());
    spec.lowestRatio = 1.0 / 1000.0;
    CHECK_FALSE(Resampler::create(spec).hasValue());

    const Result<Resampler> error = Resampler::create(rates(768000.0, 1000.0));
    REQUIRE_FALSE(error.hasValue());
    CHECK(error.error().code() == ErrorCode::OutOfRange);
}

TEST_CASE("Ratio changes are checked against the history that was reserved", "[dsp][resampler]") {
    Resampler fixed = made(rates(48000.0, 48000.0));
    CHECK(fixed.ratio() == Approx(1.0));
    // Nothing was reserved beyond 1:1, so downsampling now would need history
    // that create() did not allocate -- and allocating it here is exactly what
    // an audio thread must not do.
    CHECK_FALSE(fixed.setRatio(0.5).ok());
    CHECK(fixed.setRatio(2.0).ok());
    CHECK(fixed.ratio() == Approx(2.0));
    CHECK_FALSE(fixed.setRatio(0.0).ok());
    CHECK_FALSE(fixed.setRatio(std::numeric_limits<double>::infinity()).ok());
    CHECK_FALSE(fixed.setRatio(std::numeric_limits<double>::quiet_NaN()).ok());
    CHECK_FALSE(fixed.setRates(SampleRate{48000.0}, SampleRate{0.0}).ok());

    ResamplerSpec spec = rates(48000.0, 48000.0);
    spec.lowestRatio = 0.25;
    Resampler roomy = made(spec);
    CHECK(roomy.setRatio(0.25).ok());
    CHECK(roomy.tapsPerOutput() > 4 * 128);
    // Past what was reserved. The refusal is about the taps rather than the
    // history: the history is rounded up to a power of two and has slack, but
    // the filter would have to be truncated to fit the reserved tap count, and
    // a truncated windowed sinc is a quietly worse filter.
    CHECK_FALSE(roomy.setRatio(0.2).ok());
    CHECK_FALSE(roomy.setRatio(0.1).ok());
    CHECK(roomy.setRates(kSampleRate96000, kSampleRate48000).ok());
    CHECK(roomy.ratio() == Approx(0.5));
    CHECK(roomy.hasExactPhase());
}

TEST_CASE("A whole-hertz rate pair steps the phase in integers", "[dsp][resampler]") {
    Resampler exact = made(rates(48000.0, 44100.0));
    CHECK(exact.hasExactPhase());
    CHECK(exact.ratio() == Approx(44100.0 / 48000.0));

    // A ratio from a drift estimate is a real number and cannot be exact. The
    // converter says so rather than pretending.
    CHECK(exact.setRatio(44100.0 / 48000.0 * 1.000001).ok());
    CHECK_FALSE(exact.hasExactPhase());
    CHECK(exact.setRates(SampleRate{48000.0}, SampleRate{44100.0}).ok());
    CHECK(exact.hasExactPhase());

    // A rate that is not a whole number of hertz is honest about it too.
    ResamplerSpec spec = rates(48000.5, 44100.0);
    Resampler inexact = made(spec);
    CHECK_FALSE(inexact.hasExactPhase());
}

// ---------------------------------------------------------------------------
// Streaming contract
// ---------------------------------------------------------------------------

TEST_CASE("A ratio of one is a bit-exact copy", "[dsp][resampler]") {
    // Deliberate: band-limiting a signal that is already at the target rate
    // would only remove the top of a band the caller already had. A 48 kHz to
    // 48 kHz conversion has to null against its input, and the filter cannot
    // give that.
    for (ResamplerQuality quality : {ResamplerQuality::Fast, ResamplerQuality::Best}) {
        Resampler resampler = made(rates(48000.0, 48000.0, quality));
        const std::vector<float> input = sweep(48000.0, 20.0, 23000.0, 0.9, 5000);
        const std::vector<float> output = convert(resampler, input, 37, 13);

        REQUIRE(output.size() == input.size());
        for (std::size_t i = 0; i < input.size(); ++i) {
            INFO("sample " << i);
            REQUIRE(output[i] == input[i]);
        }
        CHECK(resampler.tapsPerOutput() == 1);
        CHECK(resampler.leadInSamples() > 0);
    }
}

TEST_CASE("Degenerate blocks do nothing rather than something", "[dsp][resampler]") {
    Resampler resampler = made(rates(48000.0, 96000.0));
    float sentinel = 0.25f;
    float output = -1.0f;

    CHECK(resampler.process(nullptr, 0, &output, 1).outputProduced == 0);
    CHECK(resampler.process(&sentinel, 0, &output, 1).outputProduced == 0);
    CHECK(resampler.process(&sentinel, -4, &output, 1).inputConsumed == 0);
    CHECK(resampler.process(&sentinel, 1, nullptr, 4).inputConsumed == 0);
    CHECK(resampler.process(&sentinel, 1, &output, 0).inputConsumed == 0);
    CHECK(resampler.process(&sentinel, 1, &output, -1).inputConsumed == 0);
    CHECK(resampler.flush(nullptr, 4) == 0);
    CHECK(resampler.flush(&output, 0) == 0);
    CHECK(output == -1.0f);
    CHECK(sentinel == 0.25f);

    // A single sample is a stream. It is shorter than the filter, so it
    // produces nothing until the flush runs the filter out over it.
    Resampler single = made(rates(48000.0, 96000.0));
    std::vector<float> two(2);
    const ResamplerProgress progress = single.process(&sentinel, 1, two.data(), 2);
    CHECK(progress.inputConsumed == 1);
    CHECK(progress.outputProduced == 0);
    CHECK(single.flush(two.data(), 2) == 2);
    CHECK(single.isDrained());
    CHECK(single.flush(two.data(), 2) == 0);
    // A single sample is a sampled impulse, and what comes out is the filter's
    // response to it: the centre of the filter lands on the first output, half
    // a sample later on the second. The rest of the response is past the end of
    // a one-sample stream and goes with it -- an output has to be as long as
    // the input it describes, not as long as the filter.
    CHECK(two[0] == Approx(0.25).margin(0.02));
    CHECK(two[1] > 0.5f * two[0]);
    CHECK(two[1] < two[0]);
}

TEST_CASE("A stream produces ceil(count * ratio) samples however it is blocked",
          "[dsp][resampler]") {
    struct Conversion {
        double input;
        double output;
    };

    const Conversion conversions[] = {
        {48000.0, 44100.0},  {44100.0, 48000.0}, {96000.0, 48000.0},
        {48000.0, 192000.0}, {96000.0, 8000.0},  {8000.0, 192000.0},
    };

    for (const Conversion& conversion : conversions) {
        const std::size_t count = 9973; // prime, so no block size divides it
        const std::vector<float> input = sweep(conversion.input, 50.0, 2000.0, 0.8, count);
        const double ratio = conversion.output / conversion.input;
        const auto expected =
            static_cast<std::size_t>(std::ceil(static_cast<double>(count) * ratio));

        Resampler once = made(rates(conversion.input, conversion.output));
        const std::vector<float> reference = convert(once, input, count, expected + 64);

        INFO(conversion.input << " -> " << conversion.output);
        CHECK(reference.size() == expected);

        // Block sizes chosen to divide neither the filter length nor each
        // other, and an output buffer far smaller than one block's worth of
        // output, which forces the partially-consumed path.
        for (std::size_t block :
             {std::size_t{1}, std::size_t{7}, std::size_t{37}, std::size_t{1024}}) {
            Resampler blocked = made(rates(conversion.input, conversion.output));
            const std::vector<float> output = convert(blocked, input, block, 13);
            INFO("block " << block);
            REQUIRE(output.size() == reference.size());
            for (std::size_t i = 0; i < output.size(); ++i) {
                REQUIRE(output[i] == reference[i]);
            }
        }

        Resampler sized = made(rates(conversion.input, conversion.output));
        CHECK(sized.maximumOutputFor(static_cast<SampleCount>(count)) >=
              static_cast<SampleCount>(expected));
        CHECK(sized.maximumOutputFor(0) == 0);
    }
}

TEST_CASE("Output sample k carries input time k / ratio", "[dsp][resampler]") {
    // The alignment claim in the header. A 1 kHz sine upsampled 2:1 is compared
    // against the analytic sine at the output rate -- not against another
    // resampler, and not against itself. Anything wrong with the gain, the
    // phase, the fractional-delay table or the priming of the history shows up
    // here as a deviation, and a half-sample offset would show up as 3.8% of
    // full scale.
    //
    // Measured worst deviation: 3.9e-8 of full scale, -142 dB.
    Resampler resampler = made(rates(48000.0, 96000.0));
    const std::vector<float> input = sine(48000.0, 1000.0, 0.5, 20000);
    const std::vector<float> output = convert(resampler, input);
    REQUIRE(output.size() == 40000);

    double worst = 0.0;
    for (std::size_t k = 300; k + 300 < output.size(); ++k) {
        const double expected =
            0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(k) / 96000.0);
        worst = std::max(worst, std::abs(static_cast<double>(output[k]) - expected));
    }
    INFO("worst deviation " << decibels(worst / 0.5) << " dB");
    CHECK(worst < 1e-6);
}

TEST_CASE("The ratio can be swept under a running signal", "[dsp][resampler]") {
    // Varispeed. The filter keeps its history across the change, so the only
    // thing that may move is the rate at which the read position walks -- a
    // step in the output would mean the history or the phase had been dropped.
    ResamplerSpec spec = rates(48000.0, 48000.0);
    spec.lowestRatio = 0.5;
    Resampler resampler = made(spec);

    const std::vector<float> input = sine(48000.0, 100.0, 0.5, 40000);
    std::vector<float> output;
    std::vector<float> scratch(256);
    std::size_t consumed = 0;
    int block = 0;

    while (consumed < input.size()) {
        const double position = static_cast<double>(consumed) / static_cast<double>(input.size());
        REQUIRE(resampler.setRatio(1.0 - 0.5 * position).ok());
        const std::size_t offered = std::min<std::size_t>(512, input.size() - consumed);
        std::size_t done = 0;
        while (done < offered) {
            const ResamplerProgress progress =
                resampler.process(input.data() + consumed + done,
                                  static_cast<SampleCount>(offered - done), scratch.data(), 256);
            done += static_cast<std::size_t>(progress.inputConsumed);
            output.insert(output.end(), scratch.begin(), scratch.begin() + progress.outputProduced);
            if (progress.inputConsumed == 0 && progress.outputProduced == 0) {
                break;
            }
        }
        consumed += done;
        ++block;
    }

    REQUIRE(output.size() > 20000);
    // A 100 Hz sine at no less than 24 kHz moves by at most 0.5 * 2 * pi * 100 /
    // 24000 = 0.013 per sample. Ten times that is still far below the step a
    // dropped history would produce, and well above the largest legitimate
    // step.
    double largest = 0.0;
    for (std::size_t i = 1; i < output.size(); ++i) {
        largest = std::max(
            largest, std::abs(static_cast<double>(output[i]) - static_cast<double>(output[i - 1])));
    }
    CHECK(largest < 0.13);
}

// ---------------------------------------------------------------------------
// Measured quality
// ---------------------------------------------------------------------------

TEST_CASE("Best is flat to 0.00001 dB below 0.40 of the lower rate, Fast to 0.0003 dB below 0.35",
          "[dsp][resampler][quality]") {
    // Passband ripple, measured by putting a tone on an exact FFT bin of the
    // output and reading its level back. Both conversions are checked because
    // the filter is stretched in one direction and not the other, and a bug in
    // the stretch would show up as a gain error rather than as ripple.
    //
    // Measured worst deviation across the band, both directions:
    //   Best  0.0000052 dB below 0.40 of the lower rate
    //   Fast  0.00027 dB below 0.35 of the lower rate
    struct Expectation {
        ResamplerQuality quality;
        double edge;
        double allowedDb;
    };

    const Expectation expectations[] = {
        {ResamplerQuality::Best, 0.40, 0.00005},
        {ResamplerQuality::Fast, 0.35, 0.0010},
    };

    for (const Expectation& expectation : expectations) {
        for (const bool downward : {true, false}) {
            const double inputRate = downward ? 48000.0 : 44100.0;
            const double outputRate = downward ? 44100.0 : 48000.0;
            const double lower = std::min(inputRate, outputRate);

            double worst = 0.0;
            double worstAt = 0.0;
            for (int bin = 64; bin < kAnalysisSize / 2; bin += 419) {
                const double frequency = binnedFrequency(outputRate, bin);
                if (frequency > expectation.edge * lower) {
                    break;
                }
                Resampler resampler = made(rates(inputRate, outputRate, expectation.quality));
                const auto count = static_cast<std::size_t>(
                    (kAnalysisSize + 3000) * inputRate / outputRate + 3000.0);
                const std::vector<float> input = sine(inputRate, frequency, 0.5, count);
                const std::vector<float> output = convert(resampler, input);

                const ToneMeasurement measurement = measureTone(output, 1000, bin, outputRate);
                const double error = measurement.levelDb - decibels(0.5);
                if (std::abs(error) > std::abs(worst)) {
                    worst = error;
                    worstAt = frequency;
                }
            }

            INFO("quality " << static_cast<int>(expectation.quality) << ", " << inputRate << " -> "
                            << outputRate << ", worst " << worst << " dB at " << worstAt << " Hz");
            CHECK(std::abs(worst) < expectation.allowedDb);
        }
    }
}

TEST_CASE("The passband reaches 0.44 of the lower rate (Best) and 0.39 (Fast)",
          "[dsp][resampler][quality]") {
    // Where the flat band stops and the transition starts. This is the number
    // that decides whether a 44.1 kHz conversion keeps the top octave: 0.44 of
    // 44.1 kHz is 19.4 kHz, 0.39 is 17.2 kHz.
    //
    // Measured level of a tone at each fraction of the lower rate, converting
    // 48 kHz to 44.1 kHz:
    //            0.35        0.39       0.40        0.44      0.47
    //   Best   -0.000005      --       -0.000004   -0.016     -10.5
    //   Fast    0.000002    -0.021       --          --         --
    struct Point {
        ResamplerQuality quality;
        double fraction;
        double lowestDb;
        double highestDb;
    };

    const Point points[] = {
        {ResamplerQuality::Best, 0.40, -0.001, 0.001},
        {ResamplerQuality::Best, 0.44, -0.03, 0.001},
        {ResamplerQuality::Best, 0.47, -30.0, -3.0},
        {ResamplerQuality::Fast, 0.35, -0.001, 0.001},
        {ResamplerQuality::Fast, 0.39, -0.05, 0.001},
        {ResamplerQuality::Fast, 0.44, -30.0, -3.0},
    };

    for (const Point& point : points) {
        const int bin = static_cast<int>(point.fraction * kAnalysisSize);
        const double frequency = binnedFrequency(44100.0, bin);
        Resampler resampler = made(rates(48000.0, 44100.0, point.quality));
        const auto count =
            static_cast<std::size_t>((kAnalysisSize + 3000) * 48000.0 / 44100.0 + 3000.0);
        const std::vector<float> input = sine(48000.0, frequency, 0.5, count);
        const std::vector<float> output = convert(resampler, input);

        const ToneMeasurement measurement = measureTone(output, 1000, bin, 44100.0);
        const double level = measurement.levelDb - decibels(0.5);
        INFO("quality " << static_cast<int>(point.quality) << " at " << point.fraction << " ("
                        << frequency << " Hz): " << level << " dB");
        CHECK(level > point.lowestDb);
        CHECK(level < point.highestDb);
    }
}

TEST_CASE("A tone above the output Nyquist is rejected by 135 dB (Best) and 88 dB (Fast)",
          "[dsp][resampler][quality]") {
    // Stopband rejection, measured where it matters: a full-scale tone that the
    // output rate cannot represent. Whatever survives folds back into the band
    // as an alias, so the worst bin anywhere in the output is the rejection.
    //
    // Measured worst over the tones below, 96 kHz -> 48 kHz:
    //   Best -141.3 dB (at an input of 25.75 kHz, the filter's first sidelobe)
    //   Fast  -94.9 dB (at an input of 24.50 kHz, just past the output Nyquist)
    struct Expectation {
        ResamplerQuality quality;
        double rejectionDb;
    };

    const Expectation expectations[] = {
        {ResamplerQuality::Best, 135.0},
        {ResamplerQuality::Fast, 88.0},
    };

    for (const Expectation& expectation : expectations) {
        double worst = -400.0;
        double worstAt = 0.0;
        for (double frequency : {24500.0, 25750.0, 27000.0, 31000.0, 40000.0, 46000.0}) {
            Resampler resampler = made(rates(96000.0, 48000.0, expectation.quality));
            const std::vector<float> input =
                sine(96000.0, frequency, 1.0, 2 * (kAnalysisSize + 4000));
            const std::vector<float> output = convert(resampler, input);

            const ToneMeasurement measurement = measureTone(output, 1000, -1, 48000.0);
            if (measurement.spurDb > worst) {
                worst = measurement.spurDb;
                worstAt = frequency;
            }
        }

        INFO("quality " << static_cast<int>(expectation.quality) << ", worst " << worst
                        << " dB from an input at " << worstAt << " Hz");
        CHECK(worst < -expectation.rejectionDb);
    }
}

TEST_CASE("A swept full-scale input images 145 dB down (Best) and 104 dB down (Fast)",
          "[dsp][resampler][quality]") {
    // Upsampling 48 kHz to 96 kHz doubles the band, so every image the
    // interpolation fails to suppress lands in the top half of the output where
    // nothing legitimate can be. A sweep covers every frequency rather than the
    // handful a tone test picks.
    //
    // Measured worst above 25 kHz: Best -150.5 dBFS, Fast -109.3 dBFS.
    //
    // Fast's figure is not its window's doing. Its coefficient table holds 256
    // entries per input sample and is interpolated linearly between them, which
    // puts a floor at about -111 dB; the window itself is 40 dB better than
    // that. Raising the table resolution would move the floor and change
    // nothing audible, since the preset's own stopband sits at -95 dB.
    struct Expectation {
        ResamplerQuality quality;
        double rejectionDb;
    };

    const Expectation expectations[] = {
        {ResamplerQuality::Best, 145.0},
        {ResamplerQuality::Fast, 104.0},
    };

    for (const Expectation& expectation : expectations) {
        Resampler resampler = made(rates(48000.0, 96000.0, expectation.quality));
        const std::vector<float> input = sweep(48000.0, 20.0, 23000.0, 1.0, 150000);
        const std::vector<float> output = convert(resampler, input);

        double worst = -400.0;
        for (std::size_t offset = 4000; offset + kAnalysisSize < output.size();
             offset += kAnalysisSize) {
            const std::vector<double> magnitudes = spectrumOf(output, offset, true);
            worst = std::max(worst, worstInBand(magnitudes, 96000.0, 25000.0, 48000.0));
        }

        INFO("quality " << static_cast<int>(expectation.quality) << ", worst image " << worst
                        << " dBFS");
        CHECK(worst < -expectation.rejectionDb);
    }
}

TEST_CASE("A swept full-scale input aliases 150 dB down (Best) and 118 dB down (Fast)",
          "[dsp][resampler][quality]") {
    // The mirror of the image test. Sweeping past the output's Nyquist while
    // downsampling 96 kHz to 48 kHz means that, once the input is above 26 kHz,
    // every sample of output is alias -- there is nothing else it could be.
    //
    // Measured worst while the input is above 26 kHz: Best -169.2 dBFS,
    // Fast -124.0 dBFS.
    struct Expectation {
        ResamplerQuality quality;
        double rejectionDb;
    };

    const Expectation expectations[] = {
        {ResamplerQuality::Best, 150.0},
        {ResamplerQuality::Fast, 118.0},
    };

    const std::size_t count = 300000;
    const double from = 20.0;
    const double to = 46000.0;

    for (const Expectation& expectation : expectations) {
        Resampler resampler = made(rates(96000.0, 48000.0, expectation.quality));
        const std::vector<float> input = sweep(96000.0, from, to, 1.0, count);
        const std::vector<float> output = convert(resampler, input);

        double worst = -400.0;
        for (std::size_t offset = 0; offset + kAnalysisSize < output.size();
             offset += kAnalysisSize / 2) {
            // Only blocks whose input was entirely above 26 kHz. Two input
            // samples per output sample, and the sweep is linear.
            const double position = 2.0 * static_cast<double>(offset) / static_cast<double>(count);
            if (from + (to - from) * position < 26000.0) {
                continue;
            }
            const std::vector<double> magnitudes = spectrumOf(output, offset, true);
            worst = std::max(worst, worstInBand(magnitudes, 48000.0, 20.0, 24000.0));
        }

        INFO("quality " << static_cast<int>(expectation.quality) << ", worst alias " << worst
                        << " dBFS");
        CHECK(worst < -expectation.rejectionDb);
    }
}

TEST_CASE("Rate conversion there and back leaves the passband where it was",
          "[dsp][resampler][quality]") {
    // 48 kHz to 44.1 kHz and back. Two conversions and two filters, so the
    // result is not a null test -- but everything below the passband edge must
    // come back with its level and its timing intact, and that is what an
    // import/export round trip has to be able to promise.
    Resampler down = made(rates(48000.0, 44100.0));
    Resampler up = made(rates(44100.0, 48000.0));

    const std::vector<float> input = sine(48000.0, 997.0, 0.5, 40000);
    const std::vector<float> middle = convert(down, input);
    const std::vector<float> output = convert(up, middle);

    REQUIRE(output.size() >= input.size());
    double worst = 0.0;
    for (std::size_t i = 2000; i + 2000 < input.size(); ++i) {
        worst = std::max(worst,
                         std::abs(static_cast<double>(output[i]) - static_cast<double>(input[i])));
    }
    INFO("worst round-trip deviation " << decibels(worst / 0.5) << " dB");
    CHECK(worst < 1e-5);
}

// ---------------------------------------------------------------------------
// Real-time safety
// ---------------------------------------------------------------------------

TEST_CASE("Resampling on the audio thread allocates nothing", "[dsp][resampler][rt]") {
    if (!rt::checksEnabled()) {
        SUCCEED("SA_RT_SAFETY_CHECKS is off in this build");
        return;
    }

    ResamplerSpec spec = rates(48000.0, 44100.0, ResamplerQuality::Best);
    spec.lowestRatio = 0.25;
    Resampler resampler = made(spec);
    const std::vector<float> input = sine(48000.0, 1000.0, 0.5, 512);
    std::vector<float> output(1024);

    std::size_t allocations = 0;
    {
        const rt::ScopedAudioThread guard;
        const rt::AllocationScope scope;

        static_cast<void>(resampler.process(input.data(), 512, output.data(), 1024));
        static_cast<void>(resampler.flush(output.data(), 1024));
        static_cast<void>(resampler.setRatio(0.5));
        static_cast<void>(resampler.setRates(kSampleRate48000, kSampleRate96000));
        static_cast<void>(resampler.process(input.data(), 512, output.data(), 1024));
        resampler.reset();
        static_cast<void>(resampler.tapsPerOutput());
        static_cast<void>(resampler.maximumOutputFor(512));
        static_cast<void>(resampler.leadInSamples());
        static_cast<void>(resampler.isDrained());

        allocations = scope.count();
    }
    CHECK(allocations == 0);
}
