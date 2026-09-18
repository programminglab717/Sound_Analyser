#include "TestSource.h"

#include <sa/engine/BufferSource.h>
#include <sa/engine/Consolidate.h>
#include <sa/engine/SessionFile.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <memory>
#include <random>

using namespace sa;
using namespace sa::engine;
using namespace sa::engine::test;
using Catch::Approx;

namespace {

/// A directory that cleans up after itself, so a failing test does not leave
/// megabytes of WAV behind.
struct Workspace {
    std::filesystem::path path;

    Workspace() {
        path = std::filesystem::temp_directory_path() /
               ("sa-consolidate-" + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(path);
    }

    ~Workspace() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;
};

AudioBuffer rampBuffer(SampleCount frames) {
    AudioBuffer buffer{ChannelLayout::stereo(), frames};
    for (SampleCount i = 0; i < frames; ++i) {
        buffer.channel(0)[i] = static_cast<float>(i) / 100000.0f;
        buffer.channel(1)[i] = -static_cast<float>(i) / 100000.0f;
    }
    return buffer;
}

} // namespace

TEST_CASE("Consolidation writes in-memory sources and points the document at them",
          "[engine][consolidate]") {
    const Workspace workspace;
    Document document{kSampleRate48000, ChannelLayout::stereo()};

    auto memory = document.addSource(
        std::make_shared<BufferSource>(rampBuffer(5000), kSampleRate48000), "pasted");
    REQUIRE(memory.hasValue());
    REQUIRE(document.appendSource(memory.value(), 0).hasValue());

    REQUIRE(document.source(memory.value())->path.empty());

    const auto result = consolidateSources(document, workspace.path / "project.sa");
    CHECK(result.failures.empty());
    REQUIRE(result.written.size() == 1);
    CHECK(std::filesystem::exists(result.written.front()));

    const std::filesystem::path recorded = document.source(memory.value())->path;
    CHECK_FALSE(recorded.empty());
    CHECK(recorded == result.written.front());
    CHECK(recorded.parent_path().filename() == "project.media");
}

TEST_CASE("A consolidated session reopens with its audio intact",
          "[engine][consolidate][session]") {
    // The whole point. Without consolidation the clips survive and the audio
    // does not, which reopens looking correct and sounding like silence.
    const Workspace workspace;
    const std::filesystem::path sessionPath = workspace.path / "project.sa";

    Document document{kSampleRate48000, ChannelLayout::stereo()};
    auto memory = document.addSource(
        std::make_shared<BufferSource>(rampBuffer(5000), kSampleRate48000), "pasted");
    REQUIRE(memory.hasValue());
    REQUIRE(document.appendSource(memory.value(), 0).hasValue());

    REQUIRE(consolidateSources(document, sessionPath).failures.empty());
    REQUIRE(saveSession(document, sessionPath).ok());

    FileSourceResolver resolver;
    auto loaded = loadSession(sessionPath, resolver);
    REQUIRE(loaded.hasValue());
    CHECK(loaded.value().missingSources.empty());
    CHECK(loaded.value().document.duration() == 5000);

    AudioBuffer out{ChannelLayout::stereo(), 5000};
    REQUIRE(loaded.value().document.render(0, out.view()).ok());
    CHECK(out.channel(0)[0] == Approx(0.0f).margin(1e-6));
    CHECK(out.channel(0)[4999] == Approx(4999.0f / 100000.0f).margin(1e-6));
    CHECK(out.channel(1)[2500] == Approx(-2500.0f / 100000.0f).margin(1e-6));
}

TEST_CASE("Consolidation leaves sources that already have a file alone", "[engine][consolidate]") {
    const Workspace workspace;
    Document document{kSampleRate48000, ChannelLayout::stereo()};

    auto onDisk = document.addSource(makeRamp(2, 1000), "file", "/somewhere/real.wav");
    REQUIRE(onDisk.hasValue());

    const auto result = consolidateSources(document, workspace.path / "project.sa");
    CHECK(result.written.empty());
    CHECK(result.failures.empty());
    CHECK(document.source(onDisk.value())->path == "/somewhere/real.wav");

    // And no folder is created for nothing.
    CHECK_FALSE(std::filesystem::exists(workspace.path / "project.media"));
}

TEST_CASE("Consolidation preserves full float precision", "[engine][consolidate]") {
    // Intermediate audio inside a project must not be quantised on every save:
    // that is a loss the user never asked for and cannot undo.
    const Workspace workspace;
    Document document{kSampleRate48000, ChannelLayout::stereo()};

    AudioBuffer awkward{ChannelLayout::stereo(), 64};
    for (SampleCount i = 0; i < 64; ++i) {
        // Values no integer format represents exactly.
        awkward.channel(0)[i] = 0.1234567f * static_cast<float>(i % 7) - 0.3f;
        awkward.channel(1)[i] = -0.9876543f + static_cast<float>(i) * 0.001f;
    }
    AudioBuffer expected{ChannelLayout::stereo(), 64};
    for (int channel = 0; channel < 2; ++channel) {
        std::copy_n(awkward.channel(channel), 64, expected.channel(channel));
    }

    auto memory = document.addSource(
        std::make_shared<BufferSource>(std::move(awkward), kSampleRate48000), "processed");
    REQUIRE(memory.hasValue());
    REQUIRE(document.appendSource(memory.value(), 0).hasValue());

    const std::filesystem::path sessionPath = workspace.path / "project.sa";
    REQUIRE(consolidateSources(document, sessionPath).failures.empty());
    REQUIRE(saveSession(document, sessionPath).ok());

    FileSourceResolver resolver;
    auto loaded = loadSession(sessionPath, resolver);
    REQUIRE(loaded.hasValue());

    AudioBuffer out{ChannelLayout::stereo(), 64};
    REQUIRE(loaded.value().document.render(0, out.view()).ok());
    for (int channel = 0; channel < 2; ++channel) {
        for (SampleCount i = 0; i < 64; ++i) {
            REQUIRE(out.channel(channel)[i] == expected.channel(channel)[i]);
        }
    }
}
