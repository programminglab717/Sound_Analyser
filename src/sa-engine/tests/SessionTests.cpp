#include "TestSource.h"

#include <sa/engine/Edits.h>
#include <sa/engine/SessionFile.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

using namespace sa;
using namespace sa::engine;
using namespace sa::engine::test;
using Catch::Approx;

namespace {

/// Resolves from an in-memory table, so session tests never touch the disk and
/// a "missing file" is just a name that is not in the map.
class FakeResolver final : public SourceResolver {
public:
    void add(const std::filesystem::path& path, std::shared_ptr<const io::AudioSource> audio) {
        table_[path.generic_string()] = std::move(audio);
    }

    [[nodiscard]] Result<std::shared_ptr<const io::AudioSource>>
    open(const std::filesystem::path& path) override {
        ++opened;
        const auto found = table_.find(path.generic_string());
        if (found == table_.end()) {
            return Error{ErrorCode::NotFound, "no such file: " + path.generic_string()};
        }
        return found->second;
    }

    int opened = 0;

private:
    std::map<std::string, std::shared_ptr<const io::AudioSource>> table_;
};

struct Fixture {
    Document document{kSampleRate48000, ChannelLayout::stereo()};
    FakeResolver resolver;
    SourceId source = SourceId::Invalid;
    ClipId clip = ClipId::Invalid;

    Fixture() {
        auto audio = makeRamp(2, 1000);
        resolver.add("takes/one.wav", audio);
        auto added = document.addSource(audio, "one", "takes/one.wav");
        REQUIRE(added.hasValue());
        source = added.value();
        auto placed = document.appendSource(source, 100);
        REQUIRE(placed.hasValue());
        clip = placed.value();
    }
};

std::vector<float> render(const Document& document, SampleCount frames = 1500) {
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

TEST_CASE("A session round-trips to identical audio", "[engine][session]") {
    // The property that matters: reopening a session must play what it played
    // before, not merely look similar in a clip list.
    Fixture fixture;
    REQUIRE(setClipGain(fixture.document, fixture.clip, 0.5f).ok());
    REQUIRE(setClipFades(fixture.document, fixture.clip, Fade{50, FadeShape::Linear},
                         Fade{80, FadeShape::SCurve})
                .ok());
    REQUIRE(splitClip(fixture.document, fixture.clip, 500).hasValue());
    fixture.document.markers().push_back(Marker{250, 100, "verse"});

    const auto before = render(fixture.document);

    auto json = sessionToJson(fixture.document);
    REQUIRE(json.hasValue());

    auto loaded = sessionFromJson(json.value(), fixture.resolver);
    REQUIRE(loaded.hasValue());
    CHECK(loaded.value().missingSources.empty());
    CHECK(loaded.value().warnings.empty());

    requireSameRender(before, render(loaded.value().document));
}

TEST_CASE("Clip and marker detail survives the round-trip", "[engine][session]") {
    Fixture fixture;
    REQUIRE(setClipFades(fixture.document, fixture.clip, Fade{30, FadeShape::Exponential},
                         Fade{40, FadeShape::Logarithmic})
                .ok());
    fixture.document.markers().push_back(Marker{500, 0, "point"});
    fixture.document.markers().push_back(Marker{700, 120, "region"});

    auto json = sessionToJson(fixture.document);
    REQUIRE(json.hasValue());
    auto loaded = sessionFromJson(json.value(), fixture.resolver);
    REQUIRE(loaded.hasValue());

    const Document& restored = loaded.value().document;
    REQUIRE(restored.timeline().clipCount() == 1);
    const Clip& clip = restored.timeline().clips().front();
    CHECK(clip.timelineStart == 100);
    CHECK(clip.length == 1000);
    CHECK(clip.fadeIn.length == 30);
    CHECK(clip.fadeIn.shape == FadeShape::Exponential);
    CHECK(clip.fadeOut.length == 40);
    CHECK(clip.fadeOut.shape == FadeShape::Logarithmic);

    REQUIRE(restored.markers().size() == 2);
    CHECK(restored.markers()[0].position == 500);
    CHECK(restored.markers()[1].length == 120);
    CHECK(restored.markers()[1].label == "region");
}

TEST_CASE("A moved audio file loads as silence, not as a lost project", "[engine][session]") {
    // The decision that matters most here. Refusing to load because one file
    // moved would destroy the arrangement; substituting silence of the recorded
    // length keeps every clip, position and fade intact so the UI can relink.
    Fixture fixture;
    const auto before = render(fixture.document);

    auto json = sessionToJson(fixture.document);
    REQUIRE(json.hasValue());

    FakeResolver empty; // knows about nothing
    auto loaded = sessionFromJson(json.value(), empty);
    REQUIRE(loaded.hasValue());

    REQUIRE(loaded.value().missingSources.size() == 1);
    CHECK(loaded.value().missingSources[0].path == std::filesystem::path{"takes/one.wav"});
    CHECK_FALSE(loaded.value().missingSources[0].reason.empty());

    // The arrangement survives: same clip count, same duration, same positions.
    const Document& restored = loaded.value().document;
    CHECK(restored.timeline().clipCount() == 1);
    CHECK(restored.duration() == 1100);
    CHECK(restored.timeline().clips().front().timelineStart == 100);

    // And it renders silence rather than crashing or reading garbage.
    const auto after = render(restored);
    for (float value : after) {
        REQUIRE(value == 0.0f);
    }
    CHECK(before[500] != 0.0f); // the original genuinely had audio there
}

TEST_CASE("Relinking a missing source restores the audio", "[engine][session]") {
    Fixture fixture;
    const auto before = render(fixture.document);
    auto json = sessionToJson(fixture.document);
    REQUIRE(json.hasValue());

    FakeResolver relocated;
    relocated.add("takes/one.wav", makeRamp(2, 1000));

    auto loaded = sessionFromJson(json.value(), relocated);
    REQUIRE(loaded.hasValue());
    CHECK(loaded.value().missingSources.empty());
    requireSameRender(before, render(loaded.value().document));
}

TEST_CASE("Source paths are stored relative to the session file", "[engine][session]") {
    // So a project folder can be moved, zipped, or handed to someone else and
    // still open. An absolute path would break the moment it left this machine.
    Document document{kSampleRate48000, ChannelLayout::stereo()};
    auto added = document.addSource(makeRamp(2, 100), "take", "/projects/song/audio/take.wav");
    REQUIRE(added.hasValue());
    REQUIRE(document.appendSource(added.value(), 0).hasValue());

    auto json = sessionToJson(document, "/projects/song");
    REQUIRE(json.hasValue());
    CHECK(json.value().find("audio/take.wav") != std::string::npos);
    CHECK(json.value().find("/projects/song/audio") == std::string::npos);

    // And it resolves back against a different base, which is what moving the
    // folder amounts to.
    FakeResolver resolver;
    resolver.add("/elsewhere/audio/take.wav", makeRamp(2, 100));
    auto loaded = sessionFromJson(json.value(), resolver, "/elsewhere");
    REQUIRE(loaded.hasValue());
    CHECK(loaded.value().missingSources.empty());
}

TEST_CASE("A path outside the session folder stays absolute", "[engine][session]") {
    // A relative path climbing out of the project folder is more fragile than
    // an absolute one, not less.
    Document document{kSampleRate48000, ChannelLayout::stereo()};
    auto added = document.addSource(makeRamp(2, 100), "shared", "/library/sfx/rain.wav");
    REQUIRE(added.hasValue());

    auto json = sessionToJson(document, "/projects/song");
    REQUIRE(json.hasValue());
    CHECK(json.value().find("/library/sfx/rain.wav") != std::string::npos);
    CHECK(json.value().find("../") == std::string::npos);
}

TEST_CASE("A newer session version is refused rather than guessed", "[engine][session]") {
    // Loading a newer file by ignoring fields we do not understand produces a
    // session that looks right and is not -- and then overwrites the original.
    Fixture fixture;
    auto json = sessionToJson(fixture.document);
    REQUIRE(json.hasValue());

    std::string tampered = json.value();
    const auto position = tampered.find("\"version\": 1");
    REQUIRE(position != std::string::npos);
    tampered.replace(position, 12, "\"version\": 99");

    auto loaded = sessionFromJson(tampered, fixture.resolver);
    REQUIRE_FALSE(loaded.hasValue());
    CHECK(loaded.error().code() == ErrorCode::UnsupportedFormat);
    CHECK(std::string{loaded.error().what()}.find("newer version") != std::string::npos);
}

TEST_CASE("Malformed and foreign files are rejected", "[engine][session]") {
    FakeResolver resolver;

    CHECK_FALSE(sessionFromJson("", resolver).hasValue());
    CHECK_FALSE(sessionFromJson("not json at all", resolver).hasValue());
    CHECK_FALSE(sessionFromJson("{ unterminated", resolver).hasValue());
    CHECK_FALSE(sessionFromJson("[1, 2, 3]", resolver).hasValue());
    CHECK_FALSE(sessionFromJson("null", resolver).hasValue());

    // Valid JSON, wrong document.
    CHECK_FALSE(sessionFromJson(R"({"format":"something-else","version":1})", resolver).hasValue());
    CHECK_FALSE(sessionFromJson(R"({"format":"auscultate-session"})", resolver).hasValue());
}

TEST_CASE("Degenerate header values are rejected", "[engine][session]") {
    FakeResolver resolver;
    const auto attempt = [&resolver](const char* body) {
        return sessionFromJson(body, resolver).hasValue();
    };

    CHECK_FALSE(attempt(R"({"format":"auscultate-session","version":1,
                            "sampleRate":0,"channels":2})"));
    CHECK_FALSE(attempt(R"({"format":"auscultate-session","version":1,
                            "sampleRate":-48000,"channels":2})"));
    CHECK_FALSE(attempt(R"({"format":"auscultate-session","version":1,
                            "sampleRate":48000,"channels":0})"));
    CHECK_FALSE(attempt(R"({"format":"auscultate-session","version":1,
                            "sampleRate":48000,"channels":9999})"));
    CHECK_FALSE(attempt(R"({"format":"auscultate-session","version":0,
                            "sampleRate":48000,"channels":2})"));
    CHECK(attempt(R"({"format":"auscultate-session","version":1,
                      "sampleRate":48000,"channels":2})"));
}

TEST_CASE("Structurally invalid entries are dropped and reported", "[engine][session]") {
    // A user whose session lost a clip deserves to be told which one, rather
    // than quietly getting a different arrangement than they saved.
    FakeResolver resolver;
    resolver.add("a.wav", makeRamp(2, 500));

    const std::string json = R"({
      "format": "auscultate-session",
      "version": 1,
      "sampleRate": 48000,
      "channels": 2,
      "nextId": 10,
      "sources": [{"id": 1, "path": "a.wav", "name": "a", "frameCount": 500}],
      "clips": [
        {"id": 2, "source": 1, "sourceStart": 0, "length": 100, "timelineStart": 0},
        {"id": 3, "source": 77, "sourceStart": 0, "length": 100, "timelineStart": 0},
        {"id": 4, "source": 1, "sourceStart": 0, "length": -5, "timelineStart": 0},
        {"id": 5, "source": 1, "sourceStart": -1, "length": 100, "timelineStart": 0},
        {"id": 6, "source": 1, "length": 100, "timelineStart": 0}
      ],
      "markers": [{"position": 10, "label": "ok"}, {"position": -4, "label": "bad"}]
    })";

    auto loaded = sessionFromJson(json, resolver);
    REQUIRE(loaded.hasValue());

    CHECK(loaded.value().document.timeline().clipCount() == 1); // only the valid one
    CHECK(loaded.value().document.markers().size() == 1);
    CHECK(loaded.value().warnings.size() == 5); // four clips, one marker
}

TEST_CASE("A source with no path loads as silence", "[engine][session]") {
    // Recorded-but-unsaved audio has no file behind it yet. It should not stop
    // the session opening.
    FakeResolver resolver;
    const std::string json = R"({
      "format": "auscultate-session", "version": 1,
      "sampleRate": 48000, "channels": 2,
      "sources": [{"id": 1, "name": "recorded", "frameCount": 480}],
      "clips": [{"id": 2, "source": 1, "sourceStart": 0, "length": 480, "timelineStart": 0}]
    })";

    auto loaded = sessionFromJson(json, resolver);
    REQUIRE(loaded.hasValue());
    CHECK(loaded.value().missingSources.size() == 1);
    CHECK(loaded.value().document.duration() == 480);
    CHECK(resolver.opened == 0); // no pointless attempt to open an empty path
}

TEST_CASE("Loading never reissues a live clip id", "[engine][session]") {
    // A file could claim a nextId below an id it also uses. Trusting it would
    // hand the next edit an id a live clip already holds.
    FakeResolver resolver;
    resolver.add("a.wav", makeRamp(2, 500));

    const std::string json = R"({
      "format": "auscultate-session", "version": 1,
      "sampleRate": 48000, "channels": 2,
      "nextId": 2,
      "sources": [{"id": 1, "path": "a.wav", "frameCount": 500}],
      "clips": [{"id": 900, "source": 1, "sourceStart": 0, "length": 100, "timelineStart": 0}]
    })";

    auto loaded = sessionFromJson(json, resolver);
    REQUIRE(loaded.hasValue());
    Document& document = loaded.value().document;
    CHECK(document.nextId() > 900);

    auto duplicate = duplicateClip(document, static_cast<ClipId>(900), 5000);
    REQUIRE(duplicate.hasValue());
    CHECK(duplicate.value() != static_cast<ClipId>(900));
}

TEST_CASE("A fade longer than its clip is clamped on load", "[engine][session]") {
    FakeResolver resolver;
    resolver.add("a.wav", makeRamp(2, 500));

    const std::string json = R"({
      "format": "auscultate-session", "version": 1,
      "sampleRate": 48000, "channels": 2,
      "sources": [{"id": 1, "path": "a.wav", "frameCount": 500}],
      "clips": [{"id": 2, "source": 1, "sourceStart": 0, "length": 100, "timelineStart": 0,
                 "fadeIn": {"length": 9999, "shape": "linear"},
                 "fadeOut": {"length": 9999, "shape": "linear"}}]
    })";

    auto loaded = sessionFromJson(json, resolver);
    REQUIRE(loaded.hasValue());
    const Clip& clip = loaded.value().document.timeline().clips().front();
    CHECK(clip.fadeIn.length <= 100);
    CHECK(clip.fadeOut.length <= 100);
}

TEST_CASE("Saving and loading through the filesystem works", "[engine][session]") {
    const auto directory = std::filesystem::temp_directory_path() / "sa-session-test";
    std::filesystem::create_directories(directory);
    const auto path = directory / "project.sasession";

    Fixture fixture;
    REQUIRE(saveSession(fixture.document, path).ok());
    REQUIRE(std::filesystem::exists(path));

    auto loaded = loadSession(path, fixture.resolver);
    REQUIRE(loaded.hasValue());
    CHECK(loaded.value().document.timeline().clipCount() == 1);

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

TEST_CASE("Saving to an unwritable path fails cleanly", "[engine][session]") {
    Fixture fixture;
    auto status = saveSession(fixture.document, "/nonexistent/dir/project.sasession");
    REQUIRE_FALSE(status.ok());
    CHECK(status.error().code() == ErrorCode::IoFailure);

    FakeResolver resolver;
    auto loaded = loadSession("/nonexistent/dir/project.sasession", resolver);
    REQUIRE_FALSE(loaded.hasValue());
}

TEST_CASE("An empty document round-trips", "[engine][session]") {
    const Document empty{kSampleRate44100, ChannelLayout::mono()};
    auto json = sessionToJson(empty);
    REQUIRE(json.hasValue());

    FakeResolver resolver;
    auto loaded = sessionFromJson(json.value(), resolver);
    REQUIRE(loaded.hasValue());
    CHECK(loaded.value().document.timeline().isEmpty());
    CHECK(loaded.value().document.sampleRate() == kSampleRate44100);
    CHECK(loaded.value().document.layout().count() == 1);
}
