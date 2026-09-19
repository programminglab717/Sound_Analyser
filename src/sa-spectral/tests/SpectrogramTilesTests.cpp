#include <sa/spectral/SpectrogramTiles.h>

#include <algorithm>
#include <atomic>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <memory>
#include <numbers>
#include <random>
#include <thread>
#include <vector>

using namespace sa;
using namespace sa::spectral;

namespace {

/// An AudioSource over a buffer, so a tile can be compared against the eager
/// pyramid on exactly the same samples.
class BufferSource final : public io::AudioSource {
public:
    explicit BufferSource(AudioBuffer audio) : audio_(std::move(audio)) {
        info_.sampleRate = SampleRate{48000.0};
        info_.layout = audio_.layout();
        info_.frameCount = audio_.frames();
        info_.format = io::SampleFormat::Float32;
    }

    [[nodiscard]] const io::AudioFileInfo& info() const noexcept override { return info_; }

    [[nodiscard]] Result<SampleCount> read(SampleIndex startFrame,
                                           AudioBufferView destination) const override {
        if (startFrame < 0 || startFrame >= info_.frameCount) {
            return SampleCount{0};
        }
        const SampleCount count =
            std::min<SampleCount>(destination.frames(), info_.frameCount - startFrame);
        for (int channel = 0; channel < destination.channelCount(); ++channel) {
            std::copy_n(audio_.channel(channel) + startFrame, count, destination.channel(channel));
        }
        return count;
    }

private:
    AudioBuffer audio_;
    io::AudioFileInfo info_;
};

/// Material with structure at both ends of the time axis: a sweep so every
/// frame differs from its neighbours, plus isolated clicks, which are what the
/// max-combining exists to preserve.
[[nodiscard]] AudioBuffer material(SampleCount frames, unsigned seed = 4) {
    std::mt19937 engine{seed};
    std::normal_distribution<float> hiss{0.0f, 0.02f};
    AudioBuffer buffer{ChannelLayout::mono(), frames};
    double phase = 0.0;
    for (SampleCount i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(frames);
        const double hz = 100.0 * std::exp(t * std::log(180.0));
        phase += 2.0 * std::numbers::pi * hz / 48000.0;
        buffer.channel(0)[i] = static_cast<float>(0.5 * std::sin(phase)) + hiss(engine);
    }
    // A click every 6000 samples: one frame wide at the default hop.
    for (SampleCount i = 3000; i < frames; i += 6000) {
        buffer.channel(0)[i] = 0.95f;
    }
    return buffer;
}

/// AudioBuffer is move-only by design, and these tests want the same samples
/// in both a buffer and a source, so the copy is explicit here.
[[nodiscard]] AudioBuffer clone(const AudioBuffer& audio) {
    AudioBuffer copy{audio.layout(), audio.frames()};
    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        std::copy_n(audio.channel(channel), audio.frames(), copy.channel(channel));
    }
    return copy;
}

[[nodiscard]] std::shared_ptr<const io::AudioSource> sourceOf(const AudioBuffer& audio) {
    return std::make_shared<const BufferSource>(clone(audio));
}

[[nodiscard]] SpectrogramTiles::Settings smallSettings() {
    SpectrogramTiles::Settings settings;
    settings.coarseLevel = 3; // 8 STFT frames per overview frame.
    settings.tileFrames = 32; // A multiple of 8, and small enough to evict in a test.
    settings.detailBudgetBytes = std::size_t{1} << 30;
    return settings;
}

} // namespace

TEST_CASE("A detail tile is bit-identical to the eager pyramid over the same frames",
          "[spectral][tiles]") {
    // The whole claim. A tiled cache that quietly re-frames its STFT still
    // looks like a spectrogram; it just disagrees with the one the rest of the
    // program draws, and nothing but this would say so.
    const AudioBuffer buffer = material(60000);
    const auto eager = SpectrogramPyramid::build(buffer.constView(), 0);
    REQUIRE(eager);

    auto tiles = SpectrogramTiles::create(sourceOf(buffer), 0, smallSettings());
    REQUIRE(tiles);
    REQUIRE(tiles.value().buildOverview());
    REQUIRE(tiles.value().fineFrameCount() == eager.value().frameCountAt(0));

    // Ask for the middle, so the tiles built are not the ones at frame zero.
    REQUIRE(tiles.value().ensureDetail(20000, 40000));
    REQUIRE(tiles.value().residentTileCount() > 0);

    int compared = 0;
    for (SampleCount frame = 0; frame < tiles.value().fineFrameCount(); ++frame) {
        const std::uint8_t* fine = tiles.value().fineFrame(frame);
        if (fine == nullptr) {
            continue;
        }
        const std::uint8_t* reference = eager.value().frameData(0, frame);
        REQUIRE(reference != nullptr);
        for (int bin = 0; bin < tiles.value().binCount(); ++bin) {
            REQUIRE(fine[bin] == reference[bin]);
        }
        ++compared;
    }
    REQUIRE(compared > 0);
}

TEST_CASE("Every tile of a file matches the eager pyramid, not just the middle",
          "[spectral][tiles]") {
    // The first and last tiles are where framing bugs live: the first because
    // of the front padding, the last because it is short.
    const AudioBuffer buffer = material(40000);
    const auto eager = SpectrogramPyramid::build(buffer.constView(), 0);
    REQUIRE(eager);

    auto tiles = SpectrogramTiles::create(sourceOf(buffer), 0, smallSettings());
    REQUIRE(tiles);
    REQUIRE(tiles.value().buildOverview());
    REQUIRE(tiles.value().ensureDetail(0, buffer.frames()));

    for (SampleCount frame = 0; frame < tiles.value().fineFrameCount(); ++frame) {
        const std::uint8_t* fine = tiles.value().fineFrame(frame);
        REQUIRE(fine != nullptr);
        const std::uint8_t* reference = eager.value().frameData(0, frame);
        REQUIRE(reference != nullptr);
        REQUIRE(std::equal(fine, fine + tiles.value().binCount(), reference));
    }
}

TEST_CASE("The overview is the max-combine of the fine frames, not a sample of them",
          "[spectral][tiles]") {
    // What makes the overview usable as a fallback rather than a placeholder: a
    // click one frame wide has to survive being folded down.
    const AudioBuffer buffer = material(60000);
    const auto eager = SpectrogramPyramid::build(buffer.constView(), 0);
    REQUIRE(eager);

    SpectrogramTiles::Settings settings = smallSettings();
    auto tiles = SpectrogramTiles::create(sourceOf(buffer), 0, settings);
    REQUIRE(tiles);
    REQUIRE(tiles.value().buildOverview());

    const SpectrogramPyramid& overview = tiles.value().overview();
    const SampleCount group = SampleCount{1} << settings.coarseLevel;
    REQUIRE(overview.hopAt(0) == settings.config.hopSize * group);

    for (SampleCount coarse = 0; coarse < overview.frameCountAt(0); ++coarse) {
        const std::uint8_t* got = overview.frameData(0, coarse);
        REQUIRE(got != nullptr);
        for (int bin = 0; bin < tiles.value().binCount(); ++bin) {
            std::uint8_t loudest = 0;
            for (SampleCount k = 0; k < group; ++k) {
                const SampleCount fine = coarse * group + k;
                if (fine >= eager.value().frameCountAt(0)) {
                    break;
                }
                loudest = std::max(loudest, eager.value().frameData(0, fine)[bin]);
            }
            REQUIRE(got[bin] == loudest);
        }
    }
}

TEST_CASE("The overview costs a fraction of the full pyramid", "[spectral][tiles]") {
    // The reason any of this exists.
    const AudioBuffer buffer = material(200000);
    const auto eager = SpectrogramPyramid::build(buffer.constView(), 0);
    REQUIRE(eager);

    SpectrogramTiles::Settings settings = smallSettings();
    settings.coarseLevel = 6; // 64 fine frames per overview frame.
    settings.tileFrames = 64;
    auto tiles = SpectrogramTiles::create(sourceOf(buffer), 0, settings);
    REQUIRE(tiles);
    REQUIRE(tiles.value().buildOverview());

    const std::size_t full = eager.value().memoryFootprint();
    const std::size_t coarse = tiles.value().overview().memoryFootprint();
    REQUIRE(coarse * 8 < full);
    // And no detail is resident until something asks for some.
    REQUIRE(tiles.value().residentDetailBytes() == 0);
}

TEST_CASE("Detail stays inside its budget, evicting the least recently used", "[spectral][tiles]") {
    const AudioBuffer buffer = material(200000);
    auto probe = SpectrogramTiles::create(sourceOf(buffer), 0, smallSettings());
    REQUIRE(probe);
    const auto perTile = static_cast<std::size_t>(smallSettings().tileFrames) *
                         static_cast<std::size_t>(probe.value().binCount());

    SpectrogramTiles::Settings settings = smallSettings();
    settings.detailBudgetBytes = perTile * 4; // Room for four tiles, no more.
    auto tiles = SpectrogramTiles::create(sourceOf(buffer), 0, settings);
    REQUIRE(tiles);
    REQUIRE(tiles.value().buildOverview());

    REQUIRE(tiles.value().ensureDetail(0, buffer.frames()));
    REQUIRE(tiles.value().residentDetailBytes() <= settings.detailBudgetBytes);
    REQUIRE(tiles.value().residentTileCount() <= 4);
    // Sweeping the whole file leaves the end resident, because the front was
    // evicted to make room for it.
    REQUIRE(tiles.value().fineFrame(tiles.value().fineFrameCount() - 1) != nullptr);
    REQUIRE(tiles.value().fineFrame(0) == nullptr);
}

TEST_CASE("A rebuilt tile is identical to the one that was evicted", "[spectral][tiles]") {
    // Eviction has to be free of consequence, or the picture changes depending
    // on where the user has been.
    const AudioBuffer buffer = material(120000);
    SpectrogramTiles::Settings settings = smallSettings();
    auto probe = SpectrogramTiles::create(sourceOf(buffer), 0, settings);
    REQUIRE(probe);
    settings.detailBudgetBytes = static_cast<std::size_t>(settings.tileFrames) *
                                 static_cast<std::size_t>(probe.value().binCount()) * 2;

    auto tiles = SpectrogramTiles::create(sourceOf(buffer), 0, settings);
    REQUIRE(tiles);
    REQUIRE(tiles.value().buildOverview());

    REQUIRE(tiles.value().ensureDetail(0, 4000));
    const std::uint8_t* first = tiles.value().fineFrame(0);
    REQUIRE(first != nullptr);
    const std::vector<std::uint8_t> before(first, first + tiles.value().binCount());

    // Walk away far enough to evict it, then come back.
    REQUIRE(tiles.value().ensureDetail(100000, 120000));
    REQUIRE(tiles.value().fineFrame(0) == nullptr);
    REQUIRE(tiles.value().ensureDetail(0, 4000));

    const std::uint8_t* again = tiles.value().fineFrame(0);
    REQUIRE(again != nullptr);
    REQUIRE(std::equal(before.begin(), before.end(), again));
}

TEST_CASE("Asking twice for the same range builds nothing the second time", "[spectral][tiles]") {
    const AudioBuffer buffer = material(60000);
    auto tiles = SpectrogramTiles::create(sourceOf(buffer), 0, smallSettings());
    REQUIRE(tiles);
    REQUIRE(tiles.value().buildOverview());

    REQUIRE(tiles.value().ensureDetail(10000, 30000));
    const std::size_t after = tiles.value().residentDetailBytes();
    const std::size_t count = tiles.value().residentTileCount();
    REQUIRE(count > 0);

    REQUIRE(tiles.value().ensureDetail(10000, 30000));
    REQUIRE(tiles.value().residentDetailBytes() == after);
    REQUIRE(tiles.value().residentTileCount() == count);
}

TEST_CASE("A fully detailed render equals the eager pyramid's render", "[spectral][tiles]") {
    const AudioBuffer buffer = material(60000);
    const auto eager = SpectrogramPyramid::build(buffer.constView(), 0);
    REQUIRE(eager);

    auto tiles = SpectrogramTiles::create(sourceOf(buffer), 0, smallSettings());
    REQUIRE(tiles);
    REQUIRE(tiles.value().buildOverview());
    REQUIRE(tiles.value().ensureDetail(0, buffer.frames()));

    constexpr int kRows = 24;
    constexpr int kColumns = 40;
    std::vector<float> edges(kRows + 1);
    for (int row = 0; row <= kRows; ++row) {
        edges[static_cast<std::size_t>(row)] =
            static_cast<float>(row) * static_cast<float>(tiles.value().binCount()) / kRows;
    }

    // A zoom where a column is narrower than the overview's hop, so detail is
    // what should be drawn.
    const SampleIndex from = 10000;
    const SampleIndex to = 14000;
    REQUIRE(tiles.value().detailWorthwhile((to - from) / kColumns));

    std::vector<std::uint8_t> fromTiles(static_cast<std::size_t>(kRows * kColumns));
    std::vector<std::uint8_t> fromEager(static_cast<std::size_t>(kRows * kColumns));
    tiles.value().render(from, to, edges.data(), kRows, kColumns, fromTiles.data());
    eager.value().render(from, to, edges.data(), kRows, kColumns, fromEager.data());

    REQUIRE(fromTiles == fromEager);
}

TEST_CASE("A render with no detail resident still draws, from the overview", "[spectral][tiles]") {
    // The property that makes scrolling bearable: something correct but coarse
    // beats nothing at all.
    const AudioBuffer buffer = material(60000);
    auto tiles = SpectrogramTiles::create(sourceOf(buffer), 0, smallSettings());
    REQUIRE(tiles);
    REQUIRE(tiles.value().buildOverview());
    REQUIRE(tiles.value().residentTileCount() == 0);

    constexpr int kRows = 16;
    constexpr int kColumns = 32;
    std::vector<float> edges(kRows + 1);
    for (int row = 0; row <= kRows; ++row) {
        edges[static_cast<std::size_t>(row)] =
            static_cast<float>(row) * static_cast<float>(tiles.value().binCount()) / kRows;
    }
    std::vector<std::uint8_t> out(static_cast<std::size_t>(kRows * kColumns));
    tiles.value().render(0, buffer.frames(), edges.data(), kRows, kColumns, out.data());

    const bool anything = std::any_of(out.begin(), out.end(), [](std::uint8_t v) { return v > 0; });
    REQUIRE(anything);
}

TEST_CASE("Settings that cannot work are refused", "[spectral][tiles]") {
    const AudioBuffer buffer = material(10000);

    SpectrogramTiles::Settings settings = smallSettings();
    settings.tileFrames = 0;
    REQUIRE_FALSE(SpectrogramTiles::create(sourceOf(buffer), 0, settings));

    settings = smallSettings();
    settings.coarseLevel = 3;
    settings.tileFrames = 12; // Not a multiple of 8, so tiles straddle overview frames.
    REQUIRE_FALSE(SpectrogramTiles::create(sourceOf(buffer), 0, settings));

    settings = smallSettings();
    REQUIRE_FALSE(SpectrogramTiles::create(nullptr, 0, settings));
    REQUIRE_FALSE(SpectrogramTiles::create(sourceOf(buffer), 5, settings));
}

TEST_CASE("An empty source yields an empty cache rather than an error", "[spectral][tiles]") {
    const AudioBuffer empty{ChannelLayout::mono(), 0};
    auto tiles = SpectrogramTiles::create(sourceOf(empty), 0, smallSettings());
    REQUIRE(tiles);
    REQUIRE(tiles.value().buildOverview());
    REQUIRE(tiles.value().ensureDetail(0, 1000));
    REQUIRE(tiles.value().residentTileCount() == 0);
    REQUIRE(tiles.value().fineFrame(0) == nullptr);
}

TEST_CASE("Detail is not worth fetching once a column is wider than the overview's hop",
          "[spectral][tiles]") {
    const AudioBuffer buffer = material(60000);
    auto tiles = SpectrogramTiles::create(sourceOf(buffer), 0, smallSettings());
    REQUIRE(tiles);
    const SampleCount overviewHop =
        smallSettings().config.hopSize * (SampleCount{1} << smallSettings().coarseLevel);
    REQUIRE(tiles.value().detailWorthwhile(overviewHop - 1));
    REQUIRE_FALSE(tiles.value().detailWorthwhile(overviewHop));
    REQUIRE_FALSE(tiles.value().detailWorthwhile(overviewHop * 4));
}

TEST_CASE("Rendering while a worker builds and evicts is safe", "[tiles][threads]") {
    // The arrangement this class exists inside: a worker fetching detail for
    // wherever the view has scrolled to, and the view drawing from whatever is
    // resident. The budget here is small enough that the worker is evicting
    // constantly, which is the case that matters -- the first version of
    // render() took the lock per frame lookup rather than per render, so a tile
    // could be freed between being found and being read, which is a
    // use-after-free.
    //
    // Be clear about what this test is and is not. It is a regression guard: it
    // puts two threads on the same tiles so that a future change dropping the
    // lock has something to trip over under ThreadSanitizer. It is *not* a
    // demonstration that it catches that specific bug -- reintroducing the
    // hazard and watching TSan report it was attempted and abandoned, because
    // the unfixed version locks so many times that it did not finish in twenty
    // minutes under TSan, which is also why it cannot be the shape of the
    // shipped code. The fix rests on the argument rather than on that run:
    // render() holds the lock for its whole duration, so no tile pointer it
    // dereferences can be freed while it holds one.
    const AudioBuffer buffer = material(300000);
    SpectrogramTiles::Settings settings = smallSettings();
    auto probe = SpectrogramTiles::create(sourceOf(buffer), 0, settings);
    REQUIRE(probe);
    settings.detailBudgetBytes = static_cast<std::size_t>(settings.tileFrames) *
                                 static_cast<std::size_t>(probe.value().binCount()) * 3;

    auto tiles = SpectrogramTiles::create(sourceOf(buffer), 0, settings);
    REQUIRE(tiles);
    REQUIRE(tiles.value().buildOverview());

    constexpr int kRows = 16;
    constexpr int kColumns = 24;
    std::vector<float> edges(kRows + 1);
    for (int row = 0; row <= kRows; ++row) {
        edges[static_cast<std::size_t>(row)] =
            static_cast<float>(row) * static_cast<float>(tiles.value().binCount()) / kRows;
    }

    std::atomic<bool> stop{false};
    std::thread worker{[&] {
        for (int pass = 0; pass < 6 && !stop.load(); ++pass) {
            for (SampleIndex at = 0; at < buffer.frames() && !stop.load(); at += 20000) {
                (void)tiles.value().ensureDetail(at, at + 20000);
            }
        }
        stop.store(true);
    }};

    std::vector<std::uint8_t> out(static_cast<std::size_t>(kRows * kColumns));
    int renders = 0;
    while (!stop.load() && renders < 4000) {
        const SampleIndex from = (renders % 10) * 20000;
        tiles.value().render(from, from + 8000, edges.data(), kRows, kColumns, out.data());
        ++renders;
    }
    stop.store(true);
    worker.join();

    REQUIRE(renders > 0);
    // Nothing to assert about the content -- which tiles were resident at any
    // instant is genuinely nondeterministic. What is being asserted is that
    // none of it crashed and none of it raced, and the second of those is
    // ThreadSanitizer's to say.
    tiles.value().render(0, buffer.frames(), edges.data(), kRows, kColumns, out.data());
    REQUIRE(std::any_of(out.begin(), out.end(), [](std::uint8_t v) { return v > 0; }));
}
