#include "TileHashing.h"

#include <sa/io/AudioFile.h>
#include <sa/io/WavWriter.h>
#include <sa/spectral/SpectrogramTileStore.h>
#include <sa/spectral/SpectrogramTiles.h>

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace sa;
using namespace sa::spectral;

namespace {

/// A directory that deletes itself, so a failing assertion does not leave a
/// gigabyte of tiles in the system temporary directory.
class TemporaryDirectory {
public:
    explicit TemporaryDirectory(std::string_view label) {
        static std::atomic<std::uint64_t> counter{0};
        const auto now =
            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path() /
                ("sa-tile-store-" + std::string{label} + "-" + std::to_string(now) + "-" +
                 std::to_string(counter.fetch_add(1)));
        std::error_code code;
        std::filesystem::create_directories(path_, code);
    }

    ~TemporaryDirectory() {
        // Permissions first, and everywhere: a test that made a directory
        // read-only and then failed an assertion would otherwise leave one
        // behind that this cannot delete.
        std::error_code code;
        std::filesystem::permissions(path_, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, code);
        for (const auto& entry : std::filesystem::recursive_directory_iterator{path_, code}) {
            std::error_code ignored;
            std::filesystem::permissions(entry.path(), std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::add, ignored);
        }
        std::filesystem::remove_all(path_, code);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

/// An AudioSource over a buffer that counts its reads, so a test can assert
/// that a cached open touched no audio at all rather than merely being quick.
class CountingSource final : public io::AudioSource {
public:
    explicit CountingSource(AudioBuffer audio) : audio_(std::move(audio)) {
        info_.sampleRate = SampleRate{48000.0};
        info_.layout = audio_.layout();
        info_.frameCount = audio_.frames();
        info_.format = io::SampleFormat::Float32;
    }

    [[nodiscard]] const io::AudioFileInfo& info() const noexcept override { return info_; }

    [[nodiscard]] Result<SampleCount> read(SampleIndex startFrame,
                                           AudioBufferView destination) const override {
        reads_.fetch_add(1, std::memory_order_relaxed);
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

    [[nodiscard]] int reads() const noexcept { return reads_.load(std::memory_order_relaxed); }

    void forgetReads() const noexcept { reads_.store(0, std::memory_order_relaxed); }

private:
    AudioBuffer audio_;
    io::AudioFileInfo info_;
    mutable std::atomic<int> reads_{0};
};

/// A sweep so that every frame differs from its neighbours, plus isolated
/// clicks. The same material the in-memory tests use, for the same reason.
[[nodiscard]] AudioBuffer material(SampleCount frames, int channels = 1, unsigned seed = 4) {
    std::mt19937 engine{seed};
    std::normal_distribution<float> hiss{0.0f, 0.02f};
    AudioBuffer buffer{ChannelLayout::discrete(channels), frames};
    for (int channel = 0; channel < channels; ++channel) {
        double phase = 0.0;
        for (SampleCount i = 0; i < frames; ++i) {
            const double t =
                static_cast<double>(i) / static_cast<double>(std::max<SampleCount>(1, frames));
            const double hz = (100.0 + 70.0 * channel) * std::exp(t * std::log(180.0));
            phase += 2.0 * std::numbers::pi * hz / 48000.0;
            buffer.channel(channel)[i] = static_cast<float>(0.5 * std::sin(phase)) + hiss(engine);
        }
        for (SampleCount i = 3000; i < frames; i += 6000) {
            buffer.channel(channel)[i] = 0.95f;
        }
    }
    return buffer;
}

[[nodiscard]] AudioBuffer clone(const AudioBuffer& audio) {
    AudioBuffer copy{audio.layout(), audio.frames()};
    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        std::copy_n(audio.channel(channel), audio.frames(), copy.channel(channel));
    }
    return copy;
}

[[nodiscard]] std::shared_ptr<const CountingSource> sourceOf(const AudioBuffer& audio) {
    return std::make_shared<const CountingSource>(clone(audio));
}

[[nodiscard]] SpectrogramTiles::Settings smallSettings() {
    SpectrogramTiles::Settings settings;
    settings.coarseLevel = 3;
    settings.tileFrames = 32;
    settings.detailBudgetBytes = std::size_t{1} << 30;
    return settings;
}

[[nodiscard]] TileLayout smallLayout(int channel = 0) {
    return SpectrogramTiles::layoutFor(smallSettings(), channel);
}

/// A key that is not null and not derived from anything, for the store tests,
/// which are about the store rather than about what keys mean.
[[nodiscard]] ContentKey madeUpKey(std::uint8_t seed) {
    ContentKey key;
    for (std::size_t i = 0; i < key.bytes.size(); ++i) {
        key.bytes[i] = static_cast<std::uint8_t>(seed + i + 1);
    }
    return key;
}

[[nodiscard]] StoredTile madeUpTile(SampleCount frames, int bins, unsigned seed) {
    StoredTile tile;
    tile.firstFrame = 64;
    tile.frameCount = frames;
    tile.hop = 512;
    tile.sourceFrames = 480000;
    tile.binCount = bins;
    tile.magnitudes.resize(static_cast<std::size_t>(frames) * static_cast<std::size_t>(bins));
    std::mt19937 engine{seed};
    for (std::uint8_t& byte : tile.magnitudes) {
        byte = static_cast<std::uint8_t>(engine() & 0xFFu);
    }
    return tile;
}

[[nodiscard]] std::vector<std::filesystem::path>
entryFiles(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> found;
    std::error_code code;
    for (const auto& entry : std::filesystem::directory_iterator{directory, code}) {
        if (entry.path().extension() == ".satile") {
            found.push_back(entry.path());
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

[[nodiscard]] std::vector<std::uint8_t> readWhole(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    return std::vector<std::uint8_t>{std::istreambuf_iterator<char>{stream},
                                     std::istreambuf_iterator<char>{}};
}

void writeWhole(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    if (!bytes.empty()) {
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
}

[[nodiscard]] std::string hexOf(const std::array<std::uint8_t, 32>& digest) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t byte : digest) {
        out.push_back(kDigits[static_cast<std::size_t>(byte >> 4)]);
        out.push_back(kDigits[static_cast<std::size_t>(byte & 0x0Fu)]);
    }
    return out;
}

/// True if writing into `directory` still works. Needed because a test process
/// running as root ignores the permission bits it just set, and an assertion
/// that silently tests nothing is worse than one that says so.
[[nodiscard]] bool stillWritable(const std::filesystem::path& directory) {
    const std::filesystem::path probe = directory / "probe.tmp";
    std::ofstream stream{probe, std::ios::binary | std::ios::trunc};
    const bool opened = stream.is_open();
    stream.close();
    std::error_code code;
    std::filesystem::remove(probe, code);
    return opened;
}

} // namespace

TEST_CASE("The digest is SHA-256 and the check is CRC-32", "[spectral][tilestore]") {
    // Published vectors, because a hash that is stable and wrong is still a
    // usable cache key -- so nothing else in this file would notice, and the
    // documentation claims SHA-256 rather than "a hash of ours".
    detail::Sha256 empty;
    REQUIRE(hexOf(empty.finish()) ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    detail::Sha256 abc;
    abc.update("abc", 3);
    REQUIRE(hexOf(abc.finish()) ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // Longer than one 64-byte block, so the message schedule and the padding of
    // a multi-block message are covered rather than only the one-block case.
    detail::Sha256 twoBlocks;
    twoBlocks.update("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56);
    REQUIRE(hexOf(twoBlocks.finish()) ==
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    const std::string check = "123456789";
    REQUIRE(detail::crc32(reinterpret_cast<const std::uint8_t*>(check.data()), check.size()) ==
            0xCBF43926u);
}

TEST_CASE("A tile written and read back is byte for byte what went in", "[spectral][tilestore]") {
    const TemporaryDirectory directory{"roundtrip"};
    auto store = SpectrogramTileStore::open({directory.path(), std::uint64_t{16} << 20});
    REQUIRE(store);

    const ContentKey key = madeUpKey(7);
    const TileLayout layout = smallLayout();
    const StoredTile written = madeUpTile(200, 129, 11);

    REQUIRE(store.value()->write(key, kOverviewEntry, layout, written));

    const Result<StoredTile> read = store.value()->read(key, kOverviewEntry, layout);
    REQUIRE(read);
    const StoredTile& got = read.value();
    REQUIRE(got.firstFrame == written.firstFrame);
    REQUIRE(got.frameCount == written.frameCount);
    REQUIRE(got.hop == written.hop);
    REQUIRE(got.sourceFrames == written.sourceFrames);
    REQUIRE(got.binCount == written.binCount);
    REQUIRE(got.magnitudes.size() == written.magnitudes.size());
    REQUIRE(got.magnitudes == written.magnitudes);
}

TEST_CASE("A store reopened over the same directory finds what the last one wrote",
          "[spectral][tilestore]") {
    const TemporaryDirectory directory{"reopen"};
    const ContentKey key = madeUpKey(3);
    const TileLayout layout = smallLayout();
    const StoredTile written = madeUpTile(64, 65, 5);

    {
        auto store = SpectrogramTileStore::open({directory.path(), std::uint64_t{16} << 20});
        REQUIRE(store);
        REQUIRE(store.value()->write(key, tileEntry(9), layout, written));
    }

    auto reopened = SpectrogramTileStore::open({directory.path(), std::uint64_t{16} << 20});
    REQUIRE(reopened);
    REQUIRE(reopened.value()->entryCount() == 1);
    REQUIRE(reopened.value()->residentBytes() ==
            SpectrogramTileStore::entrySizeFor(written.magnitudes.size()));

    const Result<StoredTile> read = reopened.value()->read(key, tileEntry(9), layout);
    REQUIRE(read);
    REQUIRE(read.value().magnitudes == written.magnitudes);
}

TEST_CASE("A spectrogram from the store is identical to one built from scratch",
          "[spectral][tilestore]") {
    // The property the whole thing rests on. A cache that is nearly right is a
    // cache that draws a different picture depending on whether the user has
    // opened the file before, which is worse than no cache at all.
    const TemporaryDirectory directory{"identical"};
    const AudioBuffer buffer = material(200000);
    const auto source = sourceOf(buffer);

    SpectrogramTiles::Settings settings = smallSettings();
    const TileLayout layout = SpectrogramTiles::layoutFor(settings, 0);
    const Result<ContentKey> key = contentKeyForSource(*source, layout);
    REQUIRE(key);

    auto store = SpectrogramTileStore::open({directory.path(), std::uint64_t{64} << 20});
    REQUIRE(store);
    settings.store = store.value();
    settings.key = key.value();

    auto fresh = SpectrogramTiles::create(source, 0, settings);
    REQUIRE(fresh);
    REQUIRE(fresh.value().buildOverview());
    REQUIRE_FALSE(fresh.value().overviewCameFromStore());
    REQUIRE(fresh.value().ensureDetail(0, buffer.frames()));
    REQUIRE(store.value()->entryCount() > 1);

    // A second store object over the same directory, because the point is that
    // this survives the process rather than that one object remembers.
    auto reopened = SpectrogramTileStore::open({directory.path(), std::uint64_t{64} << 20});
    REQUIRE(reopened);
    settings.store = reopened.value();

    auto cached = SpectrogramTiles::create(source, 0, settings);
    REQUIRE(cached);
    source->forgetReads();
    REQUIRE(cached.value().buildOverview());
    REQUIRE(cached.value().overviewCameFromStore());
    REQUIRE(cached.value().ensureDetail(0, buffer.frames()));
    // Not "it was quick": it did not touch the audio at all.
    REQUIRE(source->reads() == 0);

    const SpectrogramPyramid& want = fresh.value().overview();
    const SpectrogramPyramid& got = cached.value().overview();
    REQUIRE(got.levelCount() == want.levelCount());
    REQUIRE(got.binCount() == want.binCount());
    REQUIRE(got.sourceFrames() == want.sourceFrames());
    for (int level = 0; level < want.levelCount(); ++level) {
        REQUIRE(got.hopAt(level) == want.hopAt(level));
        REQUIRE(got.frameCountAt(level) == want.frameCountAt(level));
        for (SampleCount frame = 0; frame < want.frameCountAt(level); ++frame) {
            const std::uint8_t* a = want.frameData(level, frame);
            const std::uint8_t* b = got.frameData(level, frame);
            REQUIRE(a != nullptr);
            REQUIRE(b != nullptr);
            REQUIRE(std::equal(a, a + want.binCount(), b));
        }
    }

    REQUIRE(cached.value().fineFrameCount() == fresh.value().fineFrameCount());
    for (SampleCount frame = 0; frame < fresh.value().fineFrameCount(); ++frame) {
        const std::uint8_t* a = fresh.value().fineFrame(frame);
        const std::uint8_t* b = cached.value().fineFrame(frame);
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        REQUIRE(std::equal(a, a + fresh.value().binCount(), b));
    }

    constexpr int kRows = 24;
    constexpr int kColumns = 40;
    std::vector<float> edges(kRows + 1);
    for (int row = 0; row <= kRows; ++row) {
        edges[static_cast<std::size_t>(row)] =
            static_cast<float>(row) * static_cast<float>(fresh.value().binCount()) / kRows;
    }
    std::vector<std::uint8_t> fromFresh(static_cast<std::size_t>(kRows * kColumns));
    std::vector<std::uint8_t> fromCached(static_cast<std::size_t>(kRows * kColumns));
    fresh.value().render(10000, 14000, edges.data(), kRows, kColumns, fromFresh.data());
    cached.value().render(10000, 14000, edges.data(), kRows, kColumns, fromCached.data());
    REQUIRE(fromFresh == fromCached);
}

TEST_CASE("A spectrogram from a file on disk survives being reopened", "[spectral][tilestore]") {
    // The same property again, through the key that a real open would use: one
    // derived from the file rather than from the decoded samples.
    const TemporaryDirectory directory{"fromfile"};
    const std::filesystem::path wav = directory.path() / "material.wav";
    const AudioBuffer buffer = material(200000);
    REQUIRE(io::WavWriter::writeFile(wav, buffer.constView(), SampleRate{48000.0},
                                     ChannelLayout::mono()));

    const auto opened = io::openAudioFile(wav);
    REQUIRE(opened);

    SpectrogramTiles::Settings settings = smallSettings();
    const TileLayout layout = SpectrogramTiles::layoutFor(settings, 0);
    const Result<ContentKey> key = contentKeyForFile(wav, layout);
    REQUIRE(key);
    REQUIRE_FALSE(key.value().isNull());
    // Keying the same file twice is the same key, or nothing would ever hit.
    const Result<ContentKey> again = contentKeyForFile(wav, layout);
    REQUIRE(again);
    REQUIRE(again.value() == key.value());

    const std::filesystem::path cache = directory.path() / "cache";
    std::vector<std::uint8_t> firstPass;
    {
        auto store = SpectrogramTileStore::open({cache, std::uint64_t{64} << 20});
        REQUIRE(store);
        settings.store = store.value();
        settings.key = key.value();
        auto tiles = SpectrogramTiles::create(opened.value(), 0, settings);
        REQUIRE(tiles);
        REQUIRE(tiles.value().buildOverview());
        REQUIRE(tiles.value().ensureDetail(0, buffer.frames()));
        const SpectrogramPyramid& overview = tiles.value().overview();
        for (SampleCount frame = 0; frame < overview.frameCountAt(0); ++frame) {
            const std::uint8_t* data = overview.frameData(0, frame);
            firstPass.insert(firstPass.end(), data, data + overview.binCount());
        }
    }

    auto store = SpectrogramTileStore::open({cache, std::uint64_t{64} << 20});
    REQUIRE(store);
    settings.store = store.value();
    auto tiles = SpectrogramTiles::create(opened.value(), 0, settings);
    REQUIRE(tiles);
    REQUIRE(tiles.value().buildOverview());
    REQUIRE(tiles.value().overviewCameFromStore());

    std::vector<std::uint8_t> secondPass;
    const SpectrogramPyramid& overview = tiles.value().overview();
    for (SampleCount frame = 0; frame < overview.frameCountAt(0); ++frame) {
        const std::uint8_t* data = overview.frameData(0, frame);
        secondPass.insert(secondPass.end(), data, data + overview.binCount());
    }
    REQUIRE(secondPass == firstPass);
}

TEST_CASE("Changing any one setting misses rather than returning the wrong tiles",
          "[spectral][tilestore]") {
    // One at a time, because a key that happens to cover four of the five is a
    // key that is wrong in exactly one way, and a test that changed all of them
    // together would pass.
    const AudioBuffer buffer = material(40000, 2);
    const auto source = sourceOf(buffer);
    const TileLayout base = smallLayout(0);
    const Result<ContentKey> baseKey = contentKeyForSource(*source, base);
    REQUIRE(baseKey);

    const auto differs = [&](const TileLayout& changed) {
        const Result<ContentKey> key = contentKeyForSource(*source, changed);
        REQUIRE(key);
        REQUIRE_FALSE(key.value() == baseKey.value());
        return key.value();
    };

    TileLayout changed = base;
    changed.config.fftSize = 1024;
    const ContentKey byFftSize = differs(changed);

    changed = base;
    changed.config.hopSize = 256;
    const ContentKey byHopSize = differs(changed);

    changed = base;
    changed.config.window = dsp::WindowType::BlackmanHarris;
    const ContentKey byWindow = differs(changed);

    changed = base;
    changed.coarseLevel = 4;
    const ContentKey byLevel = differs(changed);

    changed = base;
    changed.tileFrames = 64;
    const ContentKey byTileFrames = differs(changed);

    changed = base;
    changed.channel = 1;
    const ContentKey byChannel = differs(changed);

    changed = base;
    changed.config.minimumDecibels = -90.0f;
    const ContentKey byFloor = differs(changed);

    changed = base;
    changed.config.maximumDecibels = 6.0f;
    const ContentKey byCeiling = differs(changed);

    // And a different key really does miss in the store, rather than the keys
    // merely being different somewhere no lookup looks.
    const TemporaryDirectory directory{"settings"};
    auto store = SpectrogramTileStore::open({directory.path(), std::uint64_t{16} << 20});
    REQUIRE(store);
    const StoredTile tile = madeUpTile(32, 33, 2);
    REQUIRE(store.value()->write(baseKey.value(), kOverviewEntry, base, tile));
    REQUIRE(store.value()->read(baseKey.value(), kOverviewEntry, base));

    for (const ContentKey& other :
         {byFftSize, byHopSize, byWindow, byLevel, byTileFrames, byChannel, byFloor, byCeiling}) {
        const Result<StoredTile> read = store.value()->read(other, kOverviewEntry, base);
        REQUIRE_FALSE(read);
        REQUIRE(read.error().code() == ErrorCode::NotFound);
    }
}

TEST_CASE("An entry is refused when its recorded settings do not match the request",
          "[spectral][tilestore]") {
    // The key already covers the settings, so this can only happen to a file
    // that is not what its name says it is. That is exactly the case worth
    // refusing: the alternative is trusting a file because of where it was
    // found rather than because of what is in it.
    const TemporaryDirectory directory{"mismatch"};
    auto store = SpectrogramTileStore::open({directory.path(), std::uint64_t{16} << 20});
    REQUIRE(store);

    const ContentKey key = madeUpKey(31);
    const TileLayout written = smallLayout();
    REQUIRE(store.value()->write(key, kOverviewEntry, written, madeUpTile(16, 17, 3)));

    TileLayout asked = written;
    asked.config.fftSize = 512;
    const Result<StoredTile> read = store.value()->read(key, kOverviewEntry, asked);
    REQUIRE_FALSE(read);
    REQUIRE(read.error().code() == ErrorCode::NotFound);

    // And asking for another entry under the right layout misses rather than
    // being served the entry that is there.
    REQUIRE_FALSE(store.value()->read(key, tileEntry(0), written));
}

TEST_CASE("Different audio content misses", "[spectral][tilestore]") {
    const TileLayout layout = smallLayout();

    const AudioBuffer one = material(40000, 1, 4);
    const AudioBuffer other = material(40000, 1, 9);
    const Result<ContentKey> first = contentKeyForSource(*sourceOf(one), layout);
    const Result<ContentKey> second = contentKeyForSource(*sourceOf(other), layout);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE_FALSE(first.value() == second.value());

    // The same samples through a different source object is the same key, which
    // is the half of this that makes a cache work at all.
    const Result<ContentKey> repeat = contentKeyForSource(*sourceOf(one), layout);
    REQUIRE(repeat);
    REQUIRE(repeat.value() == first.value());

    // And on disk: two files of the same length and the same timestamp, so that
    // it is the contents being noticed rather than the metadata.
    const TemporaryDirectory directory{"content"};
    const std::filesystem::path left = directory.path() / "left.wav";
    const std::filesystem::path right = directory.path() / "right.wav";
    REQUIRE(io::WavWriter::writeFile(left, one.constView(), SampleRate{48000.0},
                                     ChannelLayout::mono()));
    REQUIRE(io::WavWriter::writeFile(right, other.constView(), SampleRate{48000.0},
                                     ChannelLayout::mono()));
    std::error_code code;
    const auto stamp = std::filesystem::last_write_time(left, code);
    REQUIRE_FALSE(code);
    std::filesystem::last_write_time(right, stamp, code);
    REQUIRE_FALSE(code);
    REQUIRE(std::filesystem::file_size(left) == std::filesystem::file_size(right));

    const Result<ContentKey> leftKey = contentKeyForFile(left, layout);
    const Result<ContentKey> rightKey = contentKeyForFile(right, layout);
    REQUIRE(leftKey);
    REQUIRE(rightKey);
    REQUIRE_FALSE(leftKey.value() == rightKey.value());
}

TEST_CASE("A sampled hash notices a change it samples and misses one it does not",
          "[spectral][tilestore]") {
    // The documented limitation, asserted rather than described, so that nobody
    // later mistakes this for a hash of the file. A change inside a sampled
    // block is a different key; the same-sized change between two blocks, with
    // the timestamp put back, is not.
    const TemporaryDirectory directory{"sampling"};
    const std::filesystem::path path = directory.path() / "big.bin";
    std::vector<std::uint8_t> bytes(1000000, 0x5A);
    writeWhole(path, bytes);

    const TileLayout layout = smallLayout();
    std::error_code code;
    const auto stamp = std::filesystem::last_write_time(path, code);
    REQUIRE_FALSE(code);
    const Result<ContentKey> original = contentKeyForFile(path, layout);
    REQUIRE(original);

    // Inside the header, which is always hashed.
    bytes[100] = 0x01;
    writeWhole(path, bytes);
    std::filesystem::last_write_time(path, stamp, code);
    const Result<ContentKey> headerChanged = contentKeyForFile(path, layout);
    REQUIRE(headerChanged);
    REQUIRE_FALSE(headerChanged.value() == original.value());
    bytes[100] = 0x5A;

    // Inside the first sampled block, which begins at 4096.
    bytes[5000] = 0x02;
    writeWhole(path, bytes);
    std::filesystem::last_write_time(path, stamp, code);
    const Result<ContentKey> blockChanged = contentKeyForFile(path, layout);
    REQUIRE(blockChanged);
    REQUIRE_FALSE(blockChanged.value() == original.value());
    bytes[5000] = 0x5A;

    // Between the first block, which ends at 8192, and the second, which begins
    // a great deal later. Nothing hashes this byte, so the key is unchanged --
    // and a cache keyed on it would serve the old tiles for the new contents.
    bytes[9000] = 0x03;
    writeWhole(path, bytes);
    std::filesystem::last_write_time(path, stamp, code);
    const Result<ContentKey> unsampled = contentKeyForFile(path, layout);
    REQUIRE(unsampled);
    REQUIRE(unsampled.value() == original.value());

    // Which is why the timestamp is in the key: the same edit without putting
    // the timestamp back is a miss, and a miss is only a rebuild.
    writeWhole(path, bytes);
    const Result<ContentKey> restamped = contentKeyForFile(path, layout);
    REQUIRE(restamped);
    REQUIRE_FALSE(restamped.value() == original.value());
}

TEST_CASE("A truncated entry is detected rather than read off the end", "[spectral][tilestore]") {
    // Every length a crash or a full disk could leave behind, including the
    // ones either side of the header boundary, where a reader that trusted its
    // own arithmetic would index past the buffer it had.
    const TemporaryDirectory directory{"truncated"};
    auto store = SpectrogramTileStore::open({directory.path(), std::uint64_t{16} << 20});
    REQUIRE(store);
    const ContentKey key = madeUpKey(19);
    const TileLayout layout = smallLayout();
    const StoredTile tile = madeUpTile(40, 33, 17);

    REQUIRE(store.value()->write(key, kOverviewEntry, layout, tile));
    const auto files = entryFiles(directory.path());
    REQUIRE(files.size() == 1);
    const std::vector<std::uint8_t> whole = readWhole(files.front());
    REQUIRE(whole.size() == SpectrogramTileStore::entrySizeFor(tile.magnitudes.size()));

    for (const std::size_t length :
         {std::size_t{0}, std::size_t{1}, std::size_t{16}, std::size_t{143}, std::size_t{144},
          std::size_t{145}, whole.size() / 2, whole.size() - 1}) {
        INFO("truncated to " << length);
        REQUIRE(store.value()->write(key, kOverviewEntry, layout, tile));
        writeWhole(files.front(),
                   std::vector<std::uint8_t>{whole.begin(),
                                             whole.begin() + static_cast<std::ptrdiff_t>(length)});
        const Result<StoredTile> read = store.value()->read(key, kOverviewEntry, layout);
        REQUIRE_FALSE(read);
        REQUIRE(read.error().code() == ErrorCode::CorruptData);
        // And the useless file is gone rather than being paid for again.
        REQUIRE(entryFiles(directory.path()).empty());
        REQUIRE(store.value()->entryCount() == 0);
        REQUIRE(store.value()->residentBytes() == 0);
    }
}

TEST_CASE("A flipped bit anywhere in an entry is detected", "[spectral][tilestore]") {
    const TemporaryDirectory directory{"flipped"};
    auto store = SpectrogramTileStore::open({directory.path(), std::uint64_t{16} << 20});
    REQUIRE(store);
    const ContentKey key = madeUpKey(23);
    const TileLayout layout = smallLayout();
    const StoredTile tile = madeUpTile(40, 33, 29);

    REQUIRE(store.value()->write(key, kOverviewEntry, layout, tile));
    const auto files = entryFiles(directory.path());
    REQUIRE(files.size() == 1);
    const std::filesystem::path path = files.front();
    const std::vector<std::uint8_t> whole = readWhole(path);

    // Every byte of the header, one at a time -- the magic, the version, the
    // key, the layout, the shape, the lengths and both checksums -- and a
    // scattering through the payload.
    std::vector<std::size_t> offsets;
    for (std::size_t at = 0; at < 144; ++at) {
        offsets.push_back(at);
    }
    for (std::size_t at = 144; at < whole.size(); at += 97) {
        offsets.push_back(at);
    }

    for (const std::size_t at : offsets) {
        INFO("flipped byte " << at);
        REQUIRE(store.value()->write(key, kOverviewEntry, layout, tile));
        std::vector<std::uint8_t> damaged = whole;
        damaged[at] = static_cast<std::uint8_t>(damaged[at] ^ 0xFFu);
        writeWhole(path, damaged);
        REQUIRE_FALSE(store.value()->read(key, kOverviewEntry, layout));
    }
}

TEST_CASE("A zeroed, garbled or overlong entry is detected", "[spectral][tilestore]") {
    const TemporaryDirectory directory{"garbage"};
    auto store = SpectrogramTileStore::open({directory.path(), std::uint64_t{16} << 20});
    REQUIRE(store);
    const ContentKey key = madeUpKey(37);
    const TileLayout layout = smallLayout();
    const StoredTile tile = madeUpTile(40, 33, 41);

    const auto rewrite = [&]() {
        REQUIRE(store.value()->write(key, kOverviewEntry, layout, tile));
        const auto files = entryFiles(directory.path());
        REQUIRE(files.size() == 1);
        return files.front();
    };

    SECTION("zeroed") {
        const std::filesystem::path path = rewrite();
        const std::vector<std::uint8_t> whole = readWhole(path);
        writeWhole(path, std::vector<std::uint8_t>(whole.size(), 0));
        const Result<StoredTile> read = store.value()->read(key, kOverviewEntry, layout);
        REQUIRE_FALSE(read);
        REQUIRE(read.error().code() == ErrorCode::CorruptData);
    }

    SECTION("random") {
        const std::filesystem::path path = rewrite();
        std::vector<std::uint8_t> whole = readWhole(path);
        std::mt19937 engine{7};
        for (std::uint8_t& byte : whole) {
            byte = static_cast<std::uint8_t>(engine() & 0xFFu);
        }
        writeWhole(path, whole);
        REQUIRE_FALSE(store.value()->read(key, kOverviewEntry, layout));
    }

    SECTION("claiming a payload far larger than the file") {
        // The case that would be a read off the end of an allocation if the
        // length in the file were believed rather than checked against it. The
        // header checksum is recomputed afterwards, so that what refuses this
        // is the length check rather than the checksum happening to catch it
        // first -- this is a *consistent* file that lies about its size, which
        // is what a half-written one looks like.
        const std::filesystem::path path = rewrite();
        std::vector<std::uint8_t> whole = readWhole(path);
        const auto patch64 = [&whole](std::size_t at, std::uint64_t value) {
            for (std::size_t i = 0; i < 8; ++i) {
                whole[at + i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
            }
        };
        const auto reseal = [&whole]() {
            const std::uint32_t crc = detail::crc32(whole.data(), 140);
            for (std::size_t i = 0; i < 4; ++i) {
                whole[140 + i] = static_cast<std::uint8_t>((crc >> (8 * i)) & 0xFFu);
            }
        };

        // A shape and a length that agree with each other and with nothing on
        // disk: 8 million frames of 33 bins, a quarter of a gigabyte.
        patch64(104, 8000000);         // frameCount
        patch64(128, 8000000ull * 33); // payloadBytes
        reseal();
        writeWhole(path, whole);
        Result<StoredTile> read = store.value()->read(key, kOverviewEntry, layout);
        REQUIRE_FALSE(read);
        REQUIRE(read.error().code() == ErrorCode::CorruptData);

        // And one beyond anything this format stores, which is refused before
        // the arithmetic rather than after it.
        const std::filesystem::path again = rewrite();
        whole = readWhole(again);
        patch64(104, 0x0FFFFFFFFFFFFFFFll);
        patch64(128, 0xFFFFFFFFFFFFFFFFull);
        reseal();
        writeWhole(again, whole);
        read = store.value()->read(key, kOverviewEntry, layout);
        REQUIRE_FALSE(read);
        REQUIRE(read.error().code() == ErrorCode::CorruptData);
    }

    SECTION("with bytes appended after a valid entry") {
        const std::filesystem::path path = rewrite();
        std::vector<std::uint8_t> whole = readWhole(path);
        whole.insert(whole.end(), 512, 0x7E);
        writeWhole(path, whole);
        REQUIRE_FALSE(store.value()->read(key, kOverviewEntry, layout));
    }

    SECTION("of an older format version") {
        const std::filesystem::path path = rewrite();
        std::vector<std::uint8_t> whole = readWhole(path);
        // The version, and then the header checksum over it, so that this is a
        // well-formed file of another version rather than a damaged one.
        whole[8] = 0x7F;
        const std::uint32_t crc = detail::crc32(whole.data(), 140);
        for (std::size_t i = 0; i < 4; ++i) {
            whole[140 + i] = static_cast<std::uint8_t>((crc >> (8 * i)) & 0xFFu);
        }
        writeWhole(path, whole);
        const Result<StoredTile> read = store.value()->read(key, kOverviewEntry, layout);
        REQUIRE_FALSE(read);
        REQUIRE(read.error().code() == ErrorCode::CorruptData);
    }

    SECTION("that has vanished") {
        const std::filesystem::path path = rewrite();
        std::error_code code;
        std::filesystem::remove(path, code);
        const Result<StoredTile> read = store.value()->read(key, kOverviewEntry, layout);
        REQUIRE_FALSE(read);
        REQUIRE(read.error().code() == ErrorCode::NotFound);
        REQUIRE(store.value()->entryCount() == 0);
    }
}

TEST_CASE("A corrupt overview costs a rebuild and nothing else", "[spectral][tilestore]") {
    const TemporaryDirectory directory{"corruptoverview"};
    const AudioBuffer buffer = material(60000);
    const auto source = sourceOf(buffer);

    SpectrogramTiles::Settings settings = smallSettings();
    const Result<ContentKey> key =
        contentKeyForSource(*source, SpectrogramTiles::layoutFor(settings, 0));
    REQUIRE(key);
    auto store = SpectrogramTileStore::open({directory.path(), std::uint64_t{32} << 20});
    REQUIRE(store);
    settings.store = store.value();
    settings.key = key.value();

    auto fresh = SpectrogramTiles::create(source, 0, settings);
    REQUIRE(fresh);
    REQUIRE(fresh.value().buildOverview());

    for (const std::filesystem::path& path : entryFiles(directory.path())) {
        std::vector<std::uint8_t> whole = readWhole(path);
        whole[whole.size() / 2] = static_cast<std::uint8_t>(whole[whole.size() / 2] ^ 0xFFu);
        writeWhole(path, whole);
    }

    auto after = SpectrogramTiles::create(source, 0, settings);
    REQUIRE(after);
    REQUIRE(after.value().buildOverview());
    REQUIRE_FALSE(after.value().overviewCameFromStore());

    const SpectrogramPyramid& want = fresh.value().overview();
    const SpectrogramPyramid& got = after.value().overview();
    REQUIRE(got.levelCount() == want.levelCount());
    for (SampleCount frame = 0; frame < want.frameCountAt(0); ++frame) {
        const std::uint8_t* a = want.frameData(0, frame);
        const std::uint8_t* b = got.frameData(0, frame);
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        REQUIRE(std::equal(a, a + want.binCount(), b));
    }
}

TEST_CASE("The store stays inside its budget, evicting the least recently used",
          "[spectral][tilestore]") {
    const TemporaryDirectory directory{"budget"};
    const StoredTile tile = madeUpTile(32, 65, 13);
    const std::uint64_t each = SpectrogramTileStore::entrySizeFor(tile.magnitudes.size());
    auto store = SpectrogramTileStore::open({directory.path(), each * 4});
    REQUIRE(store);

    const ContentKey key = madeUpKey(59);
    const TileLayout layout = smallLayout();
    for (SampleCount index = 0; index < 4; ++index) {
        REQUIRE(store.value()->write(key, tileEntry(index), layout, tile));
    }
    REQUIRE(store.value()->entryCount() == 4);
    REQUIRE(store.value()->residentBytes() == each * 4);

    // Use the two oldest, which makes the two after them the oldest instead.
    REQUIRE(store.value()->read(key, tileEntry(0), layout));
    REQUIRE(store.value()->read(key, tileEntry(1), layout));

    REQUIRE(store.value()->write(key, tileEntry(4), layout, tile));
    REQUIRE(store.value()->write(key, tileEntry(5), layout, tile));

    REQUIRE(store.value()->residentBytes() <= store.value()->budgetBytes());
    REQUIRE(store.value()->entryCount() == 4);
    REQUIRE(entryFiles(directory.path()).size() == 4);
    REQUIRE(store.value()->read(key, tileEntry(0), layout));
    REQUIRE(store.value()->read(key, tileEntry(1), layout));
    REQUIRE(store.value()->read(key, tileEntry(4), layout));
    REQUIRE(store.value()->read(key, tileEntry(5), layout));
    REQUIRE_FALSE(store.value()->read(key, tileEntry(2), layout));
    REQUIRE_FALSE(store.value()->read(key, tileEntry(3), layout));

    store.value()->clear();
    REQUIRE(store.value()->entryCount() == 0);
    REQUIRE(store.value()->residentBytes() == 0);
    REQUIRE(entryFiles(directory.path()).empty());
}

TEST_CASE("A store opened with a smaller budget than it holds evicts down to it",
          "[spectral][tilestore]") {
    // A budget that only applied to writes would leave a store that is only
    // read from permanently over its bound, which is the shape of the bug
    // report this class exists to avoid.
    const TemporaryDirectory directory{"shrunk"};
    const StoredTile tile = madeUpTile(32, 65, 151);
    const std::uint64_t each = SpectrogramTileStore::entrySizeFor(tile.magnitudes.size());
    const ContentKey key = madeUpKey(157);
    const TileLayout layout = smallLayout();

    {
        auto store = SpectrogramTileStore::open({directory.path(), each * 6});
        REQUIRE(store);
        for (SampleCount index = 0; index < 6; ++index) {
            REQUIRE(store.value()->write(key, tileEntry(index), layout, tile));
        }
        REQUIRE(store.value()->entryCount() == 6);
    }

    auto smaller = SpectrogramTileStore::open({directory.path(), each * 2});
    REQUIRE(smaller);
    REQUIRE(smaller.value()->residentBytes() <= each * 2);
    REQUIRE(smaller.value()->entryCount() == 2);
    REQUIRE(entryFiles(directory.path()).size() == 2);
}

TEST_CASE("Rewriting an entry does not count it twice", "[spectral][tilestore]") {
    const TemporaryDirectory directory{"rewrite"};
    const StoredTile tile = madeUpTile(32, 65, 71);
    const std::uint64_t each = SpectrogramTileStore::entrySizeFor(tile.magnitudes.size());
    auto store = SpectrogramTileStore::open({directory.path(), each * 3});
    REQUIRE(store);

    const ContentKey key = madeUpKey(67);
    const TileLayout layout = smallLayout();
    for (int pass = 0; pass < 5; ++pass) {
        REQUIRE(store.value()->write(key, kOverviewEntry, layout, tile));
        REQUIRE(store.value()->entryCount() == 1);
        REQUIRE(store.value()->residentBytes() == each);
    }
    REQUIRE(entryFiles(directory.path()).size() == 1);
}

TEST_CASE("Replacing an entry with a larger one stays inside the budget", "[spectral][tilestore]") {
    // The entry being replaced is the least recently used one, so a policy that
    // treated it as an ordinary eviction candidate would reclaim its bytes
    // twice: once by deleting it and once by only asking for the difference
    // between the old size and the new. The store would then sit over its
    // budget by the size of the entry it thought it had removed, and this is
    // the arrangement that makes that happen rather than a contrived one --
    // rebuilding a tile at a finer setting does exactly this.
    const TemporaryDirectory directory{"grown"};
    const StoredTile small = madeUpTile(16, 65, 163);
    const StoredTile large = madeUpTile(32, 65, 167);
    const std::uint64_t each = SpectrogramTileStore::entrySizeFor(small.magnitudes.size());
    auto store = SpectrogramTileStore::open({directory.path(), each * 2});
    REQUIRE(store);

    const ContentKey key = madeUpKey(173);
    const TileLayout layout = smallLayout();
    REQUIRE(store.value()->write(key, tileEntry(0), layout, small));
    REQUIRE(store.value()->write(key, tileEntry(1), layout, small));
    REQUIRE(store.value()->residentBytes() == each * 2);

    REQUIRE(store.value()->write(key, tileEntry(0), layout, large));
    REQUIRE(store.value()->residentBytes() <= store.value()->budgetBytes());

    // And the accounting still describes the directory rather than a story
    // about it.
    std::uintmax_t onDisk = 0;
    for (const std::filesystem::path& path : entryFiles(directory.path())) {
        onDisk += std::filesystem::file_size(path);
    }
    REQUIRE(static_cast<std::uint64_t>(onDisk) == store.value()->residentBytes());

    const Result<StoredTile> read = store.value()->read(key, tileEntry(0), layout);
    REQUIRE(read);
    REQUIRE(read.value().magnitudes == large.magnitudes);
}

TEST_CASE("An entry larger than the whole budget is refused", "[spectral][tilestore]") {
    const TemporaryDirectory directory{"toolarge"};
    const StoredTile tile = madeUpTile(64, 65, 83);
    auto store = SpectrogramTileStore::open(
        {directory.path(), SpectrogramTileStore::entrySizeFor(tile.magnitudes.size()) - 1});
    REQUIRE(store);
    REQUIRE_FALSE(store.value()->write(madeUpKey(89), kOverviewEntry, smallLayout(), tile));
    REQUIRE(store.value()->entryCount() == 0);
    REQUIRE(entryFiles(directory.path()).empty());
}

TEST_CASE("A cache directory that cannot be used degrades to memory", "[spectral][tilestore]") {
    const TemporaryDirectory directory{"degrade"};

    SECTION("a path that is a file rather than a directory") {
        const std::filesystem::path path = directory.path() / "not-a-directory";
        writeWhole(path, std::vector<std::uint8_t>{1, 2, 3});
        REQUIRE_FALSE(SpectrogramTileStore::open({path, std::uint64_t{1} << 20}));
    }

    SECTION("a budget too small to hold anything") {
        REQUIRE_FALSE(SpectrogramTileStore::open({directory.path(), 8}));
    }

    SECTION("a directory that disappears after it was opened") {
        const std::filesystem::path cache = directory.path() / "vanishing";
        auto store = SpectrogramTileStore::open({cache, std::uint64_t{16} << 20});
        REQUIRE(store);
        std::error_code code;
        std::filesystem::remove_all(cache, code);
        REQUIRE_FALSE(store.value()->write(madeUpKey(97), kOverviewEntry, smallLayout(),
                                           madeUpTile(8, 9, 101)));
        REQUIRE_FALSE(store.value()->read(madeUpKey(97), kOverviewEntry, smallLayout()));
    }

    SECTION("a directory that is read-only") {
        const std::filesystem::path cache = directory.path() / "readonly";
        auto store = SpectrogramTileStore::open({cache, std::uint64_t{16} << 20});
        REQUIRE(store);
        std::error_code code;
        std::filesystem::permissions(cache,
                                     std::filesystem::perms::owner_write |
                                         std::filesystem::perms::group_write |
                                         std::filesystem::perms::others_write,
                                     std::filesystem::perm_options::remove, code);
        const bool readOnly = !stillWritable(cache);
        const bool refused =
            readOnly && !store.value()->write(madeUpKey(103), kOverviewEntry, smallLayout(),
                                              madeUpTile(8, 9, 107));
        // Put the permissions back before asserting anything, or a failure here
        // leaves a directory behind that nothing can delete.
        std::filesystem::permissions(cache, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, code);
        if (readOnly) {
            REQUIRE(refused);
        } else {
            // Root ignores the bits, and a Windows ACL is not these bits at
            // all. Say so rather than asserting something that did not happen.
            WARN("could not make the directory read-only here; write refusal not exercised");
        }
    }

    SECTION("and an analysis with no store at all is unaffected") {
        const AudioBuffer buffer = material(40000);
        const auto source = sourceOf(buffer);
        SpectrogramTiles::Settings settings = smallSettings();
        REQUIRE(settings.store == nullptr);
        REQUIRE(settings.key.isNull());
        auto tiles = SpectrogramTiles::create(source, 0, settings);
        REQUIRE(tiles);
        REQUIRE(tiles.value().buildOverview());
        REQUIRE_FALSE(tiles.value().overviewCameFromStore());
        REQUIRE(tiles.value().ensureDetail(0, buffer.frames()));
        REQUIRE(tiles.value().fineFrame(0) != nullptr);
    }
}

TEST_CASE("A store with a key but no store, or a store but no key, stores nothing",
          "[spectral][tilestore]") {
    const TemporaryDirectory directory{"halfconfigured"};
    const AudioBuffer buffer = material(40000);
    const auto source = sourceOf(buffer);

    SpectrogramTiles::Settings settings = smallSettings();
    auto store = SpectrogramTileStore::open({directory.path(), std::uint64_t{16} << 20});
    REQUIRE(store);
    settings.store = store.value();
    // No key: a cache keyed on nothing would serve one file's tiles for
    // another's, so it has to do nothing at all instead.
    auto tiles = SpectrogramTiles::create(source, 0, settings);
    REQUIRE(tiles);
    REQUIRE(tiles.value().buildOverview());
    REQUIRE(tiles.value().ensureDetail(0, buffer.frames()));
    REQUIRE(store.value()->entryCount() == 0);
}

TEST_CASE("The default cache directory is the one this platform reserves for caches",
          "[spectral][tilestore]") {
    const std::filesystem::path directory = SpectrogramTileStore::defaultDirectory();
    REQUIRE_FALSE(directory.empty());
    REQUIRE(directory.is_absolute());
    REQUIRE(directory.filename() == "spectrogram-tiles");

#if !defined(_WIN32)
    // Read the environment rather than setting it: this asserts the branch this
    // machine actually takes, and leaves the process's environment alone.
    const char* cache = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    if (cache != nullptr && *cache != '\0') {
        REQUIRE(directory == std::filesystem::path{cache} / "auscultate" / "spectrogram-tiles");
    } else if (home != nullptr && *home != '\0') {
        REQUIRE(directory ==
                std::filesystem::path{home} / ".cache" / "auscultate" / "spectrogram-tiles");
    } else {
        REQUIRE(directory.parent_path().filename() == "auscultate");
    }
#endif
}

TEST_CASE("Several threads through one store do not corrupt it", "[tilestore][threads]") {
    // Two processes cannot be arranged from here, so this is the case that can
    // be: several workers fetching detail for different parts of one file, over
    // a budget small enough that they are evicting each other constantly.
    //
    // What is being asserted is that nothing crashed, nothing raced -- which is
    // ThreadSanitizer's to say rather than this test's -- and that every read
    // that succeeded returned exactly what was written, because a half-written
    // or half-evicted entry that still passed its checks would be the failure
    // this whole design is trying to prevent.
    const TemporaryDirectory directory{"threads"};
    constexpr int kEntries = 12;
    const StoredTile pattern = madeUpTile(16, 65, 127);
    const std::uint64_t each = SpectrogramTileStore::entrySizeFor(pattern.magnitudes.size());
    auto store = SpectrogramTileStore::open({directory.path(), each * 5});
    REQUIRE(store);

    const ContentKey key = madeUpKey(131);
    const TileLayout layout = smallLayout();
    std::vector<StoredTile> tiles;
    tiles.reserve(kEntries);
    for (int index = 0; index < kEntries; ++index) {
        tiles.push_back(madeUpTile(16, 65, static_cast<unsigned>(200 + index)));
    }

    std::atomic<int> mismatches{0};
    std::atomic<int> hits{0};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&, worker] {
            for (int pass = 0; pass < 24; ++pass) {
                const int index = (worker * 7 + pass * 3) % kEntries;
                const auto entry = tileEntry(index);
                (void)store.value()->write(key, entry, layout,
                                           tiles[static_cast<std::size_t>(index)]);
                const Result<StoredTile> read = store.value()->read(key, entry, layout);
                if (read) {
                    hits.fetch_add(1, std::memory_order_relaxed);
                    if (read.value().magnitudes !=
                        tiles[static_cast<std::size_t>(index)].magnitudes) {
                        mismatches.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                (void)store.value()->residentBytes();
                (void)store.value()->entryCount();
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    REQUIRE(mismatches.load() == 0);
    REQUIRE(hits.load() > 0);
    REQUIRE(store.value()->residentBytes() <= store.value()->budgetBytes());
    REQUIRE(entryFiles(directory.path()).size() == store.value()->entryCount());
}

TEST_CASE("Several tiled caches sharing one store agree about what is in it",
          "[tilestore][threads]") {
    const TemporaryDirectory directory{"sharedtiles"};
    const AudioBuffer buffer = material(120000);
    const auto source = sourceOf(buffer);

    SpectrogramTiles::Settings settings = smallSettings();
    const Result<ContentKey> key =
        contentKeyForSource(*source, SpectrogramTiles::layoutFor(settings, 0));
    REQUIRE(key);
    auto store = SpectrogramTileStore::open({directory.path(), std::uint64_t{32} << 20});
    REQUIRE(store);
    settings.store = store.value();
    settings.key = key.value();

    auto reference = SpectrogramTiles::create(source, 0, settings);
    REQUIRE(reference);
    REQUIRE(reference.value().buildOverview());
    REQUIRE(reference.value().ensureDetail(0, buffer.frames()));

    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 3; ++worker) {
        workers.emplace_back([&, worker] {
            auto tiles = SpectrogramTiles::create(source, 0, settings);
            if (!tiles || !tiles.value().buildOverview()) {
                failures.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const SampleIndex from = worker * 20000;
            if (!tiles.value().ensureDetail(from, from + 40000)) {
                failures.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            for (SampleCount frame = 0; frame < tiles.value().fineFrameCount(); ++frame) {
                const std::uint8_t* got = tiles.value().fineFrame(frame);
                if (got == nullptr) {
                    continue;
                }
                const std::uint8_t* want = reference.value().fineFrame(frame);
                if (want == nullptr || !std::equal(got, got + tiles.value().binCount(), want)) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    REQUIRE(failures.load() == 0);
}
