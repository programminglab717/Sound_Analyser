#include <sa/analysis/TempoTrack.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <random>
#include <vector>

using namespace sa;
using namespace sa::analysis;
using Catch::Approx;

namespace {

constexpr SampleRate kRate{48000.0};
constexpr double kRateHz = 48000.0;

/// One analysis hop in seconds, and the tolerance every time below is asserted
/// at: 256 samples at 48 kHz is 5.3333 ms. Nothing here can be more accurate
/// than that, and nothing here is allowed to be less.
constexpr double kHopSeconds = 256.0 / 48000.0;

/// Audio and the times of the events in it, kept together so the assertions
/// cannot drift away from the material they are about.
struct Material {
    AudioBuffer audio;
    std::vector<double> times;
};

/// A single full-scale sample on every beat.
///
/// The arithmetic is exact by construction. At 120 BPM a beat is
/// 60 / 120 = 0.5 s, which at 48 kHz is 24000 samples; at 90 BPM it is
/// 60 / 90 = 0.6667 s or 32000 samples. 140 BPM is the awkward one --
/// 60 / 140 * 48000 = 20571.43 samples, which is not a whole number of samples
/// at all -- so clicks are placed at the nearest sample to the exact time and
/// the train is 140 BPM on average rather than in every gap. That is the
/// honest version of the question anyway: no real 140 BPM recording has its
/// beats on sample boundaries either.
///
/// The first click is at 0.5 s by default rather than at zero. A click at
/// sample zero sits at the very start of the first analysis window, where the
/// Hann window is exactly zero, so it is inaudible to the analysis -- and a
/// test whose first beat cannot be found is testing the wrong thing.
[[nodiscard]] Material clickTrain(double bpm, double seconds, double firstClickSeconds = 0.5) {
    const auto frames = static_cast<SampleCount>(seconds * kRateHz);
    AudioBuffer buffer{ChannelLayout::mono(), frames};
    std::vector<double> times;

    const double spacing = 60.0 / bpm * kRateHz;
    for (int beat = 0;; ++beat) {
        const auto at = static_cast<SampleCount>(
            std::llround(firstClickSeconds * kRateHz + spacing * static_cast<double>(beat)));
        if (at >= frames) {
            break;
        }
        buffer.channel(0)[at] = 1.0f;
        times.push_back(static_cast<double>(at) / kRateHz);
    }
    return Material{std::move(buffer), std::move(times)};
}

/// Something with a sound in it rather than an impulse: a short decaying tone
/// on each beat, with a quieter, higher one halfway between.
///
/// Clicks are the easy case in two ways at once -- they are broadband, so
/// every bin of the flux sees them, and there is nothing at all between them.
/// This is neither. The off-beat is there because it is the case where the
/// tempo could honestly be read as double, so it is the case where the octave
/// weighting is doing the work rather than sitting idle.
[[nodiscard]] Material toneBursts(double bpm, double seconds, double firstBeatSeconds = 0.5) {
    const auto frames = static_cast<SampleCount>(seconds * kRateHz);
    AudioBuffer buffer{ChannelLayout::mono(), frames};
    std::vector<double> times;

    const auto addBurst = [&](SampleCount at, double amplitude, double hz) {
        // 40 ms time constant, cut off after 200 ms where it is 7 dB below the
        // noise floor of anything this would be used on.
        const auto length = static_cast<SampleCount>(0.200 * kRateHz);
        for (SampleCount i = 0; i < length && at + i < frames; ++i) {
            const double t = static_cast<double>(i) / kRateHz;
            const double decay = std::exp(-t / 0.040);
            buffer.channel(0)[at + i] +=
                static_cast<float>(amplitude * decay * std::sin(2.0 * std::numbers::pi * hz * t));
        }
    };

    const double halfBeat = 60.0 / bpm * kRateHz * 0.5;
    for (int half = 0;; ++half) {
        const auto at = static_cast<SampleCount>(
            std::llround(firstBeatSeconds * kRateHz + halfBeat * static_cast<double>(half)));
        if (at >= frames) {
            break;
        }
        const bool onBeat = half % 2 == 0;
        addBurst(at, onBeat ? 0.9 : 0.25, onBeat ? 220.0 : 440.0);
        if (onBeat) {
            times.push_back(static_cast<double>(at) / kRateHz);
        }
    }
    return Material{std::move(buffer), std::move(times)};
}

[[nodiscard]] AudioBuffer steadyNoise(double seconds, unsigned seed = 3001) {
    const auto frames = static_cast<SampleCount>(seconds * kRateHz);
    AudioBuffer buffer{ChannelLayout::mono(), frames};
    std::mt19937 engine{seed};
    std::normal_distribution<double> noise{0.0, 0.2};
    for (SampleCount i = 0; i < frames; ++i) {
        buffer.channel(0)[i] = static_cast<float>(noise(engine));
    }
    return buffer;
}

[[nodiscard]] AudioBuffer heldTone(double seconds, double hz = 440.0) {
    const auto frames = static_cast<SampleCount>(seconds * kRateHz);
    AudioBuffer buffer{ChannelLayout::mono(), frames};
    for (SampleCount i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kRateHz;
        buffer.channel(0)[i] = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * hz * t));
    }
    return buffer;
}

[[nodiscard]] double distanceToNearest(const std::vector<double>& times, double at) {
    double closest = std::numeric_limits<double>::max();
    for (const double time : times) {
        closest = std::min(closest, std::abs(time - at));
    }
    return closest;
}

} // namespace

TEST_CASE("The onset envelope rises at the clicks and nowhere else", "[analysis][tempo]") {
    const Material material = clickTrain(120.0, 8.0);
    const auto computed = onsetEnvelope(material.audio.view(), kRate);
    REQUIRE(computed);
    const std::vector<float>& envelope = computed.value();

    // 8 s is 384000 samples; frames are (384000 - 1024) / 256 + 1 = 1497.
    REQUIRE(envelope.size() == 1497);
    REQUIRE(envelope.front() == 0.0f);

    const double loudest = static_cast<double>(*std::max_element(envelope.begin(), envelope.end()));
    REQUIRE(loudest > 0.0);

    // Every click has a frame within one hop of it carrying most of the
    // envelope's full height. Half of the largest onset in the record is a
    // deliberately blunt threshold: the clicks are identical, so anything that
    // finds one has to find all of them.
    for (const double click : material.times) {
        double nearby = 0.0;
        for (std::size_t frame = 0; frame < envelope.size(); ++frame) {
            const double when = onsetFrameSeconds(static_cast<double>(frame), kRate);
            if (std::abs(when - click) <= kHopSeconds) {
                nearby = std::max(nearby, static_cast<double>(envelope[frame]));
            }
        }
        REQUIRE(nearby > 0.5 * loudest);
    }

    // And between the clicks there is nothing. A single sample either falls in
    // an analysis window or it does not, so only the two or three frames whose
    // window has just reached a click can hold any flux at all: with 15 clicks
    // that is at most 45 of 1497 frames.
    const auto sounding = static_cast<std::size_t>(
        std::count_if(envelope.begin(), envelope.end(), [](float value) { return value > 0.0f; }));
    REQUIRE(material.times.size() == 15);
    REQUIRE(sounding <= 3 * material.times.size());
}

TEST_CASE("A click train at 120 BPM reads 120, with the beats on the clicks", "[analysis][tempo]") {
    const Material material = clickTrain(120.0, 12.0);
    const auto tracked = trackTempo(material.audio.view(), kRate);
    REQUIRE(tracked);
    const BeatGrid& grid = tracked.value();

    REQUIRE(grid.valid);
    REQUIRE(grid.bpm == Approx(120.0).margin(1.0));
    REQUIRE(grid.confidence > 0.8);

    // The phase, not just the period: the first beat is the first click.
    REQUIRE(grid.firstBeatSeconds == Approx(0.5).margin(kHopSeconds));

    // Clicks at 0.5 s to 11.5 s inclusive is 23 of them; the grid may run one
    // beat past the last click, because a grid is a grid.
    REQUIRE(material.times.size() == 23);
    REQUIRE(grid.beatSeconds.size() >= material.times.size());
    REQUIRE(grid.beatSeconds.size() <= material.times.size() + 1);
    for (const double click : material.times) {
        REQUIRE(distanceToNearest(grid.beatSeconds, click) <= kHopSeconds);
    }
}

TEST_CASE("The same holds at 90 and at 140 BPM", "[analysis][tempo]") {
    // Two more tempi so that 120 cannot be a constant that happened to be
    // right. 90 divides the sample rate exactly (32000 samples a beat, 125
    // analysis frames); 140 does not divide anything (20571.43 samples,
    // 80.36 frames), so between them they cover both the easy arithmetic and
    // the awkward.
    for (const double bpm : {90.0, 140.0}) {
        const Material material = clickTrain(bpm, 12.0);
        const auto tracked = trackTempo(material.audio.view(), kRate);
        REQUIRE(tracked);
        const BeatGrid& grid = tracked.value();

        REQUIRE(grid.valid);
        REQUIRE(grid.bpm == Approx(bpm).margin(1.0));
        REQUIRE(grid.confidence > 0.8);
        for (const double click : material.times) {
            REQUIRE(distanceToNearest(grid.beatSeconds, click) <= kHopSeconds);
        }

        // Not that the first beat is the first click. At 140 BPM a beat is
        // 0.4286 s and the first click is at 0.5, so the grid quite correctly
        // starts at 0.0714 s -- on a beat where nothing was played, which is
        // what a grid is for. What can be asserted is that it starts within
        // one beat of the first click rather than anywhere at all.
        const double beatSeconds = 60.0 / bpm;
        REQUIRE(grid.firstBeatSeconds > material.times.front() - beatSeconds - kHopSeconds);
        REQUIRE(grid.firstBeatSeconds <= material.times.front() + kHopSeconds);
    }
}

TEST_CASE("A train that does not start on a round number comes back with its phase",
          "[analysis][tempo]") {
    // 0.37 s is not a multiple of the beat and not a multiple of the hop
    // (0.37 * 48000 = 17760 samples, which is 69.375 hops), so nothing about
    // the framing can arrive at it by accident.
    const Material material = clickTrain(120.0, 12.0, 0.37);
    const auto tracked = trackTempo(material.audio.view(), kRate);
    REQUIRE(tracked);
    const BeatGrid& grid = tracked.value();

    REQUIRE(grid.valid);
    REQUIRE(grid.bpm == Approx(120.0).margin(1.0));
    REQUIRE(grid.firstBeatSeconds == Approx(0.37).margin(kHopSeconds));
    for (const double click : material.times) {
        REQUIRE(distanceToNearest(grid.beatSeconds, click) <= kHopSeconds);
    }

    // The period is the same as the aligned train's; only the offset moved.
    const Material aligned = clickTrain(120.0, 12.0);
    const auto other = trackTempo(aligned.audio.view(), kRate);
    REQUIRE(other);
    REQUIRE(grid.bpm == Approx(other.value().bpm).epsilon(0.005));
    REQUIRE(std::abs(grid.firstBeatSeconds - other.value().firstBeatSeconds) > 0.1);
}

TEST_CASE("Tone bursts with a quiet off-beat are read at the beat, not at the off-beat",
          "[analysis][tempo]") {
    // 90 BPM, so the off-beats are at 180 BPM and both are inside the default
    // 60 to 200 range: the half-tempo reading is a live candidate rather than
    // one the range quietly rules out. The compression in the envelope is what
    // makes this a fair fight -- it deliberately flattens the difference
    // between a loud onset and a quiet one, so the off-beat arrives at the
    // autocorrelation nearly as strong as the beat.
    const Material material = toneBursts(90.0, 16.0);
    const auto tracked = trackTempo(material.audio.view(), kRate);
    REQUIRE(tracked);
    const BeatGrid& grid = tracked.value();

    REQUIRE(grid.valid);
    REQUIRE(grid.bpm == Approx(90.0).margin(1.0));
    REQUIRE(grid.confidence > 0.5);
    for (const double beat : material.times) {
        REQUIRE(distanceToNearest(grid.beatSeconds, beat) <= kHopSeconds);
    }
}

TEST_CASE("Material with no rhythm is reported as having none", "[analysis][tempo]") {
    // The other half of being useful. A tempo tracker that always answers is
    // one whose answer cannot be trusted anywhere.
    SECTION("steady noise") {
        // Flux everywhere and events nowhere: the envelope is dense, so the
        // concentration test turns it away before a correlation is taken. The
        // confidence is therefore a zero that was never measured, which is
        // what the header says it will be.
        const AudioBuffer noise = steadyNoise(12.0);
        const auto tracked = trackTempo(noise.view(), kRate);
        REQUIRE(tracked);
        REQUIRE_FALSE(tracked.value().valid);
        REQUIRE(tracked.value().confidence < 0.1);
        REQUIRE(tracked.value().bpm == 0.0);
        REQUIRE(tracked.value().beatSeconds.empty());
    }

    SECTION("a held tone") {
        // The dangerous one, and not for the reason it looks. A 440 Hz sine
        // never changes, so one expects no flux at all -- but a 1024-sample
        // window is 21 ms and cannot separate the tone from its own negative
        // frequency, so the magnitude spectrum pulses at the tone's rate. That
        // pulse is exactly periodic, and an autocorrelation on its own calls
        // it a tempo with a confidence above 0.96. It is only rejected because
        // the pulse is spread over every frame rather than gathered into
        // events.
        const AudioBuffer tone = heldTone(12.0);
        const auto tracked = trackTempo(tone.view(), kRate);
        REQUIRE(tracked);
        REQUIRE_FALSE(tracked.value().valid);
        REQUIRE(tracked.value().bpm == 0.0);
        REQUIRE(tracked.value().beatSeconds.empty());
    }

    SECTION("clicks at no particular time") {
        // Events, but no rhythm -- which is the case the confidence threshold
        // is for, as opposed to the concentration test above. Twenty-four
        // clicks scattered over twelve seconds concentrate as sharply as any
        // beat, and correlate with themselves at nothing.
        const auto frames = static_cast<SampleCount>(12.0 * kRateHz);
        AudioBuffer scattered{ChannelLayout::mono(), frames};
        std::mt19937 engine{7};
        std::uniform_int_distribution<SampleCount> where{0, frames - 1};
        for (int click = 0; click < 24; ++click) {
            scattered.channel(0)[where(engine)] = 1.0f;
        }
        const auto tracked = trackTempo(scattered.view(), kRate);
        REQUIRE(tracked);
        REQUIRE_FALSE(tracked.value().valid);
        REQUIRE(tracked.value().confidence < 0.15);
        REQUIRE(tracked.value().bpm == 0.0);
        REQUIRE(tracked.value().beatSeconds.empty());
    }
}

TEST_CASE("The octave ambiguity is settled towards the middle of the range", "[analysis][tempo]") {
    // A click every 0.25 s is equally consistent with 240, 120 and 60 BPM --
    // at 120 every other click is an off-beat, at 60 three in four are. The
    // weighting prefers the tempo nearest the geometric middle of the range,
    // which for 50 to 300 is sqrt(15000) = 122.5 BPM, so of the three it
    // chooses 120. That is this file's answer to the octave question and it is
    // a preference, not a measurement.
    TempoSettings wide;
    wide.minBpm = 50.0;
    wide.maxBpm = 300.0;

    const Material fast = clickTrain(240.0, 12.0, 0.5);
    const auto tracked = trackTempo(fast.audio.view(), kRate, wide);
    REQUIRE(tracked);
    REQUIRE(tracked.value().valid);
    REQUIRE(tracked.value().bpm == Approx(120.0).margin(1.0));

    // Every beat still lands on a click -- it is half the clicks that are
    // unaccounted for, not the grid that has drifted.
    for (const double beat : tracked.value().beatSeconds) {
        REQUIRE(distanceToNearest(fast.times, beat) <= kHopSeconds);
    }

    // And a 120 BPM train over the same wide range stays at 120 rather than
    // being dragged up to 240 or down to 60.
    const Material even = clickTrain(120.0, 12.0);
    const auto also = trackTempo(even.audio.view(), kRate, wide);
    REQUIRE(also);
    REQUIRE(also.value().valid);
    REQUIRE(also.value().bpm == Approx(120.0).margin(1.0));
}

TEST_CASE("What tempo tracking refuses", "[analysis][tempo]") {
    const Material material = clickTrain(120.0, 6.0);

    SECTION("nothing to analyse") {
        const AudioBuffer empty{ChannelLayout::mono(), 0};
        const auto tracked = trackTempo(empty.view(), kRate);
        REQUIRE_FALSE(tracked);
        REQUIRE(tracked.error().code() == ErrorCode::InvalidArgument);
        REQUIRE_FALSE(onsetEnvelope(empty.view(), kRate));
    }

    SECTION("shorter than one analysis window") {
        // 1023 samples against a 1024-sample window: one short, and refused
        // rather than answered from a window that was never filled.
        const AudioBuffer brief{ChannelLayout::mono(), 1023};
        const auto tracked = trackTempo(brief.view(), kRate);
        REQUIRE_FALSE(tracked);
        REQUIRE(tracked.error().code() == ErrorCode::InvalidArgument);
        REQUIRE_FALSE(onsetEnvelope(brief.view(), kRate));
    }

    SECTION("a channel that is not there") {
        REQUIRE_FALSE(trackTempo(material.audio.view(), kRate, {}, 1));
        REQUIRE(trackTempo(material.audio.view(), kRate, {}, 1).error().code() ==
                ErrorCode::OutOfRange);
        REQUIRE_FALSE(trackTempo(material.audio.view(), kRate, {}, -1));
        REQUIRE_FALSE(onsetEnvelope(material.audio.view(), kRate, {}, 1));
    }

    SECTION("no sample rate") {
        const auto tracked = trackTempo(material.audio.view(), SampleRate{0.0});
        REQUIRE_FALSE(tracked);
        REQUIRE(tracked.error().code() == ErrorCode::InvalidArgument);
        REQUIRE_FALSE(onsetEnvelope(material.audio.view(), SampleRate{0.0}));
    }

    SECTION("a tempo range that is not a range") {
        TempoSettings inverted;
        inverted.minBpm = 200.0;
        inverted.maxBpm = 100.0;
        const auto tracked = trackTempo(material.audio.view(), kRate, inverted);
        REQUIRE_FALSE(tracked);
        REQUIRE(tracked.error().code() == ErrorCode::InvalidArgument);

        TempoSettings degenerate;
        degenerate.minBpm = 120.0;
        degenerate.maxBpm = 120.0;
        REQUIRE_FALSE(trackTempo(material.audio.view(), kRate, degenerate));

        // The envelope does not refuse this: the tempo range cannot change it,
        // and refusing for a reason that cannot affect the answer would be a
        // lie about what was wrong.
        REQUIRE(onsetEnvelope(material.audio.view(), kRate, inverted));
    }
}

TEST_CASE("A buffer too short to hold several beats is not given a tempo", "[analysis][tempo]") {
    // Long enough to frame and analyse, far too short to be periodic: one
    // second at 60 BPM is one beat. This is not a refusal -- the input was
    // perfectly valid -- so it comes back as an answer of "no".
    const Material material = clickTrain(120.0, 1.0);
    const auto tracked = trackTempo(material.audio.view(), kRate);
    REQUIRE(tracked);
    REQUIRE_FALSE(tracked.value().valid);
    REQUIRE(tracked.value().beatSeconds.empty());
}
