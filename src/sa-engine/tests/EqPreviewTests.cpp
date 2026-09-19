#include <sa/core/AudioBuffer.h>
#include <sa/core/RealtimeGuard.h>
#include <sa/dsp/ParametricEq.h>
#include <sa/engine/EqPreview.h>

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <thread>
#include <vector>

using namespace sa;
using namespace sa::engine;

namespace {

constexpr SampleRate kRate = kSampleRate48000;

[[nodiscard]] ChannelLayout layoutOf(int channels) {
    return channels == 1 ? ChannelLayout::mono() : ChannelLayout::stereo();
}

[[nodiscard]] AudioBuffer makeSine(int channels, SampleCount frames, double frequency,
                                   float amplitude) {
    AudioBuffer buffer{layoutOf(channels), frames};
    for (int channel = 0; channel < channels; ++channel) {
        float* out = buffer.channel(channel);
        const double offset = static_cast<double>(channel) * 0.37;
        for (SampleCount i = 0; i < frames; ++i) {
            const double phase =
                2.0 * std::numbers::pi * frequency * static_cast<double>(i) / kRate.hz() + offset;
            out[i] = amplitude * static_cast<float>(std::sin(phase));
        }
    }
    return buffer;
}

/// Deterministic broadband material. A filter change shows up far more plainly
/// against noise than against a tone, and a fixed generator keeps the numbers
/// in a failure message reproducible.
[[nodiscard]] AudioBuffer makeNoise(int channels, SampleCount frames, float amplitude) {
    AudioBuffer buffer{layoutOf(channels), frames};
    std::uint32_t state = 22695477u;
    for (int channel = 0; channel < channels; ++channel) {
        float* out = buffer.channel(channel);
        for (SampleCount i = 0; i < frames; ++i) {
            state = state * 1664525u + 1013904223u;
            const double unit = static_cast<double>(state >> 8) / 8388608.0 - 1.0;
            out[i] = amplitude * static_cast<float>(unit);
        }
    }
    return buffer;
}

[[nodiscard]] AudioBuffer copyOf(const AudioBuffer& source) {
    AudioBuffer copy{source.layout(), source.frames()};
    for (int channel = 0; channel < source.channelCount(); ++channel) {
        std::copy_n(source.channel(channel), source.frames(), copy.channel(channel));
    }
    return copy;
}

[[nodiscard]] std::vector<dsp::EqBand> peaking(double frequency, double gainDb, double q) {
    dsp::EqBand band;
    band.filter.type = dsp::FilterType::Peaking;
    band.filter.frequency = frequency;
    band.filter.gainDb = gainDb;
    band.filter.q = q;
    band.enabled = true;
    return {band};
}

/// The same bands over the same audio, offline and from rest -- which is what
/// the tool does today when a curve is applied to the document.
[[nodiscard]] AudioBuffer processOffline(const AudioBuffer& input,
                                         std::span<const dsp::EqBand> bands) {
    AudioBuffer output{input.layout(), input.frames()};
    for (int channel = 0; channel < input.channelCount(); ++channel) {
        Result<dsp::ParametricEq> eq = dsp::ParametricEq::create(kRate);
        REQUIRE(eq.hasValue());
        for (const dsp::EqBand& band : bands) {
            REQUIRE(eq.value().addBand(band).hasValue());
        }
        eq.value().process(input.channel(channel), output.channel(channel), input.frames());
    }
    return output;
}

void processRange(EqPreview& preview, AudioBuffer& audio, SampleCount from, SampleCount to,
                  SampleCount blockFrames) {
    for (SampleCount at = from; at < to; at += blockFrames) {
        const SampleCount count = std::min(blockFrames, to - at);
        preview.process(audio.view().subRange(at, count));
    }
}

[[nodiscard]] float largestStep(const float* samples, SampleCount from, SampleCount to) {
    float largest = 0.0f;
    for (SampleCount i = from + 1; i < to; ++i) {
        largest = std::max(largest, std::abs(samples[i] - samples[i - 1]));
    }
    return largest;
}

[[nodiscard]] float largestDifference(const float* a, const float* b, SampleCount from,
                                      SampleCount to) {
    float largest = 0.0f;
    for (SampleCount i = from; i < to; ++i) {
        largest = std::max(largest, std::abs(a[i] - b[i]));
    }
    return largest;
}

[[nodiscard]] SampleCount firstDifference(const float* a, const float* b, SampleCount count) {
    for (SampleCount i = 0; i < count; ++i) {
        if (a[i] != b[i]) {
            return i;
        }
    }
    return -1;
}

} // namespace

TEST_CASE("A preview refuses settings it cannot realise", "[engine][preview][eq]") {
    EqPreview preview;

    SECTION("bands before prepare") {
        const auto bands = peaking(1000.0, 6.0, 1.0);
        const Status status = preview.setBands(bands);
        CHECK_FALSE(status.ok());
        CHECK(status.error().code() == ErrorCode::InvalidArgument);
    }

    SECTION("a sample rate that is not one") {
        CHECK_FALSE(preview.prepare(SampleRate{0.0}, 2, 512).ok());
        CHECK_FALSE(preview.prepare(SampleRate{-48000.0}, 2, 512).ok());
        CHECK_FALSE(preview.isPrepared());
    }

    SECTION("a channel count the stage does not carry") {
        CHECK_FALSE(preview.prepare(kRate, 0, 512).ok());
        CHECK_FALSE(preview.prepare(kRate, -1, 512).ok());
        CHECK_FALSE(preview.prepare(kRate, kMaxPreviewChannels + 1, 512).ok());
        CHECK(preview.prepare(kRate, kMaxPreviewChannels, 512).ok());
    }

    SECTION("a block size that is not one") {
        CHECK_FALSE(preview.prepare(kRate, 2, 0).ok());
        CHECK_FALSE(preview.prepare(kRate, 2, -512).ok());
        CHECK_FALSE(preview.prepare(kRate, 2, kMaxPreviewBlockFrames + 1).ok());
    }

    SECTION("bands that cannot be designed") {
        REQUIRE(preview.prepare(kRate, 1, 512).ok());
        const auto good = peaking(1000.0, 6.0, 1.0);
        REQUIRE(preview.setBands(good).ok());
        REQUIRE(preview.publishedSettings().sectionCount == 1);

        const std::vector<std::vector<dsp::EqBand>> nonsense{
            peaking(0.0, 6.0, 1.0),
            peaking(-1000.0, 6.0, 1.0),
            peaking(kRate.hz() * 0.5, 6.0, 1.0),
            peaking(kRate.hz(), 6.0, 1.0),
            peaking(std::numeric_limits<double>::quiet_NaN(), 6.0, 1.0),
            peaking(1000.0, 6.0, 0.0),
            peaking(1000.0, 6.0, -1.0),
            peaking(1000.0, std::numeric_limits<double>::infinity(), 1.0),
        };
        for (const std::vector<dsp::EqBand>& bands : nonsense) {
            CHECK_FALSE(preview.setBands(bands).ok());
        }

        // A refusal must leave the curve that is already playing alone -- the
        // audio thread never sees half a change.
        const Result<dsp::BiquadCoefficients> expected =
            dsp::BiquadCoefficients::peaking(kRate, 1000.0, 1.0, 6.0);
        REQUIRE(expected.hasValue());
        CHECK(preview.publishedSettings().sectionCount == 1);
        CHECK(preview.publishedSettings().sections[0].b0 == expected.value().b0);
        CHECK(preview.publishedSettings().sections[0].a2 == expected.value().a2);
    }

    SECTION("more bands than the equaliser holds") {
        REQUIRE(preview.prepare(kRate, 1, 512).ok());
        std::vector<dsp::EqBand> bands;
        for (int i = 0; i < EqPreview::kMaxBands + 1; ++i) {
            bands.push_back(peaking(100.0 + 100.0 * static_cast<double>(i), 1.0, 1.0).front());
        }
        const Status status = preview.setBands(bands);
        CHECK_FALSE(status.ok());
        CHECK(status.error().code() == ErrorCode::OutOfRange);

        bands.pop_back();
        CHECK(preview.setBands(bands).ok());
    }

    SECTION("an empty curve is not nonsense") {
        REQUIRE(preview.prepare(kRate, 1, 512).ok());
        CHECK(preview.setBands(std::span<const dsp::EqBand>{}).ok());
        CHECK(preview.publishedSettings().sectionCount == 0);
    }
}

TEST_CASE("An unprepared preview leaves audio alone", "[engine][preview][eq]") {
    EqPreview preview;
    AudioBuffer audio = makeSine(1, 512, 440.0, 0.5f);
    const AudioBuffer original = copyOf(audio);

    preview.process(audio.view());

    CHECK(firstDifference(audio.channel(0), original.channel(0), 512) == -1);
}

TEST_CASE("Bypass passes the signal through sample for sample", "[engine][preview][eq]") {
    constexpr SampleCount kFrames = 48000;
    constexpr SampleCount kBlock = 512;

    EqPreview preview;
    REQUIRE(preview.prepare(kRate, 2, kBlock).ok());
    // A curve loud enough that any leakage at all would be obvious.
    REQUIRE(preview.setBands(peaking(1000.0, 18.0, 1.0)).ok());
    REQUIRE(preview.isBypassed());

    AudioBuffer audio = makeNoise(2, kFrames, 0.4f);
    const AudioBuffer original = copyOf(audio);

    processRange(preview, audio, 0, kFrames, kBlock);
    for (int channel = 0; channel < 2; ++channel) {
        CHECK(firstDifference(audio.channel(channel), original.channel(channel), kFrames) == -1);
    }
    CHECK(preview.refusedBlocks() == 0);

    // Switch in, hear something, switch out again, and check that what comes
    // back is the input and not something within a hair of it.
    preview.setBypassed(false);
    processRange(preview, audio, 0, kFrames, kBlock);
    CHECK(largestDifference(audio.channel(0), original.channel(0), 0, kFrames) > 0.1f);

    AudioBuffer again = copyOf(original);
    preview.setBypassed(true);
    processRange(preview, again, 0, kFrames, kBlock);

    // The fade out of the processed path spans the first block; after it the
    // signal is untouched, not merely close.
    const SampleCount settled = kBlock;
    for (int channel = 0; channel < 2; ++channel) {
        CHECK(largestDifference(again.channel(channel), original.channel(channel), settled,
                                kFrames) == 0.0f);
    }
}

TEST_CASE("The callback allocates nothing", "[engine][preview][eq][rt]") {
    if (!rt::checksEnabled()) {
        SUCCEED("SA_RT_SAFETY_CHECKS is off in this build; detection not compiled in");
        return;
    }

    constexpr SampleCount kBlock = 256;
    EqPreview preview;
    REQUIRE(preview.prepare(kRate, 2, kBlock).ok());
    REQUIRE(preview.setBands(peaking(800.0, 9.0, 1.4)).ok());

    AudioBuffer audio = makeNoise(2, kBlock, 0.3f);

    // Every path the callback has: collecting a value, adopting one outright,
    // running the filters, starting and finishing a crossfade, bypassing for
    // free, and refusing a block whose shape is wrong.
    AudioBuffer wrongShape{ChannelLayout::mono(), kBlock};
    std::size_t allocations = 0;
    {
        const rt::ScopedAudioThread guard;
        const rt::AllocationScope scope;

        preview.process(audio.view()); // cold adopt, bypassed
        preview.process(audio.view()); // nothing to collect, free
        preview.process(wrongShape.view());
        allocations = scope.count();
    }
    CHECK(allocations == 0);

    preview.setBypassed(false);
    {
        const rt::ScopedAudioThread guard;
        const rt::AllocationScope scope;
        for (int i = 0; i < 8; ++i) {
            preview.process(audio.view()); // fade in, then steady
        }
        allocations = scope.count();
    }
    CHECK(allocations == 0);

    REQUIRE(preview.setBands(peaking(3000.0, -12.0, 4.0)).ok());
    {
        const rt::ScopedAudioThread guard;
        const rt::AllocationScope scope;
        for (int i = 0; i < 8; ++i) {
            preview.process(audio.view()); // crossfade to the new curve
        }
        allocations = scope.count();
    }
    CHECK(allocations == 0);

    // A block longer than the one prepare() was told about, so the piecewise
    // path runs too.
    AudioBuffer longBlock = makeNoise(2, kBlock * 5 + 17, 0.3f);
    {
        const rt::ScopedAudioThread guard;
        const rt::AllocationScope scope;
        preview.process(longBlock.view());
        allocations = scope.count();
    }
    CHECK(allocations == 0);

    CHECK(preview.refusedBlocks() == 1);
}

TEST_CASE("A setting changed between blocks is heard, and agrees with the offline result",
          "[engine][preview][eq]") {
    constexpr SampleCount kFrames = 96000; // two seconds
    constexpr SampleCount kBlock = 512;
    constexpr SampleCount kChangeAt = 24000;

    const std::vector<dsp::EqBand> before = peaking(400.0, -9.0, 1.0);
    const std::vector<dsp::EqBand> after = peaking(2500.0, 9.0, 2.0);

    const AudioBuffer input = makeNoise(1, kFrames, 0.3f);
    const AudioBuffer offlineBefore = processOffline(input, before);
    const AudioBuffer offlineAfter = processOffline(input, after);

    EqPreview preview;
    REQUIRE(preview.prepare(kRate, 1, kBlock).ok());
    REQUIRE(preview.setBands(before).ok());
    preview.setBypassed(false);

    AudioBuffer audio = copyOf(input);
    processRange(preview, audio, 0, kChangeAt, kBlock);

    // Settings in hand when the stream starts are the settings, not a change,
    // so the first block adopts them outright. Nothing has been faded and the
    // filters ran from rest, which is exactly the offline arrangement -- so
    // this half is not merely close, it is the same arithmetic.
    CHECK(firstDifference(audio.channel(0), offlineBefore.channel(0), kChangeAt) == -1);

    REQUIRE(preview.setBands(after).ok());
    processRange(preview, audio, kChangeAt, kFrames, kBlock);

    // The change was heard: the tail is nowhere near what the old curve would
    // have produced.
    const SampleCount settled = kChangeAt + preview.crossfadeFrames() + 12000;
    CHECK(largestDifference(audio.channel(0), offlineBefore.channel(0), settled, kFrames) > 0.02f);

    // And it is the new curve that is being heard. Not exact, for two reasons:
    // the crossfade region is a blend of both curves, which is why the
    // comparison starts after it; and after it the preview's filters carry the
    // state the previous curve left behind, where the offline run started from
    // rest. That difference is the filter's homogeneous response and decays
    // with its poles -- a quarter of a second is hundreds of time constants
    // for any band this test uses, which puts the residue far below the
    // tolerance and far below audibility (1e-5 is about -100 dBFS).
    const float residue =
        largestDifference(audio.channel(0), offlineAfter.channel(0), settled, kFrames);
    INFO("largest difference from the offline render: " << residue);
    CHECK(residue < 1.0e-5f);
}

TEST_CASE("A parameter change puts no step in the output", "[engine][preview][eq]") {
    constexpr SampleCount kFrames = 48000;
    constexpr SampleCount kBlock = 480;
    constexpr SampleCount kChangeAt = 24000;
    constexpr double kTone = 300.0;

    // Both curves are cuts, so neither can make the tone steeper than it
    // already is: a peaking filter at negative gain has a magnitude of at most
    // one everywhere, and a sine through a filter of magnitude m has slew m
    // times its own.
    const std::vector<dsp::EqBand> before = peaking(kTone, -18.0, 0.9);
    const std::vector<dsp::EqBand> after = peaking(900.0, -18.0, 0.9);

    const AudioBuffer input = makeSine(1, kFrames, kTone, 0.5f);
    const AudioBuffer offlineBefore = processOffline(input, before);
    const AudioBuffer offlineAfter = processOffline(input, after);

    EqPreview preview;
    REQUIRE(preview.prepare(kRate, 1, kBlock).ok());
    REQUIRE(preview.setBands(before).ok());
    preview.setBypassed(false);

    AudioBuffer audio = copyOf(input);
    processRange(preview, audio, 0, kChangeAt, kBlock);
    REQUIRE(preview.setBands(after).ok());
    processRange(preview, audio, kChangeAt, kFrames, kBlock);

    // Measured from a point where the filters have long settled, so that the
    // only candidate for a step is the change itself.
    const SampleCount from = 2400;
    const float inputStep = largestStep(input.channel(0), from, kFrames);
    const float previewStep = largestStep(audio.channel(0), from, kFrames);

    // The test has teeth only if swapping the coefficients outright would have
    // been audible. That jump is the distance between the two curves' outputs
    // at the moment of the change.
    const float unramped =
        largestDifference(offlineBefore.channel(0), offlineAfter.channel(0), from, kFrames);
    INFO("input step " << inputStep << ", preview step " << previewStep
                       << ", an unramped swap would step " << unramped);
    REQUIRE(unramped > 5.0f * inputStep);

    // out[n] = (1-w)A[n] + w B[n], so
    //   |out[n] - out[n-1]| <= max(|dA|, |dB|) + |dw| * max|B - A|,
    // and |dw| is one over the crossfade length. Anything above this is a step
    // the ramp does not account for.
    const float rampTerm = unramped / static_cast<float>(preview.crossfadeFrames());
    const float bound = std::max(largestStep(offlineBefore.channel(0), from, kFrames),
                                 largestStep(offlineAfter.channel(0), from, kFrames)) +
                        rampTerm;
    CHECK(previewStep <= bound);

    // Which for these two curves comes out below the slew the tone already
    // has, so the plain statement holds as well: the change adds no step the
    // material did not have.
    CHECK(previewStep <= inputStep);
}

TEST_CASE("Block size does not change the output", "[engine][preview][eq]") {
    constexpr SampleCount kFrames = 24000;
    const std::vector<dsp::EqBand> bands = peaking(1200.0, 8.0, 1.6);
    const AudioBuffer input = makeNoise(2, kFrames, 0.35f);

    const auto run = [&](SampleCount prepareFrames, SampleCount blockFrames) {
        EqPreview preview;
        REQUIRE(preview.prepare(kRate, 2, prepareFrames).ok());
        REQUIRE(preview.setBands(bands).ok());
        preview.setBypassed(false);
        AudioBuffer audio = copyOf(input);
        processRange(preview, audio, 0, kFrames, blockFrames);
        return audio;
    };

    const AudioBuffer reference = run(512, 512);
    // The last of these is longer than the stage was prepared for, so it
    // exercises the piecewise path as well.
    for (const SampleCount blockFrames :
         {SampleCount{1}, SampleCount{16}, SampleCount{64}, SampleCount{999}, SampleCount{4096}}) {
        const AudioBuffer other = run(512, blockFrames);
        for (int channel = 0; channel < 2; ++channel) {
            INFO("block size " << blockFrames << ", channel " << channel);
            CHECK(firstDifference(reference.channel(channel), other.channel(channel), kFrames) ==
                  -1);
        }
    }
}

TEST_CASE("A crossfaded change lands in the same place at any block size",
          "[engine][preview][eq]") {
    // A change is collected at a block boundary, so it can only be compared
    // across block sizes that share the boundary it arrives on. 4096 is a
    // multiple of every size used here, which makes the comparison meaningful
    // rather than a coincidence.
    constexpr SampleCount kFrames = 24000;
    constexpr SampleCount kChangeAt = 4096;

    const std::vector<dsp::EqBand> before = peaking(500.0, -6.0, 1.0);
    const std::vector<dsp::EqBand> after = peaking(4000.0, 10.0, 0.8);
    const AudioBuffer input = makeNoise(1, kFrames, 0.35f);

    const auto run = [&](SampleCount blockFrames) {
        EqPreview preview;
        REQUIRE(preview.prepare(kRate, 1, 512).ok());
        REQUIRE(preview.setBands(before).ok());
        preview.setBypassed(false);
        AudioBuffer audio = copyOf(input);
        processRange(preview, audio, 0, kChangeAt, blockFrames);
        REQUIRE(preview.setBands(after).ok());
        processRange(preview, audio, kChangeAt, kFrames, blockFrames);
        return audio;
    };

    const AudioBuffer reference = run(512);
    for (const SampleCount blockFrames : {SampleCount{16}, SampleCount{64}, SampleCount{2048}}) {
        const AudioBuffer other = run(blockFrames);
        INFO("block size " << blockFrames);
        CHECK(firstDifference(reference.channel(0), other.channel(0), kFrames) == -1);
    }
}

TEST_CASE("A block the stage was not prepared for passes through and is counted",
          "[engine][preview][eq]") {
    EqPreview preview;
    REQUIRE(preview.prepare(kRate, 1, 512).ok());
    REQUIRE(preview.setBands(peaking(1000.0, 12.0, 1.0)).ok());
    preview.setBypassed(false);

    AudioBuffer stereo = makeNoise(2, 512, 0.3f);
    const AudioBuffer original = copyOf(stereo);
    preview.process(stereo.view());

    for (int channel = 0; channel < 2; ++channel) {
        CHECK(firstDifference(stereo.channel(channel), original.channel(channel), 512) == -1);
    }
    CHECK(preview.refusedBlocks() == 1);
}

TEST_CASE("Settings and blocks cross threads at once", "[engine][preview][eq][threads]") {
    // Built for ThreadSanitizer. One thread changes the curve as fast as it
    // can while another pulls blocks through it; anything shared between them
    // that is not the slot would be reported here.
    constexpr SampleCount kBlock = 128;

    EqPreview preview;
    REQUIRE(preview.prepare(kRate, 2, kBlock).ok());
    REQUIRE(preview.setBands(peaking(1000.0, 6.0, 1.0)).ok());
    preview.setBypassed(false);

    const AudioBuffer source = makeNoise(2, kBlock, 0.3f);
    std::atomic<bool> running{true};
    std::atomic<std::uint64_t> changes{0};

    std::thread writer{[&] {
        std::uint64_t count = 0;
        while (running.load(std::memory_order_relaxed)) {
            const double frequency = 200.0 + static_cast<double>(count % 8000u);
            const double gain = -18.0 + static_cast<double>(count % 37u);
            if (preview.setBands(peaking(frequency, gain, 0.7)).ok()) {
                ++count;
            }
            if (count % 64 == 0) {
                preview.setBypassed((count / 64) % 2 == 0);
            }
        }
        changes.store(count, std::memory_order_relaxed);
    }};

    AudioBuffer audio{ChannelLayout::stereo(), kBlock};
    std::uint64_t blocks = 0;
    bool sane = true;
    {
        const rt::ScopedAudioThread guard;
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < until) {
            for (int channel = 0; channel < 2; ++channel) {
                std::copy_n(source.channel(channel), kBlock, audio.channel(channel));
            }
            preview.process(audio.view());
            for (int channel = 0; channel < 2 && sane; ++channel) {
                const float* samples = audio.channel(channel);
                for (SampleCount i = 0; i < kBlock; ++i) {
                    if (!std::isfinite(samples[i]) || std::abs(samples[i]) > 8.0f) {
                        sane = false;
                        break;
                    }
                }
            }
            ++blocks;
        }
    }

    running.store(false, std::memory_order_relaxed);
    writer.join();

    CHECK(sane);
    CHECK(blocks > 100);
    CHECK(changes.load(std::memory_order_relaxed) > 100);
    CHECK(preview.refusedBlocks() == 0);
}

TEST_CASE("Resetting clears what was ringing", "[engine][preview][eq]") {
    constexpr SampleCount kBlock = 256;
    EqPreview preview;
    REQUIRE(preview.prepare(kRate, 1, kBlock).ok());
    // A narrow band rings for a long time, so state left behind would be
    // plainly visible in the silence that follows.
    REQUIRE(preview.setBands(peaking(200.0, 18.0, 12.0)).ok());
    preview.setBypassed(false);

    AudioBuffer loud = makeSine(1, kBlock, 200.0, 0.8f);
    preview.process(loud.view());

    AudioBuffer silence{ChannelLayout::mono(), kBlock};
    silence.clear();
    preview.reset();
    preview.process(silence.view());

    float loudest = 0.0f;
    for (SampleCount i = 0; i < kBlock; ++i) {
        loudest = std::max(loudest, std::abs(silence.channel(0)[i]));
    }
    CHECK(loudest == 0.0f);
}
