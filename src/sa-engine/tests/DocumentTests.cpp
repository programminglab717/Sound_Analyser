#include "TestSource.h"

#include <sa/core/RealtimeGuard.h>
#include <sa/engine/Document.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <vector>

using namespace sa;
using namespace sa::engine;
using namespace sa::engine::test;
using Catch::Approx;

namespace {

Document makeDocument() {
    return Document{kSampleRate48000, ChannelLayout::stereo()};
}

SourceId addRamp(Document& document, SampleCount frames, float scale = 1.0f) {
    auto id = document.addSource(makeRamp(2, frames, scale), "ramp");
    REQUIRE(id.hasValue());
    return id.value();
}

std::vector<float> renderChannel(const Document& document, SampleIndex start, SampleCount frames,
                                 int channel = 0) {
    AudioBuffer buffer{ChannelLayout::stereo(), frames};
    REQUIRE(document.render(start, buffer.view()).ok());
    return std::vector<float>{buffer.channel(channel), buffer.channel(channel) + frames};
}

} // namespace

TEST_CASE("A new document is empty", "[engine][document]") {
    const Document document = makeDocument();
    CHECK(document.duration() == 0);
    CHECK(document.sourceCount() == 0);
    CHECK(document.timeline().isEmpty());
}

TEST_CASE("Sources are shared, not copied", "[engine][document]") {
    Document document = makeDocument();
    auto audio = makeRamp(2, 1000);
    const auto first = document.addSource(audio, "a");
    const auto second = document.addSource(audio, "b");
    REQUIRE(first.hasValue());
    REQUIRE(second.hasValue());

    // Distinct registrations, one decode. Ten clips over one file must not cost
    // ten copies of the audio.
    CHECK(first.value() != second.value());
    CHECK(document.sourceCount() == 2);
    CHECK(document.source(first.value())->audio == document.source(second.value())->audio);

    CHECK_FALSE(document.addSource(nullptr).hasValue());
    CHECK(document.source(static_cast<SourceId>(99999)) == nullptr);
}

TEST_CASE("Appending a source spans its whole length", "[engine][document]") {
    Document document = makeDocument();
    const SourceId source = addRamp(document, 1000);

    auto clip = document.appendSource(source, 500);
    REQUIRE(clip.hasValue());
    CHECK(document.duration() == 1500);
    CHECK(document.timeline().clipCount() == 1);

    const Clip* placed = document.timeline().find(clip.value());
    REQUIRE(placed != nullptr);
    CHECK(placed->timelineStart == 500);
    CHECK(placed->length == 1000);
    CHECK(placed->sourceStart == 0);

    CHECK_FALSE(document.appendSource(static_cast<SourceId>(12345), 0).hasValue());
    CHECK_FALSE(document.appendSource(source, -1).hasValue());
}

TEST_CASE("Rendering places audio at the clip's timeline position", "[engine][document][render]") {
    Document document = makeDocument();
    const SourceId source = addRamp(document, 100);
    REQUIRE(document.appendSource(source, 50).hasValue());

    const auto rendered = renderChannel(document, 0, 200);

    for (SampleCount i = 0; i < 50; ++i) {
        REQUIRE(rendered[static_cast<std::size_t>(i)] == 0.0f); // before the clip
    }
    for (SampleCount i = 0; i < 100; ++i) {
        // Sample f of the source lands at timeline 50 + f.
        REQUIRE(rendered[static_cast<std::size_t>(50 + i)] == Approx(static_cast<float>(i)));
    }
    for (SampleCount i = 150; i < 200; ++i) {
        REQUIRE(rendered[static_cast<std::size_t>(i)] == 0.0f); // after the clip
    }
}

TEST_CASE("Rendering an empty region yields silence, not an error", "[engine][document][render]") {
    Document document = makeDocument();
    const SourceId source = addRamp(document, 100);
    REQUIRE(document.appendSource(source, 0).hasValue());

    const auto rendered = renderChannel(document, 10000, 64);
    for (float value : rendered) {
        CHECK(value == 0.0f);
    }
}

TEST_CASE("Channels stay separate", "[engine][document][render]") {
    Document document = makeDocument();
    const SourceId source = addRamp(document, 100);
    REQUIRE(document.appendSource(source, 0).hasValue());

    const auto left = renderChannel(document, 0, 100, 0);
    const auto right = renderChannel(document, 0, 100, 1);
    CHECK(left[10] == Approx(10.0f));
    CHECK(right[10] == Approx(10010.0f));
}

TEST_CASE("Clip gain scales the output", "[engine][document][render]") {
    Document document = makeDocument();
    auto audio = document.addSource(makeConstant(2, 100, 1.0f), "dc");
    REQUIRE(audio.hasValue());
    auto clip = document.appendSource(audio.value(), 0);
    REQUIRE(clip.hasValue());

    document.timeline().findMutable(clip.value())->gain = 0.25f;
    const auto rendered = renderChannel(document, 0, 100);
    for (float value : rendered) {
        CHECK(value == Approx(0.25f));
    }
}

TEST_CASE("Overlapping clips sum", "[engine][document][render]") {
    // Mixing is addition, which is what lets a crossfade work with no special
    // case in the renderer.
    Document document = makeDocument();
    auto audio = document.addSource(makeConstant(2, 100, 0.5f), "dc");
    REQUIRE(audio.hasValue());
    REQUIRE(document.appendSource(audio.value(), 0).hasValue());
    REQUIRE(document.appendSource(audio.value(), 50).hasValue());

    const auto rendered = renderChannel(document, 0, 150);
    CHECK(rendered[10] == Approx(0.5f));  // first clip only
    CHECK(rendered[75] == Approx(1.0f));  // both
    CHECK(rendered[120] == Approx(0.5f)); // second clip only
}

TEST_CASE("Fades shape the clip edges", "[engine][document][render]") {
    Document document = makeDocument();
    auto audio = document.addSource(makeConstant(2, 100, 1.0f), "dc");
    REQUIRE(audio.hasValue());
    auto clip = document.appendSource(audio.value(), 0);
    REQUIRE(clip.hasValue());

    Clip* placed = document.timeline().findMutable(clip.value());
    placed->fadeIn = Fade{20, FadeShape::Linear};
    placed->fadeOut = Fade{20, FadeShape::Linear};

    const auto rendered = renderChannel(document, 0, 100);
    CHECK(rendered[0] == Approx(0.0f).margin(1e-6));
    CHECK(rendered[10] == Approx(0.5f).margin(1e-6));
    CHECK(rendered[50] == Approx(1.0f).margin(1e-6));
    CHECK(rendered[90] == Approx(0.5f).margin(1e-6));
    CHECK(rendered[99] == Approx(0.05f).margin(0.06f));
}

TEST_CASE("Equal-power fades hold constant through a crossfade", "[engine][document][render]") {
    // Two linear fades sum to a 3 dB dip in the middle, which is audible as a
    // hole. Equal-power is the reason the default is what it is.
    for (SampleIndex position = 0; position <= 100; position += 10) {
        const float in = fadeGain(FadeShape::EqualPower, position, 100, true);
        const float out = fadeGain(FadeShape::EqualPower, position, 100, false);
        INFO("position " << position);
        CHECK(in * in + out * out == Approx(1.0f).margin(1e-5));
    }

    // Linear deliberately does not hold, confirming the test discriminates.
    const float linearIn = fadeGain(FadeShape::Linear, 50, 100, true);
    const float linearOut = fadeGain(FadeShape::Linear, 50, 100, false);
    CHECK(linearIn * linearIn + linearOut * linearOut == Approx(0.5f).margin(1e-5));
}

TEST_CASE("Fade gain is clamped outside the fade", "[engine][document]") {
    CHECK(fadeGain(FadeShape::Linear, -5, 100, true) == 0.0f);
    CHECK(fadeGain(FadeShape::Linear, 200, 100, true) == 1.0f);
    CHECK(fadeGain(FadeShape::Linear, -5, 100, false) == 1.0f);
    CHECK(fadeGain(FadeShape::Linear, 200, 100, false) == 0.0f);
    CHECK(fadeGain(FadeShape::Linear, 10, 0, true) == 1.0f);
}

TEST_CASE("Rendering in blocks matches rendering in one pass", "[engine][document][render]") {
    // Scrubbing and playback render in blocks; an export renders in one call.
    // If those disagreed, what you hear would not be what you export.
    Document document = makeDocument();
    const SourceId source = addRamp(document, 5000);
    REQUIRE(document.appendSource(source, 100).hasValue());
    REQUIRE(document.appendSource(source, 3000).hasValue());

    const auto whole = renderChannel(document, 0, 6000);

    AudioBuffer assembled{ChannelLayout::stereo(), 6000};
    AudioBuffer block{ChannelLayout::stereo(), 257};
    RenderContext context;
    for (SampleIndex position = 0; position < 6000; position += 257) {
        const SampleCount frames = std::min<SampleCount>(257, 6000 - position);
        REQUIRE(document.render(position, block.view().subRange(0, frames), context).ok());
        std::copy_n(block.channel(0), frames, assembled.channel(0) + position);
    }

    for (SampleCount i = 0; i < 6000; ++i) {
        REQUIRE(assembled.channel(0)[i] == Approx(whole[static_cast<std::size_t>(i)]));
    }
}

TEST_CASE("A reused RenderContext stops allocating", "[engine][document][render]") {
    Document document = makeDocument();
    const SourceId source = addRamp(document, 10000);
    REQUIRE(document.appendSource(source, 0).hasValue());

    AudioBuffer block{ChannelLayout::stereo(), 512};
    RenderContext context;
    REQUIRE(document.render(0, block.view(), context).ok()); // warm the scratch

    if (!rt::checksEnabled()) {
        SUCCEED("SA_RT_SAFETY_CHECKS is off in this build");
        return;
    }

    const rt::ScopedAudioThread guard;
    const rt::AllocationScope scope;
    for (SampleIndex position = 0; position < 5000; position += 512) {
        (void)document.render(position, block.view(), context);
    }
    CHECK(scope.count() == 0);
}

TEST_CASE("Timeline queries find every overlapping clip", "[engine][timeline]") {
    Timeline timeline;
    for (int i = 0; i < 10; ++i) {
        Clip clip;
        clip.id = static_cast<ClipId>(i + 1);
        clip.timelineStart = i * 100;
        clip.length = 100;
        timeline.insert(clip);
    }

    std::vector<std::size_t> found;
    timeline.collectOverlapping(250, 450, found);
    CHECK(found.size() == 3); // clips at 200, 300, 400

    timeline.collectOverlapping(0, 10000, found);
    CHECK(found.size() == 10);

    timeline.collectOverlapping(5000, 6000, found);
    CHECK(found.empty());

    timeline.collectOverlapping(100, 100, found);
    CHECK(found.empty());
}

TEST_CASE("A long clip starting before the range is still found", "[engine][timeline]") {
    // The failure mode of a naive binary search: a clip that starts well before
    // the query but extends into it gets skipped, and its audio silently
    // vanishes from the render.
    Timeline timeline;
    Clip longClip;
    longClip.id = static_cast<ClipId>(1);
    longClip.timelineStart = 0;
    longClip.length = 100000;
    timeline.insert(longClip);

    Clip shortClip;
    shortClip.id = static_cast<ClipId>(2);
    shortClip.timelineStart = 90000;
    shortClip.length = 100;
    timeline.insert(shortClip);

    std::vector<std::size_t> found;
    timeline.collectOverlapping(50000, 50100, found);
    CHECK(found.size() == 1);

    timeline.collectOverlapping(90000, 90050, found);
    CHECK(found.size() == 2);
}

TEST_CASE("Clips stay ordered", "[engine][timeline]") {
    Timeline timeline;
    for (SampleIndex start : {500, 100, 900, 300}) {
        Clip clip;
        clip.id = static_cast<ClipId>(start);
        clip.timelineStart = start;
        clip.length = 50;
        timeline.insert(clip);
    }

    const auto& clips = timeline.clips();
    for (std::size_t i = 1; i < clips.size(); ++i) {
        CHECK(clips[i - 1].timelineStart <= clips[i].timelineStart);
    }
    CHECK(timeline.duration() == 950);

    CHECK(timeline.remove(static_cast<ClipId>(300)));
    CHECK_FALSE(timeline.remove(static_cast<ClipId>(300)));
    CHECK(timeline.clipCount() == 3);
}
