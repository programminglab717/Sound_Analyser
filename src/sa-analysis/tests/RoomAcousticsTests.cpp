#include <sa/analysis/RoomAcoustics.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <random>

using namespace sa;
using namespace sa::analysis;
using Catch::Approx;

namespace {

constexpr SampleRate kRate{48000.0};

/// An exponentially decaying noise burst: the textbook idealisation of a room,
/// and the one case where the right answer can be written down.
///
/// The envelope is exp(-t/tau), so the energy goes as exp(-2t/tau) and the
/// decay in decibels is -10*log10(e) * 2t/tau = -8.6859 t/tau. Sixty decibels
/// therefore takes 60*tau/8.6859 = 6.9078*tau seconds, which is the T60 every
/// measure below should recover.
[[nodiscard]] double tauForT60(double t60Seconds) {
    return t60Seconds * std::log10(std::numbers::e) * 2.0 * 10.0 / 60.0;
}

[[nodiscard]] AudioBuffer decayingNoise(double t60Seconds, double lengthSeconds, unsigned seed = 17,
                                        double leadingSilence = 0.0, double noiseFloor = 0.0) {
    const auto frames = static_cast<SampleCount>(lengthSeconds * kRate.hz());
    const auto lead = static_cast<SampleCount>(leadingSilence * kRate.hz());
    const double tau = tauForT60(t60Seconds);

    std::mt19937 engine{seed};
    std::normal_distribution<double> noise{0.0, 1.0};
    AudioBuffer buffer{ChannelLayout::mono(), frames + lead};
    for (SampleCount i = 0; i < lead; ++i) {
        buffer.channel(0)[i] = static_cast<float>(noiseFloor * noise(engine));
    }
    for (SampleCount i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kRate.hz();
        const double envelope = std::exp(-t / tau);
        buffer.channel(0)[i + lead] =
            static_cast<float>(0.9 * envelope * noise(engine) + noiseFloor * noise(engine));
    }
    return buffer;
}

} // namespace

TEST_CASE("Every reverberation measure recovers a decay whose rate is known", "[analysis][room]") {
    // The whole claim. A room with a 1.2 second T60 has to come back as 1.2.
    for (const double t60 : {0.4, 1.2, 2.5}) {
        const AudioBuffer ir = decayingNoise(t60, t60 * 2.0);
        const auto measured = measureRoomAcoustics(ir.view(), kRate);
        REQUIRE(measured);
        REQUIRE(measured.value().valid);

        REQUIRE(measured.value().hasT20);
        REQUIRE(measured.value().t20Seconds == Approx(t60).epsilon(0.05));
        REQUIRE(measured.value().hasT30);
        REQUIRE(measured.value().t30Seconds == Approx(t60).epsilon(0.05));
        // A purely exponential decay has no early reflections, so EDT agrees
        // with the tail. A real room is where they come apart.
        REQUIRE(measured.value().hasEarlyDecay);
        REQUIRE(measured.value().earlyDecaySeconds == Approx(t60).epsilon(0.10));
    }
}

TEST_CASE("Clarity matches what an exponential decay implies", "[analysis][room]") {
    // C80 is the energy before 80 ms over the energy after it. For an envelope
    // exp(-t/tau) the energy integrates to (1 - exp(-2*0.08/tau)) against
    // exp(-2*0.08/tau), so the ratio is exp(0.16/tau) - 1 and the answer can be
    // written down rather than remembered.
    const double t60 = 1.0;
    const double tau = tauForT60(t60);
    const AudioBuffer ir = decayingNoise(t60, 4.0);

    const auto measured = measureRoomAcoustics(ir.view(), kRate);
    REQUIRE(measured);

    const double expected80 = 10.0 * std::log10(std::exp(0.16 / tau) - 1.0);
    const double expected50 = 10.0 * std::log10(std::exp(0.10 / tau) - 1.0);
    REQUIRE(measured.value().clarity80Db == Approx(expected80).margin(0.6));
    REQUIRE(measured.value().clarity50Db == Approx(expected50).margin(0.6));

    // D50 and C50 are the same statement twice; they have to agree.
    const double fromClarity = std::pow(10.0, measured.value().clarity50Db / 10.0) /
                               (1.0 + std::pow(10.0, measured.value().clarity50Db / 10.0));
    REQUIRE(measured.value().definition50 == Approx(fromClarity).margin(0.01));
}

TEST_CASE("A livelier room reads as less clear", "[analysis][room]") {
    // The direction of the measure, independent of its absolute calibration.
    const auto clarityOf = [](double t60) {
        const AudioBuffer ir = decayingNoise(t60, t60 * 3.0);
        const auto measured = measureRoomAcoustics(ir.view(), kRate);
        REQUIRE(measured);
        return measured.value().clarity80Db;
    };
    REQUIRE(clarityOf(2.5) < clarityOf(1.0));
    REQUIRE(clarityOf(1.0) < clarityOf(0.3));
}

TEST_CASE("Centre time grows with reverberation", "[analysis][room]") {
    const auto centreOf = [](double t60) {
        const AudioBuffer ir = decayingNoise(t60, t60 * 3.0);
        const auto measured = measureRoomAcoustics(ir.view(), kRate);
        REQUIRE(measured);
        return measured.value().centreTimeSeconds;
    };
    REQUIRE(centreOf(0.3) < centreOf(1.0));
    REQUIRE(centreOf(1.0) < centreOf(2.5));
    // For an exponential decay the centre of gravity is tau/2.
    REQUIRE(centreOf(1.0) == Approx(tauForT60(1.0) / 2.0).epsilon(0.15));
}

TEST_CASE("Silence before the impulse is discarded rather than counted", "[analysis][room]") {
    // Half a second of nothing in front of the direct sound would otherwise be
    // half a second of "early energy", and every measure would depend on how
    // the file happened to be trimmed.
    const AudioBuffer tight = decayingNoise(1.0, 3.0, 17, 0.0);
    const AudioBuffer padded = decayingNoise(1.0, 3.0, 17, 0.5);

    const auto a = measureRoomAcoustics(tight.view(), kRate);
    const auto b = measureRoomAcoustics(padded.view(), kRate);
    REQUIRE(a);
    REQUIRE(b);

    REQUIRE(b.value().directSound > 20000); // About half a second in.
    REQUIRE(b.value().t20Seconds == Approx(a.value().t20Seconds).epsilon(0.02));
    REQUIRE(b.value().clarity80Db == Approx(a.value().clarity80Db).margin(0.2));
    REQUIRE(b.value().centreTimeSeconds == Approx(a.value().centreTimeSeconds).margin(0.01));
}

TEST_CASE("A noise floor does not inflate the reverberation time", "[analysis][room]") {
    // The failure the truncation exists to prevent: integrating a noisy tail
    // backwards bends the end of the decay curve flat, and a slope fitted
    // through that reads as a longer reverberation than the room has.
    const AudioBuffer clean = decayingNoise(1.0, 4.0, 17, 0.0, 0.0);
    const AudioBuffer noisy = decayingNoise(1.0, 4.0, 17, 0.0, 0.0015);

    const auto a = measureRoomAcoustics(clean.view(), kRate);
    const auto b = measureRoomAcoustics(noisy.view(), kRate);
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(b.value().hasT20);
    REQUIRE(b.value().t20Seconds == Approx(1.0).epsilon(0.10));
    REQUIRE(b.value().t20Seconds == Approx(a.value().t20Seconds).epsilon(0.10));
}

TEST_CASE("A measurement without the range for T30 says so rather than guessing",
          "[analysis][room]") {
    // A record holding about forty decibels of decay supports T20 and does not
    // support T30. The honest answer to the second is "not measurable", not an
    // extrapolation through the end of the recording.
    //
    // This test is the one that found the missing tail compensation. It was
    // written against a 25 dB record and failed, because the uncompensated
    // curve dives to minus infinity at the end of any finite record and so
    // always reaches -35 dB somewhere. The code was wrong, not the assertion:
    // that same dive was returning T20 = 0.547 for a decay of 1.000 s.
    const double t60 = 1.0;
    const double tau = tauForT60(t60);
    const double seconds = 40.0 * tau / 8.6859;

    const AudioBuffer ir = decayingNoise(t60, seconds, 17);
    const auto measured = measureRoomAcoustics(ir.view(), kRate);
    REQUIRE(measured);
    REQUIRE(measured.value().valid);

    REQUIRE(measured.value().hasT20);
    REQUIRE(measured.value().t20Seconds == Approx(t60).epsilon(0.05));
    REQUIRE_FALSE(measured.value().hasT30);
    REQUIRE(measured.value().t30Seconds == 0.0);
}

TEST_CASE("A record barely longer than the decay measures nothing but EDT", "[analysis][room]") {
    // Twenty-five decibels supports neither reverberation time -- both need
    // the curve to reach -25 dB after starting at -5 -- and saying so is the
    // point. EDT needs only ten decibels and is still available.
    const double t60 = 1.0;
    const double seconds = 25.0 * tauForT60(t60) / 8.6859;

    const AudioBuffer ir = decayingNoise(t60, seconds, 17);
    const auto measured = measureRoomAcoustics(ir.view(), kRate);
    REQUIRE(measured);
    REQUIRE(measured.value().valid);
    REQUIRE_FALSE(measured.value().hasT20);
    REQUIRE_FALSE(measured.value().hasT30);
    REQUIRE(measured.value().hasEarlyDecay);
    REQUIRE(measured.value().earlyDecaySeconds == Approx(t60).epsilon(0.12));
}

TEST_CASE("The tail compensation is what makes a short record honest", "[analysis][room]") {
    // Directly against the numbers the uncompensated version produced, so a
    // regression cannot pass quietly. A record holding forty decibels of a
    // 1.000 s decay returned 0.928 before the compensation and 1.011 after;
    // one holding sixty returned 0.997 and 1.004. The second is within noise
    // of correct either way, which is exactly why the first is the test.
    const double t60 = 1.0;
    const double tau = tauForT60(t60);

    const AudioBuffer shortish = decayingNoise(t60, 40.0 * tau / 8.6859, 17);
    const auto measured = measureRoomAcoustics(shortish.view(), kRate);
    REQUIRE(measured);
    REQUIRE(measured.value().hasT20);
    // The uncompensated answer was 0.928, which is outside this margin; the
    // compensated one is 1.011, which is inside it.
    REQUIRE(measured.value().t20Seconds == Approx(1.0).margin(0.04));
}

TEST_CASE("The Schroeder curve starts at zero and never rises", "[analysis][room]") {
    // The two properties that make it usable at all: it is relative to the
    // total energy, and it is monotonic, which is exactly what integrating
    // backwards buys over reading the envelope directly.
    const AudioBuffer ir = decayingNoise(1.0, 3.0);
    const auto curve = schroederCurveDb(ir.view());
    REQUIRE(curve);
    REQUIRE(curve.value().size() > 1000);
    REQUIRE(curve.value().front() == Approx(0.0).margin(1e-6));
    for (std::size_t i = 1; i < curve.value().size(); ++i) {
        REQUIRE(curve.value()[i] <= curve.value()[i - 1] + 1e-5f);
    }
}

TEST_CASE("Nonsense input is refused or reported as unmeasurable", "[analysis][room]") {
    const AudioBuffer ir = decayingNoise(1.0, 1.0);
    REQUIRE_FALSE(measureRoomAcoustics(ir.view(), kRate, 3));
    REQUIRE_FALSE(measureRoomAcoustics(ir.view(), SampleRate{0.0}));
    REQUIRE_FALSE(schroederCurveDb(ir.view(), -1));

    const AudioBuffer empty{ChannelLayout::mono(), 0};
    const auto nothing = measureRoomAcoustics(empty.view(), kRate);
    REQUIRE(nothing);
    REQUIRE_FALSE(nothing.value().valid);

    const AudioBuffer silent{ChannelLayout::mono(), 48000};
    const auto quiet = measureRoomAcoustics(silent.view(), kRate);
    REQUIRE(quiet);
    REQUIRE_FALSE(quiet.value().valid);
}

TEST_CASE("Each channel of a stereo impulse response is measured separately", "[analysis][room]") {
    // A stereo IR is two rooms as heard from two places, not one averaged room.
    AudioBuffer pair{ChannelLayout::stereo(), 48000 * 3};
    const AudioBuffer fast = decayingNoise(0.4, 3.0, 5);
    const AudioBuffer slow = decayingNoise(2.0, 3.0, 9);
    for (SampleCount i = 0; i < pair.frames(); ++i) {
        pair.channel(0)[i] = fast.channel(0)[i];
        pair.channel(1)[i] = slow.channel(0)[i];
    }

    const auto left = measureRoomAcoustics(pair.view(), kRate, 0);
    const auto right = measureRoomAcoustics(pair.view(), kRate, 1);
    REQUIRE(left);
    REQUIRE(right);
    REQUIRE(left.value().t20Seconds == Approx(0.4).epsilon(0.08));
    REQUIRE(right.value().t20Seconds == Approx(2.0).epsilon(0.08));
}
