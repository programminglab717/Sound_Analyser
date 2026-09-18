#include <sa/core/AudioBuffer.h>
#include <sa/engine/BufferSource.h>
#include <sa/engine/DocumentSource.h>
#include <sa/engine/Edits.h>
#include <sa/engine/SilentSource.h>

#include <atomic>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <memory>
#include <numbers>
#include <thread>
#include <vector>

using namespace sa;
using namespace sa::engine;
using Catch::Approx;

namespace {

/// A source whose sample value is its own frame index, so any read can be
/// checked against where it claims to have come from.
class RampSource final : public io::AudioSource {
public:
    RampSource(SampleCount frames, int channels) {
        info_.sampleRate = SampleRate{48000.0};
        info_.layout = channels == 1 ? ChannelLayout::mono() : ChannelLayout::stereo();
        info_.frameCount = frames;
        info_.format = io::SampleFormat::Float32;
    }

    [[nodiscard]] const io::AudioFileInfo& info() const noexcept override { return info_; }

    [[nodiscard]] Result<SampleCount> read(SampleIndex startFrame,
                                           AudioBufferView destination) const override {
        if (startFrame >= info_.frameCount) {
            return SampleCount{0};
        }
        const SampleCount count =
            std::min<SampleCount>(destination.frames(), info_.frameCount - startFrame);
        for (int channel = 0; channel < destination.channelCount(); ++channel) {
            float* out = destination.channel(channel);
            for (SampleCount i = 0; i < count; ++i) {
                out[i] = static_cast<float>(startFrame + i);
            }
        }
        return count;
    }

private:
    io::AudioFileInfo info_;
};

Document documentWithRamp(SampleCount frames, int channels = 1) {
    Document document{SampleRate{48000.0},
                      channels == 1 ? ChannelLayout::mono() : ChannelLayout::stereo()};
    auto source = document.addSource(std::make_shared<RampSource>(frames, channels), "ramp");
    REQUIRE(source.hasValue());
    REQUIRE(document.appendSource(source.value(), 0).hasValue());
    return document;
}

} // namespace

TEST_CASE("A document reads back as the audio it renders", "[engine][documentsource]") {
    const Document document = documentWithRamp(1000);
    const DocumentSource source{document};

    CHECK(source.info().frameCount == 1000);
    CHECK(source.info().channelCount() == 1);
    CHECK(source.info().sampleRate.hz() == Approx(48000.0));

    AudioBuffer buffer{ChannelLayout::mono(), 256};
    const auto read = source.read(100, buffer.view());
    REQUIRE(read.hasValue());
    CHECK(read.value() == 256);
    CHECK(buffer.channel(0)[0] == Approx(100.0f));
    CHECK(buffer.channel(0)[255] == Approx(355.0f));
}

TEST_CASE("Reading past the end returns a short read, not silence", "[engine][documentsource]") {
    // The distinction matters: a caller that trusts a full buffer of zeros
    // treats the document as longer than it is, and every streaming analysis
    // pass in the project drives its loop off the returned count.
    const Document document = documentWithRamp(300);
    const DocumentSource source{document};

    AudioBuffer buffer{ChannelLayout::mono(), 256};
    auto read = source.read(200, buffer.view());
    REQUIRE(read.hasValue());
    CHECK(read.value() == 100);

    read = source.read(300, buffer.view());
    REQUIRE(read.hasValue());
    CHECK(read.value() == 0);

    read = source.read(9999, buffer.view());
    REQUIRE(read.hasValue());
    CHECK(read.value() == 0);
}

TEST_CASE("An edit changes what the source reads back", "[engine][documentsource]") {
    // This is the whole reason the adapter exists: after an edit the file on
    // disk is no longer what the user is looking at.
    Document document = documentWithRamp(1000);
    REQUIRE(deleteRange(document, 0, 100, true).ok());

    const DocumentSource source{document};
    CHECK(source.info().frameCount == 900);

    AudioBuffer buffer{ChannelLayout::mono(), 8};
    const auto read = source.read(0, buffer.view());
    REQUIRE(read.hasValue());
    // The first 100 frames were rippled away, so frame 0 now holds what was
    // frame 100.
    CHECK(buffer.channel(0)[0] == Approx(100.0f));
}

TEST_CASE("Mismatched and negative reads are refused", "[engine][documentsource]") {
    const Document document = documentWithRamp(500, 2);
    const DocumentSource source{document};

    AudioBuffer mono{ChannelLayout::mono(), 64};
    CHECK_FALSE(source.read(0, mono.view()).hasValue());

    AudioBuffer stereo{ChannelLayout::stereo(), 64};
    CHECK_FALSE(source.read(-1, stereo.view()).hasValue());
    CHECK(source.read(0, stereo.view()).hasValue());
}

TEST_CASE("Concurrent reads do not corrupt each other", "[engine][documentsource]") {
    // The adapter shares one render scratch, so an analysis pass and a redraw
    // reading at once must not tear. If the mutex were dropped this test is
    // what would catch it.
    const Document document = documentWithRamp(20000);
    const DocumentSource source{document};

    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&source, &failures, t] {
            AudioBuffer buffer{ChannelLayout::mono(), 512};
            for (int pass = 0; pass < 40; ++pass) {
                const SampleIndex start = (t * 977 + pass * 131) % 10000;
                const auto read = source.read(start, buffer.view());
                if (!read.hasValue() || buffer.channel(0)[0] != static_cast<float>(start)) {
                    ++failures;
                    return;
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(failures.load() == 0);
}

TEST_CASE("A buffer source reads back exactly what it was given", "[engine][buffersource]") {
    AudioBuffer audio{ChannelLayout::stereo(), 400};
    for (SampleCount i = 0; i < 400; ++i) {
        audio.channel(0)[i] = static_cast<float>(i);
        audio.channel(1)[i] = -static_cast<float>(i);
    }
    const BufferSource source{std::move(audio), SampleRate{44100.0}};

    CHECK(source.info().frameCount == 400);
    CHECK(source.info().channelCount() == 2);
    CHECK(source.info().sampleRate.hz() == Approx(44100.0));

    AudioBuffer out{ChannelLayout::stereo(), 100};
    auto read = source.read(350, out.view());
    REQUIRE(read.hasValue());
    CHECK(read.value() == 50); // Short read at the end, not silence.
    CHECK(out.channel(0)[0] == Approx(350.0f));
    CHECK(out.channel(1)[49] == Approx(-399.0f));

    AudioBuffer mono{ChannelLayout::mono(), 10};
    CHECK_FALSE(source.read(0, mono.view()).hasValue());
    CHECK_FALSE(source.read(-5, out.view()).hasValue());
    CHECK(source.read(400, out.view()).value() == 0);
}

TEST_CASE("A source is a snapshot, not a window onto a document still being edited",
          "[engine][documentsource]") {
    // This is the contract the background readers depend on. A spectrogram
    // build and the player's render worker both hold a source and read it on
    // their own threads while the user goes on editing; if the source looked at
    // the live document, every edit would be a race against every reader, and
    // the failure mode is a crash once in a hundred runs on a loaded machine
    // rather than anything a test would catch by accident.
    Document document = documentWithRamp(1000);
    const DocumentSource source{document};

    REQUIRE(deleteRange(document, 0, 500, true).ok());
    REQUIRE(document.duration() == 500);

    // The source still describes, and still reads, what it was made from.
    CHECK(source.info().frameCount == 1000);

    AudioBuffer read{ChannelLayout::mono(), 8};
    const auto count = source.read(600, read.view());
    REQUIRE(count);
    CHECK(count.value() == 8);
    for (SampleCount i = 0; i < 8; ++i) {
        INFO("frame " << i);
        CHECK(read.channel(0)[i] == Approx(static_cast<float>(600 + i)));
    }
}

TEST_CASE("Reading a source while the document it came from is rewritten is safe",
          "[engine][documentsource]") {
    // The shape of the fault this guards, run for real: one thread reading
    // through a source, another editing the document underneath it. Under the
    // thread sanitiser this fails outright if the source ever looks at the
    // live document again; without it, it is still a stress test that used to
    // crash and now does not.
    Document document = documentWithRamp(200000);
    const auto source = std::make_shared<const DocumentSource>(document);

    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};
    std::thread reader{[source, &stop, &reads] {
        AudioBuffer block{ChannelLayout::mono(), 4096};
        while (!stop.load(std::memory_order_relaxed)) {
            for (SampleIndex at = 0; at + 4096 < 200000; at += 4096) {
                if (const auto got = source->read(at, block.view()); got) {
                    reads.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }};

    // Wait for the reader to actually be reading before editing under it.
    // Structural edits are microseconds and a thread takes longer than that to
    // start, so without this the edits are all over before the race could
    // happen and the test proves nothing.
    while (reads.load(std::memory_order_relaxed) == 0) {
        std::this_thread::yield();
    }
    for (int round = 0; round < 2000; ++round) {
        REQUIRE(insertSilence(document, 1000, 500).ok());
        REQUIRE(deleteRange(document, 1000, 1500, true).ok());
    }
    const int duringEdits = reads.load(std::memory_order_relaxed);
    stop.store(true, std::memory_order_relaxed);
    reader.join();

    CHECK(duringEdits > 0);
    CHECK(reads.load() > duringEdits);
    CHECK(document.duration() == 200000);
}
