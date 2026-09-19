#include <sa/ui/ChunkedPitch.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <random>
#include <string>
#include <vector>

using namespace sa;
using namespace sa::ui;

namespace {

constexpr SampleRate kRate{48000.0};
constexpr double kTwoPi = 2.0 * std::numbers::pi;

/// The chunk length trackPitchInChunks cuts at, recomputed here on purpose.
///
/// It is the whole number of hops nearest a second, and the lengths below are
/// written in terms of it -- four chunks and a fifth, one chunk less a sample,
/// a tail too short for a frame -- so that each case says which boundary it is
/// about instead of carrying a bare number of samples. A chunk that moved
/// would move every length with it, so the cases would stop straddling what
/// they were written to straddle without failing -- which is why the first one
/// below asserts how many chunks it actually spans.
[[nodiscard]] SampleCount chunkFrames(SampleRate rate, SampleCount hop) {
    return std::max<SampleCount>(hop, static_cast<SampleCount>(rate.hz()) / hop * hop);
}

/// What one frame of the tracker reads: the window plus the longest lag the
/// lowest pitch asks for. Audio shorter than this has no frame in it at all.
[[nodiscard]] SampleCount frameSpan(SampleRate rate, const analysis::PitchSettings& settings) {
    return settings.window + static_cast<SampleCount>(std::ceil(rate.hz() / settings.minHz));
}

/// Audio with a contour worth comparing: a glide through two octaves, in three
/// phrases with silence between them, over a little noise.
///
/// Every part of that earns its place. A steady tone would give every frame the
/// same reading, so two contours could agree on all of them while one of the
/// two was built wrongly. The glide makes each frame's answer different from
/// its neighbours' -- the assertions below check that hardly any two voiced
/// frames agree -- so a frame taken from the wrong place cannot match the frame
/// it was supposed to be. The silences put unvoiced frames in, which is the other
/// thing a frame can be. The noise floor stops the silences being exactly zero,
/// where the tracker's normalisation takes its one special case.
[[nodiscard]] AudioBuffer phrases(SampleCount frames, SampleRate rate = kRate) {
    AudioBuffer buffer{ChannelLayout::mono(), frames};
    std::mt19937 noise{20250919};
    std::uniform_real_distribution<double> hiss{-1e-4, 1e-4};

    const double seconds = static_cast<double>(frames) / rate.hz();
    double phase = 0.0;
    for (SampleCount i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate.hz();
        // Three phrases of equal length, each with its last eighth silent.
        const double within = std::fmod(t, seconds / 3.0) / (seconds / 3.0);
        // 150 Hz up to 600 Hz across the whole passage, so that no two frames
        // of the contour hold the same number.
        const double hz = 150.0 * std::pow(4.0, t / seconds);
        phase += kTwoPi * hz / rate.hz();
        const double sounding = within < 0.875 ? 1.0 : 0.0;
        // Two harmonics as well as the fundamental: a bare sine is the easiest
        // thing a pitch tracker ever sees, and the chunking has to hold for
        // material that makes it work.
        const double sample = sounding * (0.5 * std::sin(phase) + 0.25 * std::sin(2.0 * phase) +
                                          0.12 * std::sin(3.0 * phase));
        buffer.channel(0)[i] = static_cast<float>(sample + hiss(noise));
    }
    return buffer;
}

/// How many frames of a contour found a pitch, how many did not, and how many
/// different pitches there are among them.
struct Substance {
    std::size_t voiced = 0;
    std::size_t unvoiced = 0;
    std::size_t distinctHz = 0;
};

[[nodiscard]] Substance substanceOf(const std::vector<analysis::PitchPoint>& contour) {
    Substance found;
    std::vector<double> pitches;
    for (const analysis::PitchPoint& point : contour) {
        if (point.voiced) {
            ++found.voiced;
            pitches.push_back(point.hz);
        } else {
            ++found.unvoiced;
        }
    }
    std::sort(pitches.begin(), pitches.end());
    found.distinctHz =
        static_cast<std::size_t>(std::unique(pitches.begin(), pitches.end()) - pitches.begin());
    return found;
}

/// Compare a chunked contour against the whole-buffer one, frame for frame.
///
/// The three readings are held to bit equality rather than to a tolerance. A
/// frame reads the same bytes either way, so anything but the identical double
/// would mean the chunking had moved a frame, dropped one, or handed it
/// different samples -- and a tolerance is exactly what would hide that.
///
/// The time is the one field that cannot be held to that standard, and the
/// reason is arithmetic rather than anything about the audio: the whole-buffer
/// tracker divides once, and the chunked one divides the frame's own offset and
/// the chunk's and adds them, which is the same number reached by a different
/// route. Measured over the cases below it lands within one unit in the last
/// place -- under 10^-15 s, a ten-thousand-millionth of a sample. So the time
/// is held to two things instead, both of which a misplaced frame would break
/// by ten orders of magnitude: it converts to the same sample of the document,
/// which is the only thing anything drawn from the contour asks of it, and it
/// is inside a nanosecond, which is a fortieth of a thousandth of the 5.3 ms
/// step between one frame and the next.
void requireSameContour(const std::vector<analysis::PitchPoint>& chunked,
                        const std::vector<analysis::PitchPoint>& whole, const std::string& what) {
    INFO(what << ": " << chunked.size() << " chunked frames against " << whole.size());
    REQUIRE(chunked.size() == whole.size());

    for (std::size_t i = 0; i < whole.size(); ++i) {
        INFO(what << ": frame " << i << " of " << whole.size());
        REQUIRE(chunked[i].voiced == whole[i].voiced);
        REQUIRE(chunked[i].hz == whole[i].hz);
        REQUIRE(chunked[i].confidence == whole[i].confidence);
        REQUIRE(secondsToSamples(chunked[i].timeSeconds, kRate) ==
                secondsToSamples(whole[i].timeSeconds, kRate));
        REQUIRE(std::abs(chunked[i].timeSeconds - whole[i].timeSeconds) < 1e-9);
    }
}

/// Track `frames` of the same passage both ways and require the two to agree.
void requireChunkingChangesNothing(SampleCount frames, const analysis::PitchSettings& settings,
                                   const std::string& what) {
    const AudioBuffer audio = phrases(frames);
    CancellationToken nothing;

    const auto chunked = trackPitchInChunks(audio.constView(), kRate, settings, nothing);
    const auto whole = analysis::trackPitch(audio.constView(), kRate, settings);
    INFO(what << ": " << frames << " frames at a hop of " << settings.hop);
    REQUIRE(chunked);
    REQUIRE(whole);
    requireSameContour(chunked.value(), whole.value(), what);
}

} // namespace

TEST_CASE("A contour tracked in chunks is the contour tracked in one call", "[ui][chunkedpitch]") {
    // The panel's own settings, at the hop it uses for a passage of this
    // length, so that what is compared is what the window runs.
    analysis::PitchSettings settings;
    const SampleCount chunk = chunkFrames(kRate, settings.hop);

    // Four chunks and a fifth of one: several boundaries crossed, and a length
    // that is not a whole number of chunks -- which is the case where the last
    // chunk is shorter than the rest and the one before it still has to hand
    // over exactly the frames it owns and no others.
    const SampleCount frames = chunk * 4 + chunk / 5;
    REQUIRE(frames / chunk == 4);
    REQUIRE(frames % chunk != 0);
    // And at this length the panel would ask for exactly these settings, so
    // the comparison is of the contour the window draws and not of a
    // configuration chosen to make one.
    REQUIRE(pitchHopFor(frames, settings.hop) == settings.hop);

    const AudioBuffer audio = phrases(frames);
    CancellationToken nothing;
    const auto chunked = trackPitchInChunks(audio.constView(), kRate, settings, nothing);
    const auto whole = analysis::trackPitch(audio.constView(), kRate, settings);
    REQUIRE(chunked);
    REQUIRE(whole);

    // The comparison is about something. Most frames found a pitch, plenty
    // found none, and hardly any two voiced frames agree on what they found --
    // so a contour assembled out of the wrong frames could not match this one.
    const Substance found = substanceOf(whole.value());
    REQUIRE(found.voiced > whole.value().size() / 2);
    REQUIRE(found.unvoiced > 20);
    REQUIRE(found.distinctHz > found.voiced * 9 / 10);

    requireSameContour(chunked.value(), whole.value(), "four chunks and a fifth");
}

TEST_CASE("Chunking changes nothing at any of the lengths where it could", "[ui][chunkedpitch]") {
    // A coarser hop than the case above, which is the hop the panel uses for a
    // passage of a couple of minutes, and four times fewer frames to track for
    // each of the lengths below.
    analysis::PitchSettings settings;
    settings.hop = 1024;
    const SampleCount chunk = chunkFrames(kRate, settings.hop);
    const SampleCount span = frameSpan(kRate, settings);

    SECTION("a whole number of chunks") {
        requireChunkingChangesNothing(chunk * 3, settings, "three chunks exactly");
    }
    SECTION("a sample past a whole number of chunks") {
        // The chunk after the boundary holds one sample, which is far less than
        // a frame, so the tracker refuses it and the run has to end on what it
        // already had rather than on that refusal.
        requireChunkingChangesNothing(chunk * 2 + 1, settings, "two chunks and a sample");
    }
    SECTION("a tail one sample short of a frame") {
        requireChunkingChangesNothing(chunk * 2 + span - 1, settings, "a tail just under a frame");
    }
    SECTION("a tail one sample past a frame") {
        requireChunkingChangesNothing(chunk * 2 + span + 1, settings, "a tail just over a frame");
    }
    SECTION("a sample short of one chunk") {
        // One pass of the loop, which is the case that proves the chunked path
        // is not merely the whole-buffer path with extra arithmetic round it.
        requireChunkingChangesNothing(chunk - 1, settings, "one chunk less a sample");
    }
    SECTION("exactly one frame of audio") {
        requireChunkingChangesNothing(span, settings, "one frame exactly");
    }
}

TEST_CASE("Audio too short for a frame is refused the same way either way", "[ui][chunkedpitch]") {
    analysis::PitchSettings settings;
    settings.hop = 1024;
    const AudioBuffer audio = phrases(frameSpan(kRate, settings) - 1);
    CancellationToken nothing;

    const auto chunked = trackPitchInChunks(audio.constView(), kRate, settings, nothing);
    const auto whole = analysis::trackPitch(audio.constView(), kRate, settings);
    REQUIRE_FALSE(chunked);
    REQUIRE_FALSE(whole);
    // The same refusal, not merely a refusal: the chunked path is meant to hand
    // the tracker's own account of what was wrong back to the caller, and the
    // panel prints that sentence.
    REQUIRE(std::string{chunked.error().what()} == std::string{whole.error().what()});
}
