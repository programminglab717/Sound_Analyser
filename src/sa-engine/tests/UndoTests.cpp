#include "TestSource.h"

#include <sa/engine/Edits.h>
#include <sa/engine/UndoHistory.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
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

    Fixture() {
        auto added = document.addSource(makeRamp(2, 1000), "ramp");
        REQUIRE(added.hasValue());
        source = added.value();
        auto placed = document.appendSource(source, 0);
        REQUIRE(placed.hasValue());
        clip = placed.value();
    }
};

std::vector<float> render(const Document& document, SampleCount frames = 1200) {
    AudioBuffer buffer{ChannelLayout::stereo(), frames};
    REQUIRE(document.render(0, buffer.view()).ok());
    return std::vector<float>{buffer.channel(0), buffer.channel(0) + frames};
}

void requireSameRender(const std::vector<float>& a, const std::vector<float>& b) {
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        REQUIRE(b[i] == Approx(a[i]));
    }
}

} // namespace

TEST_CASE("A fresh history has a baseline and nothing to undo", "[engine][undo]") {
    Fixture fixture;
    const UndoHistory history{fixture.document};
    CHECK_FALSE(history.canUndo());
    CHECK_FALSE(history.canRedo());
    CHECK(history.depth() == 1);
    CHECK(history.position() == 0);
}

TEST_CASE("Undo restores the exact render", "[engine][undo]") {
    // The property that matters: after undo, what plays must be what played
    // before the edit -- not merely a similar-looking timeline.
    Fixture fixture;
    UndoHistory history{fixture.document};
    const auto before = render(fixture.document);

    REQUIRE(deleteRange(fixture.document, 200, 600, true).ok());
    history.commit(fixture.document, "Delete");
    const auto afterEdit = render(fixture.document);
    CHECK(afterEdit[250] != Approx(before[250]));

    REQUIRE(history.undo(fixture.document));
    requireSameRender(before, render(fixture.document));

    REQUIRE(history.redo(fixture.document));
    requireSameRender(afterEdit, render(fixture.document));
}

TEST_CASE("Undo survives a long chain of mixed edits", "[engine][undo]") {
    Fixture fixture;
    UndoHistory history{fixture.document};

    std::vector<std::vector<float>> snapshots;
    snapshots.push_back(render(fixture.document));

    REQUIRE(splitClip(fixture.document, fixture.clip, 400).hasValue());
    history.commit(fixture.document, "Split");
    snapshots.push_back(render(fixture.document));

    REQUIRE(setClipGain(fixture.document, fixture.clip, 0.5f).ok());
    history.commit(fixture.document, "Gain");
    snapshots.push_back(render(fixture.document));

    REQUIRE(insertSilence(fixture.document, 200, 100).ok());
    history.commit(fixture.document, "Insert silence");
    snapshots.push_back(render(fixture.document));

    REQUIRE(deleteRange(fixture.document, 50, 150, true).ok());
    history.commit(fixture.document, "Delete");
    snapshots.push_back(render(fixture.document));

    // Walk all the way back, checking each step reproduces exactly.
    for (std::size_t i = snapshots.size() - 1; i > 0; --i) {
        REQUIRE(history.undo(fixture.document));
        INFO("undoing to step " << i - 1);
        requireSameRender(snapshots[i - 1], render(fixture.document));
    }
    CHECK_FALSE(history.canUndo());

    // And all the way forward again.
    for (std::size_t i = 1; i < snapshots.size(); ++i) {
        REQUIRE(history.redo(fixture.document));
        INFO("redoing to step " << i);
        requireSameRender(snapshots[i], render(fixture.document));
    }
    CHECK_FALSE(history.canRedo());
}

TEST_CASE("Committing after an undo discards the redo branch", "[engine][undo]") {
    Fixture fixture;
    UndoHistory history{fixture.document};

    REQUIRE(setClipGain(fixture.document, fixture.clip, 0.5f).ok());
    history.commit(fixture.document, "Gain 0.5");
    REQUIRE(setClipGain(fixture.document, fixture.clip, 0.25f).ok());
    history.commit(fixture.document, "Gain 0.25");

    REQUIRE(history.undo(fixture.document));
    CHECK(history.canRedo());

    REQUIRE(moveClip(fixture.document, fixture.clip, 100).ok());
    history.commit(fixture.document, "Move");

    CHECK_FALSE(history.canRedo());
    CHECK(history.undoLabel() == "Move");
}

TEST_CASE("Ids are restored, so stale references cannot be retargeted", "[engine][undo]") {
    // If undo rewound the clip list but not the id counter, the next edit would
    // hand out an id that an existing reference already names, and that
    // reference would silently start pointing at different audio.
    Fixture fixture;
    UndoHistory history{fixture.document};

    auto split = splitClip(fixture.document, fixture.clip, 500);
    REQUIRE(split.hasValue());
    const ClipId secondHalf = split.value().second;
    history.commit(fixture.document, "Split");

    REQUIRE(history.undo(fixture.document));

    auto duplicate = duplicateClip(fixture.document, fixture.clip, 2000);
    REQUIRE(duplicate.hasValue());
    CHECK(duplicate.value() == secondHalf); // the id is reused, which is fine...

    // ...because the clip it named is gone. What must not happen is two live
    // clips sharing an id.
    std::vector<ClipId> ids;
    for (const Clip& clip : fixture.document.timeline().clips()) {
        ids.push_back(clip.id);
    }
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
}

TEST_CASE("Coalescing collapses a continuous gesture", "[engine][undo]") {
    // Dragging a gain slider emits an edit per mouse move. Without coalescing,
    // one drag buries every earlier edit under hundreds of undo steps.
    Fixture fixture;
    UndoHistory history{fixture.document};

    REQUIRE(moveClip(fixture.document, fixture.clip, 10).ok());
    history.commit(fixture.document, "Move");
    const std::size_t afterFirst = history.depth();

    for (int i = 0; i < 100; ++i) {
        REQUIRE(setClipGain(fixture.document, fixture.clip, 0.5f + static_cast<float>(i) * 0.001f)
                    .ok());
        history.commitCoalescing(fixture.document, "Adjust gain");
    }

    CHECK(history.depth() == afterFirst + 1);

    // One undo reverses the whole gesture, back to where the move left it.
    REQUIRE(history.undo(fixture.document));
    CHECK(fixture.document.timeline().find(fixture.clip)->gain == Approx(1.0f));
    CHECK(fixture.document.timeline().find(fixture.clip)->timelineStart == 10);
}

TEST_CASE("Coalescing does not merge across a different edit", "[engine][undo]") {
    Fixture fixture;
    UndoHistory history{fixture.document};

    REQUIRE(setClipGain(fixture.document, fixture.clip, 0.5f).ok());
    history.commitCoalescing(fixture.document, "Adjust gain");
    REQUIRE(moveClip(fixture.document, fixture.clip, 50).ok());
    history.commitCoalescing(fixture.document, "Move");
    REQUIRE(setClipGain(fixture.document, fixture.clip, 0.25f).ok());
    history.commitCoalescing(fixture.document, "Adjust gain");

    CHECK(history.depth() == 4); // baseline plus three distinct steps
}

TEST_CASE("History is capped and drops the oldest entries", "[engine][undo]") {
    Fixture fixture;
    UndoHistory history{fixture.document, 5};

    for (int i = 0; i < 20; ++i) {
        REQUIRE(moveClip(fixture.document, fixture.clip, i * 10).ok());
        history.commit(fixture.document, "Move " + std::to_string(i));
    }

    CHECK(history.depth() == 5);

    // The reachable history is still consistent: undoing to the oldest kept
    // entry must not crash or restore a half-written state.
    while (history.canUndo()) {
        REQUIRE(history.undo(fixture.document));
    }
    CHECK(fixture.document.timeline().clipCount() == 1);
}

TEST_CASE("Restoring by index jumps straight to a step", "[engine][undo]") {
    Fixture fixture;
    UndoHistory history{fixture.document};

    std::vector<std::vector<float>> snapshots{render(fixture.document)};
    for (int i = 1; i <= 4; ++i) {
        REQUIRE(moveClip(fixture.document, fixture.clip, i * 100).ok());
        history.commit(fixture.document, "Move " + std::to_string(i));
        snapshots.push_back(render(fixture.document));
    }

    REQUIRE(history.restore(2, fixture.document));
    CHECK(history.position() == 2);
    requireSameRender(snapshots[2], render(fixture.document));

    REQUIRE(history.restore(0, fixture.document));
    requireSameRender(snapshots[0], render(fixture.document));

    CHECK_FALSE(history.restore(999, fixture.document));
}

TEST_CASE("Labels describe the steps", "[engine][undo]") {
    Fixture fixture;
    UndoHistory history{fixture.document};

    REQUIRE(splitClip(fixture.document, fixture.clip, 500).hasValue());
    history.commit(fixture.document, "Split clip");

    CHECK(history.undoLabel() == "Split clip");
    CHECK(history.redoLabel().empty());
    CHECK(history.labels().size() == 2);

    REQUIRE(history.undo(fixture.document));
    CHECK(history.redoLabel() == "Split clip");
    CHECK(history.undoLabel().empty());
}

TEST_CASE("Sources survive undo", "[engine][undo]") {
    Fixture fixture;
    UndoHistory history{fixture.document};

    auto extra = fixture.document.addSource(makeRamp(2, 500), "second");
    REQUIRE(extra.hasValue());
    REQUIRE(fixture.document.appendSource(extra.value(), 2000).hasValue());
    history.commit(fixture.document, "Add source");

    CHECK(fixture.document.sourceCount() == 2);
    REQUIRE(history.undo(fixture.document));

    // Undoing the placement also unregisters the source, which is right: the
    // document should not accumulate references to files the user rolled back.
    CHECK(fixture.document.sourceCount() == 1);
    CHECK(fixture.document.timeline().clipCount() == 1);

    REQUIRE(history.redo(fixture.document));
    CHECK(fixture.document.sourceCount() == 2);
    CHECK(fixture.document.source(extra.value()) != nullptr);
}
