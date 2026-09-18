#include "TestSource.h"

#include <sa/engine/Edits.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <vector>

using namespace sa;
using namespace sa::engine;
using namespace sa::engine::test;
using Catch::Approx;

namespace {

struct Fixture {
    Document document{kSampleRate48000, ChannelLayout::stereo()};
    SourceId source = SourceId::Invalid;
    ClipId clip = ClipId::Invalid;

    explicit Fixture(SampleCount frames = 1000, SampleIndex at = 0) {
        auto added = document.addSource(makeRamp(2, frames), "ramp");
        REQUIRE(added.hasValue());
        source = added.value();
        auto placed = document.appendSource(source, at);
        REQUIRE(placed.hasValue());
        clip = placed.value();
    }
};

std::vector<float> render(const Document& document, SampleIndex start, SampleCount frames) {
    AudioBuffer buffer{ChannelLayout::stereo(), frames};
    REQUIRE(document.render(start, buffer.view()).ok());
    return std::vector<float>{buffer.channel(0), buffer.channel(0) + frames};
}

void requireSameRender(const std::vector<float>& before, const std::vector<float>& after) {
    REQUIRE(before.size() == after.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        REQUIRE(after[i] == Approx(before[i]));
    }
}

} // namespace

TEST_CASE("Splitting a clip does not change what renders", "[engine][edits][split]") {
    // The defining property of the model: a split is a bookkeeping change, not
    // an audio operation. If the render moves by even one sample, every edit
    // built on top of split inherits the error.
    Fixture fixture;
    const auto before = render(fixture.document, 0, 1000);

    auto halves = splitClip(fixture.document, fixture.clip, 400);
    REQUIRE(halves.hasValue());
    CHECK(fixture.document.timeline().clipCount() == 2);

    requireSameRender(before, render(fixture.document, 0, 1000));

    const Clip* first = fixture.document.timeline().find(halves.value().first);
    const Clip* second = fixture.document.timeline().find(halves.value().second);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK(first->timelineStart == 0);
    CHECK(first->length == 400);
    CHECK(second->timelineStart == 400);
    CHECK(second->length == 600);
    CHECK(second->sourceStart == 400);
}

TEST_CASE("Splitting repeatedly still renders identically", "[engine][edits][split]") {
    Fixture fixture;
    const auto before = render(fixture.document, 0, 1000);

    for (SampleIndex position : {100, 250, 700, 900, 50}) {
        std::vector<ClipId> ids;
        for (const Clip& clip : fixture.document.timeline().clips()) {
            ids.push_back(clip.id);
        }
        for (ClipId id : ids) {
            const Clip* clip = fixture.document.timeline().find(id);
            if (clip != nullptr && position > clip->timelineStart &&
                position < clip->timelineEnd()) {
                REQUIRE(splitClip(fixture.document, id, position).hasValue());
                break;
            }
        }
    }

    CHECK(fixture.document.timeline().clipCount() == 6);
    requireSameRender(before, render(fixture.document, 0, 1000));
}

TEST_CASE("Splitting outside the clip is rejected", "[engine][edits][split]") {
    Fixture fixture{1000, 100};
    CHECK_FALSE(splitClip(fixture.document, fixture.clip, 100).hasValue());  // at the start
    CHECK_FALSE(splitClip(fixture.document, fixture.clip, 1100).hasValue()); // at the end
    CHECK_FALSE(splitClip(fixture.document, fixture.clip, 50).hasValue());   // before
    CHECK_FALSE(splitClip(fixture.document, fixture.clip, 5000).hasValue()); // after
    CHECK_FALSE(splitClip(fixture.document, static_cast<ClipId>(999), 500).hasValue());
    CHECK(fixture.document.timeline().clipCount() == 1);
}

TEST_CASE("Trimming the start does not slide the audio", "[engine][edits][trim]") {
    // Dragging a clip's left edge should reveal less of the same audio, not
    // scroll different audio into view.
    Fixture fixture;
    REQUIRE(trimClip(fixture.document, fixture.clip, 200, 500).ok());

    const Clip* clip = fixture.document.timeline().find(fixture.clip);
    REQUIRE(clip != nullptr);
    CHECK(clip->timelineStart == 200);
    CHECK(clip->sourceStart == 200);
    CHECK(clip->length == 500);

    const auto rendered = render(fixture.document, 0, 800);
    CHECK(rendered[199] == 0.0f);
    CHECK(rendered[200] == Approx(200.0f)); // source sample 200 stayed at 200
    CHECK(rendered[400] == Approx(400.0f));
    CHECK(rendered[700] == 0.0f);
}

TEST_CASE("Trimming past the source is rejected", "[engine][edits][trim]") {
    Fixture fixture{1000};
    CHECK_FALSE(trimClip(fixture.document, fixture.clip, 0, 2000).ok());
    CHECK_FALSE(trimClip(fixture.document, fixture.clip, 500, 900).ok());
    CHECK_FALSE(trimClip(fixture.document, fixture.clip, 0, 0).ok());
    CHECK_FALSE(trimClip(fixture.document, fixture.clip, -10, 100).ok());

    // The clip is untouched after every rejection, so a caller can commit to
    // undo history only when something actually changed.
    const Clip* clip = fixture.document.timeline().find(fixture.clip);
    CHECK(clip->length == 1000);
    CHECK(clip->timelineStart == 0);
}

TEST_CASE("Trimming shortens fades that no longer fit", "[engine][edits][trim]") {
    Fixture fixture;
    REQUIRE(setClipFades(fixture.document, fixture.clip, Fade{400, FadeShape::Linear},
                         Fade{400, FadeShape::Linear})
                .ok());
    REQUIRE(trimClip(fixture.document, fixture.clip, 0, 100).ok());

    const Clip* clip = fixture.document.timeline().find(fixture.clip);
    CHECK(clip->fadeIn.length <= 100);
    CHECK(clip->fadeOut.length <= 100);
}

TEST_CASE("Fades that exceed the clip are rejected", "[engine][edits]") {
    Fixture fixture{100};
    CHECK_FALSE(setClipFades(fixture.document, fixture.clip, Fade{60}, Fade{60}).ok());
    CHECK_FALSE(setClipFades(fixture.document, fixture.clip, Fade{-1}, Fade{0}).ok());
    CHECK(setClipFades(fixture.document, fixture.clip, Fade{50}, Fade{50}).ok());
}

TEST_CASE("Gain is validated", "[engine][edits]") {
    Fixture fixture;
    CHECK(setClipGain(fixture.document, fixture.clip, 0.5f).ok());
    CHECK(setClipGain(fixture.document, fixture.clip, 0.0f).ok());
    CHECK_FALSE(setClipGain(fixture.document, fixture.clip, -1.0f).ok());
    CHECK_FALSE(
        setClipGain(fixture.document, fixture.clip, std::numeric_limits<float>::quiet_NaN()).ok());
    CHECK_FALSE(setClipGain(fixture.document, static_cast<ClipId>(42), 1.0f).ok());
}

TEST_CASE("Moving a clip keeps its audio", "[engine][edits][move]") {
    Fixture fixture{100};
    REQUIRE(moveClip(fixture.document, fixture.clip, 500).ok());

    const auto rendered = render(fixture.document, 0, 700);
    CHECK(rendered[499] == 0.0f);
    CHECK(rendered[500] == Approx(0.0f)); // source sample 0
    CHECK(rendered[550] == Approx(50.0f));
    CHECK_FALSE(moveClip(fixture.document, fixture.clip, -1).ok());
}

TEST_CASE("Deleting a range without ripple preserves timing", "[engine][edits][delete]") {
    // What a music editor wants: the hole stays, so everything after it keeps
    // its position against the beat.
    Fixture fixture{1000};
    REQUIRE(deleteRange(fixture.document, 300, 500, false).ok());

    const auto rendered = render(fixture.document, 0, 1000);
    CHECK(rendered[299] == Approx(299.0f));
    CHECK(rendered[300] == 0.0f);
    CHECK(rendered[499] == 0.0f);
    CHECK(rendered[500] == Approx(500.0f)); // later audio kept its position
    CHECK(fixture.document.duration() == 1000);
}

TEST_CASE("Deleting a range with ripple closes the gap", "[engine][edits][delete]") {
    // What a dialogue editor wants: cut the pause out and pull everything up.
    Fixture fixture{1000};
    REQUIRE(deleteRange(fixture.document, 300, 500, true).ok());

    const auto rendered = render(fixture.document, 0, 1000);
    CHECK(rendered[299] == Approx(299.0f));
    CHECK(rendered[300] == Approx(500.0f)); // what was at 500 moved to 300
    CHECK(fixture.document.duration() == 800);
}

TEST_CASE("Deleting a whole clip removes it", "[engine][edits][delete]") {
    Fixture fixture{100, 200};
    REQUIRE(deleteRange(fixture.document, 100, 400, false).ok());
    CHECK(fixture.document.timeline().isEmpty());
}

TEST_CASE("Deleting trims clips that straddle a boundary", "[engine][edits][delete]") {
    Fixture fixture{1000};
    REQUIRE(deleteRange(fixture.document, 800, 2000, false).ok());

    const Clip* clip = fixture.document.timeline().find(fixture.clip);
    REQUIRE(clip != nullptr);
    CHECK(clip->length == 800);

    Fixture other{1000};
    REQUIRE(deleteRange(other.document, -500, 200, false).ok());
    const Clip* head = other.document.timeline().find(other.clip);
    REQUIRE(head != nullptr);
    CHECK(head->timelineStart == 200);
    CHECK(head->sourceStart == 200);
    CHECK(head->length == 800);
}

TEST_CASE("Deleting an empty range is rejected", "[engine][edits][delete]") {
    Fixture fixture;
    CHECK_FALSE(deleteRange(fixture.document, 100, 100, false).ok());
    CHECK_FALSE(deleteRange(fixture.document, 500, 100, false).ok());
}

TEST_CASE("Ripple delete moves markers with the audio", "[engine][edits][delete]") {
    Fixture fixture{1000};
    fixture.document.markers().push_back(Marker{100, 0, "before"});
    fixture.document.markers().push_back(Marker{400, 0, "inside"});
    fixture.document.markers().push_back(Marker{700, 0, "after"});

    REQUIRE(deleteRange(fixture.document, 300, 500, true).ok());

    CHECK(fixture.document.markers()[0].position == 100); // unmoved
    CHECK(fixture.document.markers()[1].position == 300); // collapsed to the cut
    CHECK(fixture.document.markers()[2].position == 500); // pulled back by 200
}

TEST_CASE("Inserting silence splits and shifts", "[engine][edits][insert]") {
    Fixture fixture{1000};
    REQUIRE(insertSilence(fixture.document, 400, 200).ok());

    const auto rendered = render(fixture.document, 0, 1400);
    CHECK(rendered[399] == Approx(399.0f));
    CHECK(rendered[400] == 0.0f);
    CHECK(rendered[599] == 0.0f);
    CHECK(rendered[600] == Approx(400.0f)); // audio resumes where it left off
    CHECK(fixture.document.duration() == 1200);

    CHECK_FALSE(insertSilence(fixture.document, 0, 0).ok());
    CHECK_FALSE(insertSilence(fixture.document, -5, 100).ok());
}

TEST_CASE("Inserting silence moves later markers", "[engine][edits][insert]") {
    Fixture fixture{1000};
    fixture.document.markers().push_back(Marker{100, 0, "early"});
    fixture.document.markers().push_back(Marker{800, 0, "late"});

    REQUIRE(insertSilence(fixture.document, 400, 200).ok());
    CHECK(fixture.document.markers()[0].position == 100);
    CHECK(fixture.document.markers()[1].position == 1000);
}

TEST_CASE("A crossfade overlaps the clips", "[engine][edits][crossfade]") {
    Document document{kSampleRate48000, ChannelLayout::stereo()};
    auto audio = document.addSource(makeConstant(2, 1000, 1.0f), "dc");
    REQUIRE(audio.hasValue());
    auto first = document.appendSource(audio.value(), 0);
    auto second = document.appendSource(audio.value(), 1000);
    REQUIRE(first.hasValue());
    REQUIRE(second.hasValue());

    REQUIRE(crossfade(document, first.value(), second.value(), 200).ok());

    const Clip* a = document.timeline().find(first.value());
    const Clip* b = document.timeline().find(second.value());
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    // The later clip is pulled back so the two genuinely overlap. A crossfade
    // over a gap is silence, and over a butt join is a click.
    CHECK(b->timelineStart == 800);
    CHECK(a->timelineEnd() == 1000);
    CHECK(a->fadeOut.length == 200);
    CHECK(b->fadeIn.length == 200);
    // The second clip's audio does not slide: pulling it back by 200 advances
    // its source offset by the same amount.
    CHECK(b->sourceStart == 200);
}

TEST_CASE("Linear is the shape that holds amplitude for correlated material",
          "[engine][edits][crossfade]") {
    // Both clips reference the same constant source, so their outputs add
    // coherently. Equal-power is calibrated for uncorrelated material and
    // produces a +3 dB bump here; linear is the correct choice.
    const auto sumAtMidpoint = [](FadeShape shape) {
        Document document{kSampleRate48000, ChannelLayout::stereo()};
        auto audio = document.addSource(makeConstant(2, 1000, 1.0f), "dc");
        REQUIRE(audio.hasValue());
        auto first = document.appendSource(audio.value(), 0);
        auto second = document.appendSource(audio.value(), 1000);
        REQUIRE(first.hasValue());
        REQUIRE(second.hasValue());
        REQUIRE(crossfade(document, first.value(), second.value(), 200, shape).ok());
        return render(document, 0, 2000)[900]; // centre of the overlap
    };

    CHECK(sumAtMidpoint(FadeShape::Linear) == Approx(1.0f).margin(1e-4));

    // sin(pi/4) + cos(pi/4) = sqrt(2): the +3 dB bump, present by design.
    CHECK(sumAtMidpoint(FadeShape::EqualPower) == Approx(1.41421f).margin(1e-3));
}

TEST_CASE("Equal-power holds constant power across the overlap", "[engine][edits][crossfade]") {
    // The property equal-power actually guarantees: the summed *power* of two
    // uncorrelated signals is constant, so the loudness does not dip.
    for (SampleIndex position = 0; position <= 200; position += 20) {
        const float in = fadeGain(FadeShape::EqualPower, position, 200, true);
        const float out = fadeGain(FadeShape::EqualPower, position, 200, false);
        INFO("position " << position);
        CHECK(in * in + out * out == Approx(1.0f).margin(1e-5));
    }
}

TEST_CASE("Invalid crossfades are rejected", "[engine][edits][crossfade]") {
    Document document{kSampleRate48000, ChannelLayout::stereo()};
    auto audio = document.addSource(makeConstant(2, 100, 1.0f), "dc");
    REQUIRE(audio.hasValue());
    auto first = document.appendSource(audio.value(), 0);
    auto second = document.appendSource(audio.value(), 100);
    REQUIRE(first.hasValue());
    REQUIRE(second.hasValue());

    CHECK_FALSE(crossfade(document, first.value(), second.value(), 0).ok());
    CHECK_FALSE(crossfade(document, first.value(), second.value(), 500).ok());
    CHECK_FALSE(crossfade(document, first.value(), static_cast<ClipId>(999), 10).ok());
}

TEST_CASE("Duplicating shares the source", "[engine][edits][duplicate]") {
    Fixture fixture{100};
    auto copy = duplicateClip(fixture.document, fixture.clip, 500);
    REQUIRE(copy.hasValue());

    const Clip* original = fixture.document.timeline().find(fixture.clip);
    const Clip* duplicate = fixture.document.timeline().find(copy.value());
    REQUIRE(duplicate != nullptr);
    CHECK(duplicate->source == original->source);
    CHECK(duplicate->id != original->id);
    CHECK(duplicate->timelineStart == 500);
    CHECK(fixture.document.sourceCount() == 1); // no extra decode

    const auto rendered = render(fixture.document, 0, 700);
    CHECK(rendered[50] == Approx(50.0f));
    CHECK(rendered[550] == Approx(50.0f));

    CHECK_FALSE(duplicateClip(fixture.document, static_cast<ClipId>(77), 0).hasValue());
}

TEST_CASE("Range gain multiplies only what is inside the range", "[engine][edits][gain]") {
    Fixture fixture{1000};
    Document& document = fixture.document;
    REQUIRE(applyRangeGain(document, 200, 600, 0.5f).ok());

    AudioBuffer out{ChannelLayout::stereo(), 1000};
    REQUIRE(document.render(0, out.view()).ok());

    CHECK(out.channel(0)[199] == Approx(199.0f));
    CHECK(out.channel(0)[200] == Approx(100.0f));
    CHECK(out.channel(0)[599] == Approx(299.5f));
    CHECK(out.channel(0)[600] == Approx(600.0f));
}

TEST_CASE("Range gain is multiplicative, not absolute", "[engine][edits][gain]") {
    // Two corrections in a row must compose. An absolute set would silently
    // discard the first, which is exactly the bug that makes a user distrust a
    // gain control.
    Fixture fixture{1000};
    Document& document = fixture.document;
    REQUIRE(applyRangeGain(document, 0, 1000, 0.5f).ok());
    REQUIRE(applyRangeGain(document, 0, 1000, 0.5f).ok());

    AudioBuffer out{ChannelLayout::stereo(), 8};
    REQUIRE(document.render(100, out.view()).ok());
    CHECK(out.channel(0)[0] == Approx(25.0f));
}

TEST_CASE("Range gain refuses nonsense", "[engine][edits][gain]") {
    Fixture fixture{1000};
    Document& document = fixture.document;
    CHECK_FALSE(applyRangeGain(document, 500, 500, 0.5f).ok());
    CHECK_FALSE(applyRangeGain(document, 600, 200, 0.5f).ok());
    CHECK_FALSE(applyRangeGain(document, 0, 100, -1.0f).ok());
    CHECK_FALSE(applyRangeGain(document, 0, 100, std::numeric_limits<float>::quiet_NaN()).ok());
    CHECK_FALSE(applyRangeGain(document, 0, 100, std::numeric_limits<float>::infinity()).ok());
}

TEST_CASE("Splitting at an edge, in a gap or past the end is a no-op", "[engine][edits][split]") {
    Fixture fixture{1000};
    Document& document = fixture.document;
    const std::size_t before = document.timeline().clips().size();

    CHECK(splitAt(document, 0).ok());
    CHECK(splitAt(document, 1000).ok());
    CHECK(splitAt(document, 99999).ok());
    CHECK(document.timeline().clips().size() == before);

    CHECK(splitAt(document, 500).ok());
    CHECK(document.timeline().clips().size() == before + 1);

    CHECK_FALSE(splitAt(document, -1).ok());
}

TEST_CASE("A fade in over a range reaches full level at its end", "[engine][edits][fade]") {
    Fixture fixture{1000};
    Document& document = fixture.document;
    REQUIRE(applyRangeFade(document, 0, 400, true, FadeShape::Linear).ok());

    AudioBuffer out{ChannelLayout::stereo(), 1000};
    REQUIRE(document.render(0, out.view()).ok());

    // Silent at the start, half way up in the middle, untouched past the end.
    CHECK(out.channel(0)[0] == Approx(0.0f).margin(1e-6));
    CHECK(out.channel(0)[200] == Approx(100.0f).margin(1.0f));
    CHECK(out.channel(0)[500] == Approx(500.0f));
}

TEST_CASE("A fade across several clips is flattened rather than restarted",
          "[engine][edits][fade]") {
    // Two internal boundaries mean three clips under the range. Without
    // flattening each would restart the curve, which shows up as the level
    // jumping back up at every seam.
    Fixture fixture{1200};
    Document& document = fixture.document;
    REQUIRE(splitAt(document, 200).ok());
    REQUIRE(splitAt(document, 400).ok());
    REQUIRE(document.timeline().clips().size() == 3);

    REQUIRE(applyRangeFade(document, 0, 600, true, FadeShape::Linear).ok());

    AudioBuffer out{ChannelLayout::stereo(), 600};
    REQUIRE(document.render(0, out.view()).ok());

    // The applied gain must rise monotonically across the whole range. The
    // source is a ramp, so divide it out first.
    float previous = -1.0f;
    int drops = 0;
    for (SampleCount i = 10; i < 600; i += 10) {
        const float gain = out.channel(0)[i] / static_cast<float>(i);
        if (gain < previous - 1e-4f) {
            ++drops;
        }
        previous = gain;
    }
    CHECK(drops == 0);
    CHECK(out.channel(0)[599] / 599.0f == Approx(1.0f).margin(0.02f));
}

TEST_CASE("Flattening a range preserves what it sounded like", "[engine][edits][flatten]") {
    Fixture fixture{1000};
    Document& document = fixture.document;
    REQUIRE(applyRangeGain(document, 200, 600, 0.25f).ok());

    AudioBuffer before{ChannelLayout::stereo(), 1000};
    REQUIRE(document.render(0, before.view()).ok());

    REQUIRE(flattenRange(document, 100, 800).ok());
    CHECK(document.duration() == 1000);

    AudioBuffer after{ChannelLayout::stereo(), 1000};
    REQUIRE(document.render(0, after.view()).ok());

    for (SampleCount i = 0; i < 1000; ++i) {
        REQUIRE(after.channel(0)[i] == Approx(before.channel(0)[i]).margin(1e-4));
    }
}

TEST_CASE("Replacing a range puts processed audio back in place", "[engine][edits][replace]") {
    Fixture fixture{1000};
    Document& document = fixture.document;

    AudioBuffer replacement{ChannelLayout::stereo(), 200};
    for (SampleCount i = 0; i < 200; ++i) {
        replacement.channel(0)[i] = -1.0f;
        replacement.channel(1)[i] = -2.0f;
    }
    REQUIRE(replaceRange(document, 400, std::move(replacement)).ok());

    // Length unchanged: surrounding material must not move.
    CHECK(document.duration() == 1000);

    AudioBuffer out{ChannelLayout::stereo(), 1000};
    REQUIRE(document.render(0, out.view()).ok());
    CHECK(out.channel(0)[399] == Approx(399.0f));
    CHECK(out.channel(0)[400] == Approx(-1.0f));
    CHECK(out.channel(1)[500] == Approx(-2.0f));
    CHECK(out.channel(0)[599] == Approx(-1.0f));
    CHECK(out.channel(0)[600] == Approx(600.0f));
}

TEST_CASE("Replacing refuses a mismatched or empty buffer", "[engine][edits][replace]") {
    Fixture fixture{1000};
    Document& document = fixture.document;

    CHECK_FALSE(replaceRange(document, 0, AudioBuffer{ChannelLayout::stereo(), 0}).ok());
    CHECK_FALSE(replaceRange(document, 0, AudioBuffer{ChannelLayout::mono(), 100}).ok());
    CHECK_FALSE(replaceRange(document, -1, AudioBuffer{ChannelLayout::stereo(), 100}).ok());
}
