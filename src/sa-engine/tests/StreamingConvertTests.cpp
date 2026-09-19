#include <sa/core/Cancellation.h>
#include <sa/core/RealtimeGuard.h>
#include <sa/dsp/Resampler.h>
#include <sa/engine/StreamingConvert.h>
#include <sa/io/AudioSource.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string>
#include <vector>

using namespace sa;
using namespace sa::engine;

namespace {

/// A source whose sample at a frame is a function of that frame's index alone.
///
/// Every test here turns on reading the same audio two different ways, so a
/// source that answered differently depending on how it was asked would make
/// the comparisons meaningless. Computing from the absolute frame index rules
/// that out by construction.
///
/// Several partials rather than one tone, so that a conversion has something
/// across the band to get wrong, and all of them well below the lower of any
/// rate pair used here, so nothing is testing the anti-alias filter by
/// accident.
class ToneSource final : public io::AudioSource {
public:
    ToneSource(int channels, SampleCount frames, SampleRate rate, bool mirrorRight = false)
        : mirrorRight_(mirrorRight) {
        info_.sampleRate = rate;
        info_.layout = channels == 1   ? ChannelLayout::mono()
                       : channels == 2 ? ChannelLayout::stereo()
                                       : ChannelLayout::discrete(channels);
        info_.frameCount = frames;
        info_.format = io::SampleFormat::Float32;
    }

    [[nodiscard]] const io::AudioFileInfo& info() const noexcept override { return info_; }

    [[nodiscard]] Result<SampleCount> read(SampleIndex startFrame,
                                           AudioBufferView destination) const override {
        if (startFrame < 0) {
            return Error{ErrorCode::OutOfRange, "startFrame is negative"};
        }
        widestRead_ = std::max(widestRead_, destination.frames());
        if (startFrame >= info_.frameCount || destination.isEmpty()) {
            return SampleCount{0};
        }

        const SampleCount count =
            std::min<SampleCount>(destination.frames(), info_.frameCount - startFrame);
        for (int channel = 0; channel < destination.channelCount(); ++channel) {
            float* out = destination.channel(channel);
            for (SampleCount i = 0; i < count; ++i) {
                out[i] = sampleAt(channel, startFrame + i);
            }
        }
        return count;
    }

    [[nodiscard]] float sampleAt(int channel, SampleIndex frame) const noexcept {
        if (mirrorRight_ && channel == 1) {
            return -sampleAt(0, frame);
        }
        const double seconds = static_cast<double>(frame) / info_.sampleRate.hz();
        const double base = channel == 0 ? 440.0 : 613.0;
        const double turn = 2.0 * std::numbers::pi * seconds;
        return static_cast<float>(0.5 * std::sin(turn * base) +
                                  0.25 * std::sin(turn * base * 5.0 + 0.4) +
                                  0.125 * std::sin(turn * base * 11.0 + 1.1));
    }

    /// The most frames asked for in a single read, which is what bounds the
    /// converter's input block.
    [[nodiscard]] SampleCount widestRead() const noexcept { return widestRead_; }

private:
    io::AudioFileInfo info_;
    bool mirrorRight_ = false;
    mutable SampleCount widestRead_ = 0;
};

/// Keeps every frame written, so a streamed result can be compared with an
/// in-memory one sample for sample.
class RecordingSink final : public AudioSink {
public:
    explicit RecordingSink(int channels) : channels_(static_cast<std::size_t>(channels)) {}

    [[nodiscard]] Status write(ConstAudioBufferView frames) override {
        // Both of these are promises the header makes to a sink. Holding it to
        // them here costs nothing and means no other test has to.
        if (frames.channelCount() != static_cast<int>(channels_.size())) {
            return Error{ErrorCode::InvalidArgument, "the channel count changed mid-stream"};
        }
        if (frames.frames() <= 0) {
            return Error{ErrorCode::InvalidArgument, "an empty write"};
        }

        ++writes_;
        widestWrite_ = std::max(widestWrite_, frames.frames());
        for (int channel = 0; channel < frames.channelCount(); ++channel) {
            const float* in = frames.channel(channel);
            auto& out = channels_[static_cast<std::size_t>(channel)];
            out.insert(out.end(), in, in + frames.frames());
        }
        return {};
    }

    [[nodiscard]] const std::vector<float>& channel(int index) const {
        return channels_[static_cast<std::size_t>(index)];
    }

    [[nodiscard]] SampleCount frames() const noexcept {
        return channels_.empty() ? 0 : static_cast<SampleCount>(channels_.front().size());
    }

    [[nodiscard]] int writes() const noexcept { return writes_; }

    [[nodiscard]] SampleCount widestWrite() const noexcept { return widestWrite_; }

private:
    std::vector<std::vector<float>> channels_;
    int writes_ = 0;
    SampleCount widestWrite_ = 0;
};

/// Counts what it is given and keeps none of it, so that a conversion long
/// enough to be interesting does not measure the test's own storage.
class CountingSink final : public AudioSink {
public:
    [[nodiscard]] Status write(ConstAudioBufferView frames) override {
        ++writes_;
        written_ += frames.frames();
        widestWrite_ = std::max(widestWrite_, frames.frames());
        return {};
    }

    [[nodiscard]] SampleCount frames() const noexcept { return written_; }

    [[nodiscard]] int writes() const noexcept { return writes_; }

    [[nodiscard]] SampleCount widestWrite() const noexcept { return widestWrite_; }

private:
    SampleCount written_ = 0;
    int writes_ = 0;
    SampleCount widestWrite_ = 0;
};

/// A conversion's output, channel by channel.
struct Converted {
    std::vector<std::vector<float>> channels;
    SampleCount frames = 0;
};

/// Convert the whole source in memory: one converter per channel, each fed its
/// entire channel in a single call, then drained.
///
/// This is the path sa-cli takes today and the thing a streamed conversion has
/// to match. The output buffer is deliberately generous rather than sized from
/// the ratio, because how many frames the conversion owes is one of the things
/// under test and a buffer that decided it in advance would answer the question
/// for it.
[[nodiscard]] Converted convertInMemory(const io::AudioSource& source, SampleRate outputRate,
                                        dsp::ResamplerQuality quality) {
    const io::AudioFileInfo& info = source.info();
    AudioBuffer whole{info.layout, info.frameCount};
    const auto read = source.read(0, whole.view());
    REQUIRE(read.hasValue());
    REQUIRE(read.value() == info.frameCount);

    const double ratio = outputRate.hz() / info.sampleRate.hz();
    const auto capacity =
        static_cast<SampleCount>(std::ceil(static_cast<double>(info.frameCount) * ratio)) + 64;

    Converted result;
    result.channels.resize(static_cast<std::size_t>(info.channelCount()));

    for (int channel = 0; channel < info.channelCount(); ++channel) {
        dsp::ResamplerSpec spec;
        spec.inputRate = info.sampleRate;
        spec.outputRate = outputRate;
        spec.quality = quality;

        auto converter = dsp::Resampler::create(spec);
        REQUIRE(converter.hasValue());

        std::vector<float> out(static_cast<std::size_t>(capacity), 0.0f);
        SampleCount consumed = 0;
        SampleCount produced = 0;
        while (consumed < info.frameCount) {
            const auto step = converter.value().process(whole.channel(channel) + consumed,
                                                        info.frameCount - consumed,
                                                        out.data() + produced, capacity - produced);
            if (step.inputConsumed == 0 && step.outputProduced == 0) {
                break;
            }
            consumed += step.inputConsumed;
            produced += step.outputProduced;
        }
        while (true) {
            const SampleCount drained =
                converter.value().flush(out.data() + produced, capacity - produced);
            if (drained <= 0) {
                break;
            }
            produced += drained;
        }

        out.resize(static_cast<std::size_t>(produced));
        result.channels[static_cast<std::size_t>(channel)] = std::move(out);
        result.frames = produced;
    }
    return result;
}

/// Compare two channels sample for sample, reporting the first difference
/// rather than the last.
///
/// A block hand-off that loses filter state clicks at the first block boundary
/// and at every one after it, so the index of the first difference and the
/// number of them together say what went wrong. One assertion rather than one
/// per sample, because fifty thousand passing Catch assertions per channel
/// would bury the run.
void requireIdentical(const std::vector<float>& actual, const std::vector<float>& expected,
                      const std::string& what) {
    INFO(what);
    REQUIRE(actual.size() == expected.size());

    std::size_t differences = 0;
    std::size_t first = 0;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (actual[i] != expected[i]) {
            if (differences == 0) {
                first = i;
            }
            ++differences;
        }
    }

    INFO(what << ": " << differences << " of " << actual.size()
              << " samples differ, first at index " << first);
    REQUIRE(differences == 0);
}

/// ceil(frames * output / input), in integers.
///
/// In doubles the exact cases sit on the boundary -- 44100 frames at 44100 to
/// 48000 is exactly 48000 output frames -- and land either side of it depending
/// on which way the ratio rounded, which would make this a test of arithmetic
/// rather than of the converter.
[[nodiscard]] SampleCount expectedFrames(SampleCount frames, SampleRate input, SampleRate output) {
    const auto in = static_cast<std::int64_t>(input.hz());
    const auto out = static_cast<std::int64_t>(output.hz());
    return (frames * out + in - 1) / in;
}

struct RatePair {
    SampleRate input;
    SampleRate output;
    const char* name;
};

constexpr SampleCount kTestFrames = 24000;

} // namespace

TEST_CASE("A streamed conversion is sample for sample the in-memory one", "[engine][streaming]") {
    const RatePair pairs[] = {
        {SampleRate{44100.0}, SampleRate{48000.0}, "44100 -> 48000"},
        {SampleRate{48000.0}, SampleRate{44100.0}, "48000 -> 44100"},
        {SampleRate{48000.0}, SampleRate{96000.0}, "48000 -> 96000"},
        {SampleRate{96000.0}, SampleRate{48000.0}, "96000 -> 48000"},
        // 320/147 once reduced: no small common factor to make the phase land
        // on easy numbers.
        {SampleRate{44100.0}, SampleRate{96000.0}, "44100 -> 96000"},
        // Not a whole number of hertz, which is what a drift correction looks
        // like. The converter cannot step its phase in integers here and
        // accumulates in a double instead, so it exercises the other half of
        // the phase arithmetic.
        {SampleRate{48000.0}, SampleRate{48019.2}, "48000 -> 48019.2"},
    };

    // Once in a single block -- the case with no hand-off to get wrong -- and
    // once in blocks of a prime that lines up with neither the rate ratio nor
    // the filter's stride, so every pair here is converted across boundaries as
    // well as within one.
    const SampleCount blockSizes[] = {kDefaultConversionBlockFrames, 4099};

    for (const RatePair& pair : pairs) {
        const ToneSource source{2, kTestFrames, pair.input};
        const Converted reference =
            convertInMemory(source, pair.output, dsp::ResamplerQuality::Best);

        for (const SampleCount blockFrames : blockSizes) {
            const std::string label =
                std::string{pair.name} + " in blocks of " + std::to_string(blockFrames);
            INFO(label);

            ConversionSpec spec;
            spec.outputRate = pair.output;
            spec.blockFrames = blockFrames;
            RecordingSink sink{2};
            const auto progress = convertStreaming(source, sink, spec);

            REQUIRE(progress.hasValue());
            CHECK(progress.value().framesRead == kTestFrames);
            CHECK(progress.value().framesWritten == reference.frames);
            for (int channel = 0; channel < 2; ++channel) {
                requireIdentical(sink.channel(channel),
                                 reference.channels[static_cast<std::size_t>(channel)],
                                 label + ", channel " + std::to_string(channel));
            }
        }
    }
}

TEST_CASE("The result does not depend on how the input was chopped up", "[engine][streaming]") {
    // The test most likely to catch a real bug: every one of these feeds the
    // converter the same samples in the same order and differs only in where
    // the calls fall. A block size of one puts a boundary between every pair of
    // samples; 4099 is prime, so no boundary lines up with the filter's stride
    // or with the rate ratio; a block wider than the file leaves none at all.
    const SampleCount blockSizes[] = {1, 7, 1024, 4096, 4099, 65536, 1'000'000};
    const RatePair pairs[] = {
        {SampleRate{44100.0}, SampleRate{48000.0}, "44100 -> 48000"},
        {SampleRate{48000.0}, SampleRate{44100.0}, "48000 -> 44100"},
    };

    for (const RatePair& pair : pairs) {
        const ToneSource source{2, kTestFrames, pair.input};
        const Converted reference =
            convertInMemory(source, pair.output, dsp::ResamplerQuality::Best);

        for (const SampleCount blockFrames : blockSizes) {
            const std::string label =
                std::string{pair.name} + " in blocks of " + std::to_string(blockFrames);
            INFO(label);

            ConversionSpec spec;
            spec.outputRate = pair.output;
            spec.blockFrames = blockFrames;
            RecordingSink sink{2};
            const auto progress = convertStreaming(source, sink, spec);

            REQUIRE(progress.hasValue());
            CHECK(progress.value().framesWritten == reference.frames);
            for (int channel = 0; channel < 2; ++channel) {
                requireIdentical(sink.channel(channel),
                                 reference.channels[static_cast<std::size_t>(channel)],
                                 label + ", channel " + std::to_string(channel));
            }
        }
    }
}

TEST_CASE("The output is as long as the ratio implies, tail included", "[engine][streaming]") {
    const RatePair pairs[] = {
        {SampleRate{44100.0}, SampleRate{48000.0}, "44100 -> 48000"},
        {SampleRate{48000.0}, SampleRate{44100.0}, "48000 -> 44100"},
        {SampleRate{48000.0}, SampleRate{96000.0}, "48000 -> 96000"},
        {SampleRate{96000.0}, SampleRate{48000.0}, "96000 -> 48000"},
        {SampleRate{44100.0}, SampleRate{96000.0}, "44100 -> 96000"},
    };
    // One count that divides the ratio exactly and one that does not, because
    // the rounding at the end is only visible in the second.
    const SampleCount frameCounts[] = {44100, 12345};

    for (const RatePair& pair : pairs) {
        for (const SampleCount frames : frameCounts) {
            INFO(pair.name << " over " << frames << " frames");
            const ToneSource source{1, frames, pair.input};

            ConversionSpec spec;
            spec.outputRate = pair.output;
            spec.blockFrames = 4096;
            CountingSink sink;
            const auto progress = convertStreaming(source, sink, spec);

            REQUIRE(progress.hasValue());
            CHECK(progress.value().framesRead == frames);
            CHECK(progress.value().framesWritten ==
                  expectedFrames(frames, pair.input, pair.output));
            CHECK(sink.frames() == progress.value().framesWritten);
        }
    }
}

TEST_CASE("The flushed tail is audio, not a rounding difference", "[engine][streaming]") {
    // Guards the length assertions above rather than the code. An expected
    // length that were wrong in the same direction as a missing flush would
    // agree with it and neither would notice, so measure the tail instead:
    // convert the same input with process() alone and count what never came
    // out.
    constexpr SampleCount kFrames = 8000;
    const ToneSource source{1, kFrames, SampleRate{48000.0}};

    ConversionSpec spec;
    spec.outputRate = SampleRate{44100.0};
    CountingSink sink;
    const auto progress = convertStreaming(source, sink, spec);
    REQUIRE(progress.hasValue());
    CHECK(progress.value().framesWritten ==
          expectedFrames(kFrames, SampleRate{48000.0}, SampleRate{44100.0}));

    AudioBuffer whole{source.info().layout, kFrames};
    REQUIRE(source.read(0, whole.view()).hasValue());

    auto converter = dsp::Resampler::create(
        dsp::ResamplerSpec{SampleRate{48000.0}, SampleRate{44100.0}, dsp::ResamplerQuality::Best});
    REQUIRE(converter.hasValue());

    std::vector<float> out(static_cast<std::size_t>(kFrames), 0.0f);
    const auto capacity = static_cast<SampleCount>(out.size());
    SampleCount consumed = 0;
    SampleCount produced = 0;
    while (consumed < kFrames) {
        const auto step = converter.value().process(whole.channel(0) + consumed, kFrames - consumed,
                                                    out.data() + produced, capacity - produced);
        if (step.inputConsumed == 0 && step.outputProduced == 0) {
            break;
        }
        consumed += step.inputConsumed;
        produced += step.outputProduced;
    }

    const SampleCount tail = progress.value().framesWritten - produced;
    INFO("the flush contributed " << tail << " frames");
    // Roughly the filter's reach scaled by the ratio -- tens of samples, not
    // one. The bound is well under that so it pins the order of magnitude
    // rather than the quality preset's tap count.
    CHECK(tail >= 16);
}

TEST_CASE("Stereo channels keep their exact relationship", "[engine][streaming]") {
    // Left is a tone, right is that tone negated. Negation is exact in IEEE
    // arithmetic and so is the sum of negated products, so the converted right
    // channel must be the exact negation of the converted left -- not close to
    // it. Channels that drifted by a sample, or that shared filter state, would
    // break the relationship long before they became audible one at a time.
    const ToneSource source{2, kTestFrames, SampleRate{44100.0}, true};

    ConversionSpec spec;
    spec.outputRate = SampleRate{48000.0};
    spec.blockFrames = 4099;
    RecordingSink sink{2};
    const auto progress = convertStreaming(source, sink, spec);

    REQUIRE(progress.hasValue());
    REQUIRE(sink.channel(0).size() == sink.channel(1).size());
    REQUIRE(!sink.channel(0).empty());

    std::size_t differences = 0;
    std::size_t first = 0;
    for (std::size_t i = 0; i < sink.channel(0).size(); ++i) {
        if (sink.channel(1)[i] != -sink.channel(0)[i]) {
            if (differences == 0) {
                first = i;
            }
            ++differences;
        }
    }
    INFO(differences << " frames are not mirrored, first at index " << first);
    CHECK(differences == 0);
}

TEST_CASE("No rate change passes the samples through untouched", "[engine][streaming]") {
    const ToneSource source{2, kTestFrames, SampleRate{48000.0}};

    ConversionSpec spec;
    spec.outputRate = SampleRate{48000.0};
    spec.blockFrames = 4099;
    RecordingSink sink{2};
    const auto progress = convertStreaming(source, sink, spec);

    REQUIRE(progress.hasValue());
    CHECK(progress.value().framesRead == kTestFrames);
    CHECK(progress.value().framesWritten == kTestFrames);

    for (int channel = 0; channel < 2; ++channel) {
        std::vector<float> expected(static_cast<std::size_t>(kTestFrames), 0.0f);
        for (SampleCount i = 0; i < kTestFrames; ++i) {
            expected[static_cast<std::size_t>(i)] = source.sampleAt(channel, i);
        }
        requireIdentical(sink.channel(channel), expected,
                         "channel " + std::to_string(channel) + " passed through");
    }
}

TEST_CASE("Memory is bounded by the block, not by the length of the file", "[engine][streaming]") {
    // What is measured, stated plainly: the number of heap allocations the
    // conversion makes -- counted by the sa::rt instrumentation, which counts
    // allocations and not bytes -- together with the widest read it asks for
    // and the widest block it writes. That is not a peak resident figure and is
    // not claimed as one.
    //
    // It is still the claim that matters. A converter that held the file would
    // have to allocate for its length: either more blocks as it went, or one
    // wider block at the start. Two conversions twenty times apart in length
    // that allocate the same number of buffers, each the same width, are
    // holding the block and not the file.
    const SampleCount lengths[] = {50'000, 1'000'000};
    constexpr SampleCount kBlockFrames = 4096;

    std::size_t allocations[2] = {0, 0};
    SampleCount widestRead[2] = {0, 0};
    SampleCount widestWrite[2] = {0, 0};
    SampleCount written[2] = {0, 0};

    for (std::size_t i = 0; i < 2; ++i) {
        const ToneSource source{1, lengths[i], SampleRate{48000.0}};
        ConversionSpec spec;
        spec.outputRate = SampleRate{44100.0};
        spec.quality = dsp::ResamplerQuality::Fast;
        spec.blockFrames = kBlockFrames;
        CountingSink sink;

        {
            // The counter only watches threads marked as the audio thread, so
            // the mark is what makes this thread's allocations visible. Nothing
            // here is pretending to be real time.
            const rt::ScopedAudioThread guard;
            const rt::AllocationScope scope;
            const auto progress = convertStreaming(source, sink, spec);
            allocations[i] = scope.count();
            written[i] = progress.hasValue() ? progress.value().framesWritten : -1;
        }

        widestRead[i] = source.widestRead();
        widestWrite[i] = sink.widestWrite();
    }

    CHECK(written[0] == expectedFrames(lengths[0], SampleRate{48000.0}, SampleRate{44100.0}));
    CHECK(written[1] == expectedFrames(lengths[1], SampleRate{48000.0}, SampleRate{44100.0}));

    CHECK(widestRead[0] == kBlockFrames);
    CHECK(widestRead[1] == kBlockFrames);
    // This conversion's ratio is below one, so a converted block is narrower
    // than the block it came from; that it is narrower at all is what says the
    // output buffer was sized from the block rather than from the file.
    CHECK(widestWrite[0] == widestWrite[1]);
    CHECK(widestWrite[0] > 0);
    CHECK(widestWrite[0] <= kBlockFrames);

    if (!rt::checksEnabled()) {
        SUCCEED("SA_RT_SAFETY_CHECKS is off in this build; allocations are not counted");
        return;
    }
    INFO("allocations: " << allocations[0] << " for " << lengths[0] << " frames, " << allocations[1]
                         << " for " << lengths[1]);
    CHECK(allocations[0] == allocations[1]);
    CHECK(allocations[0] > 0);
}

TEST_CASE("Cancelling partway fails rather than reporting a short file as done",
          "[engine][streaming]") {
    const ToneSource source{2, 200'000, SampleRate{48000.0}};

    CancellationToken token;
    JobMonitor monitor;
    monitor.cancellation = &token;
    int reports = 0;
    monitor.onProgress = [&](double) {
        if (++reports == 2) {
            token.cancel();
        }
    };

    ConversionSpec spec;
    spec.outputRate = SampleRate{44100.0};
    spec.blockFrames = 4096;
    CountingSink sink;
    const auto progress = convertStreaming(source, sink, spec, monitor);

    REQUIRE_FALSE(progress.hasValue());
    CHECK(progress.error().code() == ErrorCode::Cancelled);

    // What reached the sink stays there -- the conversion is not undone. What
    // does not happen is a success, which is what a caller would answer by
    // finishing the file it is writing.
    CHECK(sink.frames() > 0);
    CHECK(sink.frames() < expectedFrames(200'000, SampleRate{48000.0}, SampleRate{44100.0}));
}

TEST_CASE("Cancelling before the first block writes nothing at all", "[engine][streaming]") {
    const ToneSource source{2, 200'000, SampleRate{48000.0}};

    CancellationToken token;
    token.cancel();
    JobMonitor monitor;
    monitor.cancellation = &token;

    ConversionSpec spec;
    spec.outputRate = SampleRate{44100.0};
    CountingSink sink;
    const auto progress = convertStreaming(source, sink, spec, monitor);

    REQUIRE_FALSE(progress.hasValue());
    CHECK(progress.error().code() == ErrorCode::Cancelled);
    CHECK(sink.writes() == 0);
}

TEST_CASE("Progress runs from nothing to whole", "[engine][streaming]") {
    const ToneSource source{1, 50'000, SampleRate{48000.0}};

    std::vector<double> reported;
    JobMonitor monitor;
    monitor.onProgress = [&](double fraction) { reported.push_back(fraction); };

    ConversionSpec spec;
    spec.outputRate = SampleRate{44100.0};
    spec.blockFrames = 4096;
    CountingSink sink;
    REQUIRE(convertStreaming(source, sink, spec, monitor).hasValue());

    REQUIRE(reported.size() > 1);
    CHECK(std::is_sorted(reported.begin(), reported.end()));
    CHECK(reported.front() > 0.0);
    CHECK(reported.back() == 1.0);
}

TEST_CASE("A source with no frames converts to nothing, successfully", "[engine][streaming]") {
    const ToneSource source{2, 0, SampleRate{48000.0}};

    ConversionSpec spec;
    spec.outputRate = SampleRate{44100.0};
    RecordingSink sink{2};
    const auto progress = convertStreaming(source, sink, spec);

    REQUIRE(progress.hasValue());
    CHECK(progress.value().framesRead == 0);
    CHECK(progress.value().framesWritten == 0);
    CHECK(sink.writes() == 0);
}

TEST_CASE("A conversion refuses what it cannot do", "[engine][streaming]") {
    ConversionSpec spec;
    spec.outputRate = SampleRate{44100.0};
    RecordingSink sink{2};

    SECTION("an output rate of zero") {
        const ToneSource source{2, 1000, SampleRate{48000.0}};
        spec.outputRate = SampleRate{0.0};
        const auto progress = convertStreaming(source, sink, spec);
        REQUIRE_FALSE(progress.hasValue());
        CHECK(progress.error().code() == ErrorCode::InvalidArgument);
        CHECK(sink.writes() == 0);
    }

    SECTION("a negative output rate") {
        const ToneSource source{2, 1000, SampleRate{48000.0}};
        spec.outputRate = SampleRate{-48000.0};
        const auto progress = convertStreaming(source, sink, spec);
        REQUIRE_FALSE(progress.hasValue());
        CHECK(progress.error().code() == ErrorCode::InvalidArgument);
        CHECK(sink.writes() == 0);
    }

    SECTION("a block size of zero") {
        const ToneSource source{2, 1000, SampleRate{48000.0}};
        spec.blockFrames = 0;
        const auto progress = convertStreaming(source, sink, spec);
        REQUIRE_FALSE(progress.hasValue());
        CHECK(progress.error().code() == ErrorCode::InvalidArgument);
        CHECK(sink.writes() == 0);
    }

    SECTION("a negative block size") {
        const ToneSource source{2, 1000, SampleRate{48000.0}};
        spec.blockFrames = -4096;
        const auto progress = convertStreaming(source, sink, spec);
        REQUIRE_FALSE(progress.hasValue());
        CHECK(progress.error().code() == ErrorCode::InvalidArgument);
        CHECK(sink.writes() == 0);
    }

    SECTION("a source with no channels") {
        const ToneSource source{0, 1000, SampleRate{48000.0}};
        RecordingSink empty{0};
        const auto progress = convertStreaming(source, empty, spec);
        REQUIRE_FALSE(progress.hasValue());
        CHECK(progress.error().code() == ErrorCode::InvalidArgument);
        CHECK(empty.writes() == 0);
    }

    SECTION("a source whose own rate is not a rate") {
        const ToneSource source{2, 1000, SampleRate{0.0}};
        const auto progress = convertStreaming(source, sink, spec);
        REQUIRE_FALSE(progress.hasValue());
        CHECK(progress.error().code() == ErrorCode::InvalidArgument);
        CHECK(sink.writes() == 0);
    }

    SECTION("a ratio past what the converter will do") {
        // 1/160, where the converter stops at 1/128 and says so itself. The
        // refusal is the converter's, passed on unchanged rather than
        // relabelled.
        const ToneSource source{2, 1000, SampleRate{48000.0}};
        spec.outputRate = SampleRate{300.0};
        const auto progress = convertStreaming(source, sink, spec);
        REQUIRE_FALSE(progress.hasValue());
        CHECK(progress.error().code() == ErrorCode::OutOfRange);
        CHECK(sink.writes() == 0);
    }
}

TEST_CASE("A sink that fails stops the conversion", "[engine][streaming]") {
    /// Fails on its third write, the way a disk filling up would.
    class FailingSink final : public AudioSink {
    public:
        [[nodiscard]] Status write(ConstAudioBufferView frames) override {
            static_cast<void>(frames);
            if (++writes_ >= 3) {
                return Error{ErrorCode::IoFailure, "the disk is full"};
            }
            return {};
        }

        [[nodiscard]] int writes() const noexcept { return writes_; }

    private:
        int writes_ = 0;
    };

    const ToneSource source{2, 200'000, SampleRate{48000.0}};
    ConversionSpec spec;
    spec.outputRate = SampleRate{44100.0};
    spec.blockFrames = 4096;
    FailingSink sink;
    const auto progress = convertStreaming(source, sink, spec);

    REQUIRE_FALSE(progress.hasValue());
    CHECK(progress.error().code() == ErrorCode::IoFailure);
    CHECK(sink.writes() == 3);
}
