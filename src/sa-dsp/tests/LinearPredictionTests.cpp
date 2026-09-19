#include <sa/dsp/LinearPrediction.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using namespace sa;
using namespace sa::dsp;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;

[[nodiscard]] std::vector<float> tone(SampleCount count, double frequency, double amplitude) {
    std::vector<float> samples(static_cast<std::size_t>(count));
    for (SampleCount i = 0; i < count; ++i) {
        samples[static_cast<std::size_t>(i)] =
            static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * frequency *
                                                    static_cast<double>(i) / kRate));
    }
    return samples;
}

[[nodiscard]] std::vector<float> noise(SampleCount count, double amplitude, unsigned seed) {
    std::mt19937 engine{seed};
    std::normal_distribution<float> distribution{0.0f, static_cast<float>(amplitude)};
    std::vector<float> samples(static_cast<std::size_t>(count));
    for (float& sample : samples) {
        sample = distribution(engine);
    }
    return samples;
}

[[nodiscard]] double meanSquare(const std::vector<float>& samples, std::size_t from,
                                std::size_t to) {
    double total = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        total += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
    }
    return total / static_cast<double>(to - from);
}

} // namespace

TEST_CASE("A held note is almost entirely predictable", "[dsp][lpc]") {
    // A sine is a two-pole resonance, so a model with poles to spare should
    // account for nearly all of it. This is the property the declicker rests
    // on: where the residual is small, a sample that produces a large one did
    // not come from the signal.
    const std::vector<float> samples = tone(2048, 440.0, 0.5);
    const auto fitted = fitLinearPrediction(samples.data(), 2048, 16);
    REQUIRE(fitted);

    std::vector<float> residual(2048);
    predictionResidual(fitted.value(), samples.data(), 2048, residual.data());

    // Measured clear of the start, where the filter has no history to work
    // from and the residual is the signal itself.
    const double signal = meanSquare(samples, 64, 2048);
    const double left = meanSquare(residual, 64, 2048);
    INFO("residual is " << 10.0 * std::log10(left / signal) << " dB below the signal");
    CHECK(left / signal < 1e-4);
}

TEST_CASE("Noise is not predictable, and the fit says so", "[dsp][lpc]") {
    // The other half of the same claim. A model that reported a small residual
    // here would be describing the window rather than the signal, and the
    // declicker built on it would find clicks in every recording.
    const std::vector<float> samples = noise(4096, 0.3, 12345);
    const auto fitted = fitLinearPrediction(samples.data(), 4096, 16);
    REQUIRE(fitted);

    std::vector<float> residual(4096);
    predictionResidual(fitted.value(), samples.data(), 4096, residual.data());

    const double signal = meanSquare(samples, 64, 4096);
    const double left = meanSquare(residual, 64, 4096);
    INFO("residual is " << 10.0 * std::log10(left / signal) << " dB from the signal");
    CHECK(left / signal > 0.8);
}

TEST_CASE("The residual is the convolution it says it is", "[dsp][lpc]") {
    // The documented contract, checked by hand: a[0] is stored as 1 and the
    // residual is a plain convolution, with no special case at k = 0.
    LinearPrediction prediction;
    prediction.coefficients = {1.0, -0.5, 0.25};

    const std::vector<float> samples{1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    std::vector<float> residual(5);
    predictionResidual(prediction, samples.data(), 5, residual.data());

    CHECK(residual[0] == Approx(1.0));                          // 1
    CHECK(residual[1] == Approx(2.0 - 0.5 * 1.0));              // 1.5
    CHECK(residual[2] == Approx(3.0 - 0.5 * 2.0 + 0.25 * 1.0)); // 2.25
    CHECK(residual[3] == Approx(4.0 - 0.5 * 3.0 + 0.25 * 2.0)); // 3.0
    CHECK(residual[4] == Approx(5.0 - 0.5 * 4.0 + 0.25 * 3.0)); // 3.75

    std::vector<float> backwards(5);
    reversePredictionResidual(prediction, samples.data(), 5, backwards.data());
    CHECK(backwards[4] == Approx(5.0));
    CHECK(backwards[3] == Approx(4.0 - 0.5 * 5.0));
    CHECK(backwards[2] == Approx(3.0 - 0.5 * 4.0 + 0.25 * 5.0));
}

TEST_CASE("A click rings forward in one residual and backward in the other", "[dsp][lpc]") {
    // The property that makes the pair worth having. An impulse echoes through
    // the model for as many samples as it has poles, so the forward residual
    // alone says "something is wrong here and for the next 24 samples". The
    // backward residual says the same about the 24 samples before it. Where
    // both are large is the click and nothing else, which is what decides how
    // much audio a repair is allowed to touch.
    constexpr SampleCount kCount = 2048;
    constexpr SampleCount kClick = 1000;
    constexpr int kOrder = 24;

    std::vector<float> samples = tone(kCount, 440.0, 0.5);
    samples[static_cast<std::size_t>(kClick)] += 0.8f;

    const auto fitted = fitLinearPrediction(samples.data(), kCount, kOrder);
    REQUIRE(fitted);

    std::vector<float> forward(static_cast<std::size_t>(kCount));
    std::vector<float> backward(static_cast<std::size_t>(kCount));
    predictionResidual(fitted.value(), samples.data(), kCount, forward.data());
    reversePredictionResidual(fitted.value(), samples.data(), kCount, backward.data());

    const auto at = [](const std::vector<float>& x, SampleCount i) {
        return std::abs(static_cast<double>(x[static_cast<std::size_t>(i)]));
    };

    // Both are large at the click itself.
    CHECK(at(forward, kClick) > 0.5);
    CHECK(at(backward, kClick) > 0.5);

    // Forward rings after and is quiet before; backward the other way round.
    double afterForward = 0.0;
    double beforeForward = 0.0;
    double afterBackward = 0.0;
    double beforeBackward = 0.0;
    for (int k = 1; k <= kOrder; ++k) {
        afterForward = std::max(afterForward, at(forward, kClick + k));
        beforeForward = std::max(beforeForward, at(forward, kClick - k));
        afterBackward = std::max(afterBackward, at(backward, kClick + k));
        beforeBackward = std::max(beforeBackward, at(backward, kClick - k));
    }
    INFO("forward: " << beforeForward << " before, " << afterForward << " after");
    INFO("backward: " << beforeBackward << " before, " << afterBackward << " after");
    CHECK(afterForward > beforeForward * 10.0);
    CHECK(beforeBackward > afterBackward * 10.0);

    // So the smaller of the two, taken sample by sample, peaks at the click and
    // nowhere near it.
    const auto both = [&](SampleCount i) { return std::min(at(forward, i), at(backward, i)); };
    double elsewhere = 0.0;
    for (SampleCount i = 64; i < kCount - 64; ++i) {
        if (std::abs(i - kClick) > 2) {
            elsewhere = std::max(elsewhere, both(i));
        }
    }
    INFO("at the click " << both(kClick) << ", worst elsewhere " << elsewhere);
    CHECK(both(kClick) > elsewhere * 10.0);
}

TEST_CASE("A fit refuses what it cannot describe", "[dsp][lpc]") {
    const std::vector<float> samples = tone(1024, 440.0, 0.5);

    CHECK_FALSE(fitLinearPrediction(nullptr, 1024, 16).hasValue());
    CHECK_FALSE(fitLinearPrediction(samples.data(), 1024, 0).hasValue());
    CHECK_FALSE(fitLinearPrediction(samples.data(), 1024, kMaximumPredictionOrder + 1).hasValue());
    // Fewer samples than twice the order.
    CHECK_FALSE(fitLinearPrediction(samples.data(), 31, 16).hasValue());

    const std::vector<float> silence(1024, 0.0f);
    CHECK_FALSE(fitLinearPrediction(silence.data(), 1024, 16).hasValue());
}

TEST_CASE("The error reported is the error achieved", "[dsp][lpc]") {
    // The documented meaning of LinearPrediction::error, checked rather than
    // asserted: a held note leaves almost nothing, noise leaves almost all of
    // it, and the number is in the units the header claims.
    const std::vector<float> held = tone(4096, 440.0, 0.5);
    const auto heldFit = fitLinearPrediction(held.data(), 4096, 20);
    REQUIRE(heldFit);

    const std::vector<float> hiss = noise(4096, 0.5, 777);
    const auto hissFit = fitLinearPrediction(hiss.data(), 4096, 20);
    REQUIRE(hissFit);

    INFO("held " << heldFit.value().error << ", noise " << hissFit.value().error);
    CHECK(heldFit.value().error < hissFit.value().error * 1e-3);
    CHECK(heldFit.value().order() <= 20);
    CHECK(hissFit.value().order() == 20);
}
