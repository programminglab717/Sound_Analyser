#pragma once

#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/io/AudioSource.h>
#include <sa/spectral/SpectrogramPyramid.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sa::spectral {

/// Everything about an analysis that changes the bytes it produces.
///
/// Deliberately not SpectrogramTiles::Settings. That also carries a memory
/// budget, which changes what is resident rather than what is computed, and a
/// store that keyed on it would be keying on something it has no business
/// keying on. It also keeps the dependency pointing one way: the tiled cache
/// knows about the store, the store knows nothing about the tiled cache.
struct TileLayout {
    SpectrogramConfig config;

    /// Decimation of the overview. Changes every byte of the overview and
    /// nothing about a detail tile; it is keyed on anyway, for the reason given
    /// on contentKeyForSource.
    int coarseLevel = 6;

    /// Level-0 frames per detail tile. Changes where tiles begin and end, so a
    /// tile stored under one value is not a tile under another.
    SampleCount tileFrames = 1024;

    int channel = 0;
};

/// Identifies what was analysed: the audio, and every setting that changes the
/// result of analysing it.
struct ContentKey {
    std::array<std::uint8_t, 32> bytes{};

    /// True for a default-constructed key, which means "no key was derived", so
    /// nothing may be stored or loaded under it. A genuine digest of thirty-two
    /// zero bytes would be indistinguishable from this; at a probability of
    /// 2^-256 that is not a case worth designing around, and its consequence
    /// would be a cache that declines to work rather than one that returns the
    /// wrong bytes.
    [[nodiscard]] bool isNull() const noexcept;

    /// Lower-case hex, 64 characters. This is the file name in the store, so it
    /// has to stay stable across builds and platforms.
    [[nodiscard]] std::string hex() const;

    friend bool operator==(const ContentKey&, const ContentKey&) noexcept = default;
};

/// Derive a key from a file on disk, without reading all of it.
///
/// The digest covers, in this order and at fixed widths so that no two
/// different inputs can serialise to the same bytes:
///
///  - a scheme tag, so a future change to what is sampled produces different
///    keys rather than colliding with the keys this version wrote;
///  - every field of `layout`, the decibel range included, because that is the
///    quantisation and it changes every stored byte;
///  - the file's size and last-write time;
///  - the first 4 KiB of the file, which is the container header for every
///    format this program reads;
///  - sixteen 4 KiB blocks at evenly spaced offsets through the file, each
///    hashed together with the offset it was read from, so a block that moves
///    is a different key.
///
/// A file smaller than the header plus the blocks is hashed whole.
///
/// **What this guarantees.** The digest is SHA-256, so two different sequences
/// of hashed bytes give different keys, other than by a collision nobody has
/// ever exhibited. Any change to any setting changes the key. Two files of
/// different length, or differing anywhere in the first 4 KiB or in any sampled
/// block, get different keys.
///
/// **What it does not.** It is a sampled hash, not a hash of the file. Two
/// files of the same length and timestamp whose differences all fall between
/// the sampled blocks get the *same* key, and the cache would then serve tiles
/// of the first for the second. This is not a cryptographic identification of a
/// file and must never be used as one: the sampling positions are fixed and
/// public, so constructing two files that collide is arithmetic rather than an
/// attack. It is a cache key, and its failure mode has to be weighed against
/// reading several gigabytes on every open.
///
/// The last-write time is in the digest for that reason. It is not content, and
/// including it means a file whose timestamp changed -- restored from a backup,
/// copied by a tool that does not preserve it -- misses and is rebuilt. That
/// costs what it cost before this existed. Leaving it out would mean a file
/// edited in place between the sampled blocks hits and draws the wrong picture,
/// and a needless rebuild is cheap where wrong pixels are not.
[[nodiscard]] Result<ContentKey> contentKeyForFile(const std::filesystem::path& audioPath,
                                                   const TileLayout& layout);

/// Derive a key from a source that may have no file behind it.
///
/// The same scheme and the same guarantees, over what an AudioSource can offer
/// in place of bytes on disk: the sample rate, channel count, frame count and
/// stored format, then sixteen evenly spaced blocks of 4096 decoded frames of
/// the channel being analysed, hashed as their float bit patterns. Nothing here
/// sees a timestamp, so the caveat above about in-place edits applies with
/// nothing to narrow it.
///
/// Prefer contentKeyForFile where a path is known: decoding sampled blocks of a
/// compressed file costs more than reading the same blocks of it, and the file
/// hash also notices a change that the decoder happens to round away.
///
/// `layout.coarseLevel` and `layout.tileFrames` are hashed although neither
/// changes a detail tile's bytes. Being over-conservative costs a rebuild after
/// a settings change that did not need one; being under-conservative costs a
/// wrong picture, and one entry kind in the store -- the overview -- does
/// depend on both.
[[nodiscard]] Result<ContentKey> contentKeyForSource(const io::AudioSource& source,
                                                     const TileLayout& layout);

/// One stored block of quantised magnitudes, with the shape needed to read it.
///
/// The shape is stored beside the bytes rather than recomputed on load, so that
/// a loader can check what it got against what it asked for. A tile whose
/// recorded shape disagrees with the request is refused, which is the case that
/// matters when a file has been half-overwritten by a newer analysis.
struct StoredTile {
    SampleCount firstFrame = 0;   ///< Level-0 frame of the first column.
    SampleCount frameCount = 0;   ///< Columns.
    SampleCount hop = 0;          ///< Samples between columns.
    SampleCount sourceFrames = 0; ///< Frames in the source this came from.
    int binCount = 0;             ///< Rows.

    /// `frameCount * binCount` bytes, frame-major, exactly as SpectrogramTiles
    /// and SpectrogramPyramid hold them.
    std::vector<std::uint8_t> magnitudes;
};

/// Entry number of the overview within a key. Detail tile `n` is `tileEntry(n)`.
inline constexpr std::uint64_t kOverviewEntry = 0;

[[nodiscard]] constexpr std::uint64_t tileEntry(SampleCount tileIndex) noexcept {
    return static_cast<std::uint64_t>(tileIndex) + 1;
}

/// Where a store keeps its files and how much it may keep.
///
/// At namespace scope rather than nested in SpectrogramTileStore, for the same
/// reason as WavOptions: a nested type's default member initialisers are not
/// usable in a default argument of the enclosing class, so `open(Options = {})`
/// would not compile.
struct TileStoreOptions {
    /// Empty means SpectrogramTileStore::defaultDirectory().
    std::filesystem::path directory;

    /// Ceiling on the total size of the files in the directory. Entries are
    /// evicted least-recently-used to stay under it.
    std::uint64_t budgetBytes = std::uint64_t{1} << 30;
};

/// Analysed tiles kept between runs.
///
/// The in-memory cache above this throws everything away at exit, so reopening
/// a long recording rebuilds an overview that was already built once -- seconds
/// of waiting for a result that has not changed. This keeps those bytes.
///
/// Every file here is untrusted input. Not because anyone is expected to attack
/// it, but because a crash or a full disk during a write produces a half-written
/// one, and that is an ordinary Tuesday rather than an attack. So a read
/// validates a magic number, a format version, the key, the entry number, every
/// layout field, the recorded shape, the payload length against the real size of
/// the file, and a CRC over each of the header and the payload -- and only then
/// hands the bytes back. Nothing is mapped, and no length read out of a file is
/// used to size an allocation before it has been checked against the file that
/// claims it.
///
/// Writes go to a temporary file in the same directory and are renamed over the
/// target, so a reader sees either the previous entry or the new one. That is
/// not a durability guarantee: nothing here calls fsync, because a disposable
/// cache does not earn the cost of one, and a crash between the write and the
/// rename leaves a temporary file that the next open cleans up. The CRC is what
/// makes that safe, not the rename.
///
/// **Optional by construction.** Every failure -- no directory, no permission, a
/// full disk, a corrupt entry -- degrades to computing the tile, which is what
/// the program did before this class existed. A caller that cannot open a store
/// passes none:
///
/// ```
/// settings.store = SpectrogramTileStore::open(options).valueOr(nullptr);
/// ```
class SpectrogramTileStore {
public:
    /// Where a cache belongs on this platform.
    ///
    /// Windows: `%LOCALAPPDATA%\Auscultate\spectrogram-tiles`. Local rather
    /// than roaming: `%APPDATA%` follows the user between machines on a domain,
    /// and copying a gigabyte of regenerable derived data over the network at
    /// every logon would be a support ticket with our name on it.
    ///
    /// Elsewhere: `$XDG_CACHE_HOME/auscultate/spectrogram-tiles`, or
    /// `$HOME/.cache/...` when that is unset, which is what the XDG base
    /// directory specification reserves for regenerable data -- and what tells a
    /// backup tool to skip it.
    ///
    /// If neither is available, the system temporary directory, which is the
    /// last place likely to be writable before giving up. Failing that an empty
    /// path, which open() reports as an error rather than guessing.
    [[nodiscard]] static std::filesystem::path defaultDirectory();

    /// Open, creating the directory if it does not exist, and index what is
    /// already there.
    ///
    /// Fails if the directory cannot be created or listed. It does not fail for
    /// a directory that cannot be written to: that is only discovered on a
    /// write, and a store that can read an existing cache but not add to it is
    /// still worth having.
    [[nodiscard]] static Result<std::shared_ptr<SpectrogramTileStore>>
    open(TileStoreOptions options = {});

    ~SpectrogramTileStore();

    SpectrogramTileStore(const SpectrogramTileStore&) = delete;
    SpectrogramTileStore& operator=(const SpectrogramTileStore&) = delete;

    /// Read one entry, or an error.
    ///
    /// `ErrorCode::NotFound` for an entry that is not there, and for one whose
    /// key, entry number or layout does not match -- a miss, not a fault.
    /// `ErrorCode::CorruptData` for one that is there and is not readable, and
    /// that file is then deleted: a file that failed its checks once will fail
    /// them every time, and leaving it would mean paying for the failure at
    /// every open until the eviction policy happened to reach it.
    [[nodiscard]] Result<StoredTile> read(const ContentKey& key, std::uint64_t entry,
                                          const TileLayout& layout) const;

    /// Store one entry, replacing any entry already under that key and number,
    /// and evict least-recently-used entries until the total is within budget.
    ///
    /// An entry larger than the whole budget is refused rather than stored and
    /// immediately evicted, because storing it would break the bound for as
    /// long as it took to notice.
    [[nodiscard]] Status write(const ContentKey& key, std::uint64_t entry, const TileLayout& layout,
                               const StoredTile& tile);

    /// Total size of the files this store is accounting for.
    [[nodiscard]] std::uint64_t residentBytes() const noexcept;

    [[nodiscard]] std::size_t entryCount() const noexcept;

    [[nodiscard]] std::uint64_t budgetBytes() const noexcept;

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }

    /// Delete every entry. The directory itself is kept.
    void clear() noexcept;

    /// Bytes an entry of this payload size occupies, header included. Exposed
    /// so that a caller sizing a budget is not guessing at the overhead.
    [[nodiscard]] static std::uint64_t entrySizeFor(std::uint64_t payloadBytes) noexcept;

private:
    SpectrogramTileStore(std::filesystem::path directory, std::uint64_t budgetBytes);

    struct Entry {
        std::uint64_t bytes = 0;

        /// A counter rather than a timestamp. Timestamps on a cache directory
        /// have whatever resolution the filesystem felt like giving them, and
        /// two tiles written in the same millisecond would then evict in an
        /// arbitrary order. The counter is seeded from the files' modified times
        /// at open, so the order survives a restart approximately and is exact
        /// within a run.
        std::uint64_t lastUse = 0;
    };

    void indexDirectory();
    /// Called with the lock held. Evicts until `residentBytes_ + incoming`
    /// fits, never choosing `keep` -- which is the entry being replaced, whose
    /// bytes `incoming` has already accounted for.
    void evictFor(std::uint64_t incoming, const std::string& keep);

    std::filesystem::path directory_;
    std::uint64_t budgetBytes_ = 0;

    /// Guards the index *and* the file operations, not just the map.
    ///
    /// Serialising reads costs throughput on a cache whose entries are a
    /// megabyte out of the page cache, which is a price worth paying twice
    /// over. Once because a read overlapping an eviction would be reading a file
    /// another thread is deleting -- harmless on POSIX, where the descriptor
    /// outlives the name, and a sharing violation on Windows, where it does not.
    /// Once because the index and the directory would otherwise disagree about
    /// what exists, and the budget is computed from the index.
    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, Entry> entries_;
    mutable std::uint64_t residentBytes_ = 0;
    mutable std::uint64_t useCounter_ = 0;
};

} // namespace sa::spectral
