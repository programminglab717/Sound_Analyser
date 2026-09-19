#include "TileHashing.h"

#include <sa/core/AudioBuffer.h>
#include <sa/spectral/SpectrogramTileStore.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace sa::spectral {

namespace {

using detail::crc32;
using detail::Sha256;

// --- The on-disk entry -----------------------------------------------------

constexpr std::uint64_t kMagic = 0x45525453'4C544153ull; // "SATLSTRE", little-endian
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kHeaderBytes = 144;

/// Refuse anything claiming more than this before allocating for it. A detail
/// tile at the display settings is about a megabyte and an overview of a
/// three-hour recording is tens; a gibibyte is far above anything this program
/// writes and far below anything that would hurt to refuse.
constexpr std::uint64_t kMaximumPayloadBytes = std::uint64_t{1} << 30;

constexpr std::uint64_t kHeaderSampleBytes = 4096;
constexpr int kSampleBlocks = 16;
constexpr std::uint64_t kSampleBlockBytes = 4096;
constexpr SampleCount kSampleBlockFrames = 4096;

/// A temporary file younger than this may belong to another process writing
/// right now, so it is left alone. Older than this, and the process that wrote
/// it is gone.
constexpr std::chrono::hours kStaleTemporaryAge{1};

struct EntryHeader {
    std::uint64_t magic = kMagic;
    std::uint32_t version = kFormatVersion;
    std::uint32_t headerBytes = static_cast<std::uint32_t>(kHeaderBytes);
    std::array<std::uint8_t, 32> key{};
    std::uint64_t entry = 0;
    std::int32_t fftSize = 0;
    std::int32_t hopSize = 0;
    std::int32_t window = 0;
    std::uint32_t minimumDecibels = 0; ///< Bit pattern, not a value: NaN must compare equal to NaN.
    std::uint32_t maximumDecibels = 0;
    std::int32_t coarseLevel = 0;
    std::int64_t tileFrames = 0;
    std::int32_t channel = 0;
    std::int32_t binCount = 0;
    std::int64_t firstFrame = 0;
    std::int64_t frameCount = 0;
    std::int64_t hop = 0;
    std::int64_t sourceFrames = 0;
    std::uint64_t payloadBytes = 0;
    std::uint32_t payloadCrc = 0;
    std::uint32_t headerCrc = 0;
};

/// Byte-at-a-time, little-endian, in a fixed order. Not a memcpy of the struct:
/// that would bake this machine's padding and endianness into the format, and a
/// cache written by one build and read by another is exactly the case the
/// version field exists to make safe rather than to make interesting.
class FieldWriter {
public:
    explicit FieldWriter(std::uint8_t* out) noexcept : out_(out) {}

    void u32(std::uint32_t value) noexcept {
        for (std::size_t i = 0; i < 4; ++i) {
            out_[at_++] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
        }
    }

    void u64(std::uint64_t value) noexcept {
        for (std::size_t i = 0; i < 8; ++i) {
            out_[at_++] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
        }
    }

    void i32(std::int32_t value) noexcept { u32(static_cast<std::uint32_t>(value)); }

    void i64(std::int64_t value) noexcept { u64(static_cast<std::uint64_t>(value)); }

    void raw(const std::uint8_t* data, std::size_t bytes) noexcept {
        std::memcpy(out_ + at_, data, bytes);
        at_ += bytes;
    }

private:
    std::uint8_t* out_ = nullptr;
    std::size_t at_ = 0;
};

class FieldReader {
public:
    explicit FieldReader(const std::uint8_t* in) noexcept : in_(in) {}

    [[nodiscard]] std::uint32_t u32() noexcept {
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            value |= static_cast<std::uint32_t>(in_[at_++]) << (8 * i);
        }
        return value;
    }

    [[nodiscard]] std::uint64_t u64() noexcept {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            value |= static_cast<std::uint64_t>(in_[at_++]) << (8 * i);
        }
        return value;
    }

    [[nodiscard]] std::int32_t i32() noexcept { return static_cast<std::int32_t>(u32()); }

    [[nodiscard]] std::int64_t i64() noexcept { return static_cast<std::int64_t>(u64()); }

    void raw(std::uint8_t* out, std::size_t bytes) noexcept {
        std::memcpy(out, in_ + at_, bytes);
        at_ += bytes;
    }

private:
    const std::uint8_t* in_ = nullptr;
    std::size_t at_ = 0;
};

/// Both directions walk the fields in the same order, so the layout is stated
/// once in each and a field added to one without the other stops compiling long
/// before it silently shifts everything after it.
[[nodiscard]] std::array<std::uint8_t, kHeaderBytes> encodeHeader(const EntryHeader& header) {
    std::array<std::uint8_t, kHeaderBytes> bytes{};
    FieldWriter out{bytes.data()};
    out.u64(header.magic);
    out.u32(header.version);
    out.u32(header.headerBytes);
    out.raw(header.key.data(), header.key.size());
    out.u64(header.entry);
    out.i32(header.fftSize);
    out.i32(header.hopSize);
    out.i32(header.window);
    out.u32(header.minimumDecibels);
    out.u32(header.maximumDecibels);
    out.i32(header.coarseLevel);
    out.i64(header.tileFrames);
    out.i32(header.channel);
    out.i32(header.binCount);
    out.i64(header.firstFrame);
    out.i64(header.frameCount);
    out.i64(header.hop);
    out.i64(header.sourceFrames);
    out.u64(header.payloadBytes);
    out.u32(header.payloadCrc);
    // The header's own CRC covers everything before it, so it is computed from
    // the encoded bytes rather than from the fields.
    out.u32(crc32(bytes.data(), kHeaderBytes - 4));
    return bytes;
}

[[nodiscard]] EntryHeader decodeHeader(const std::array<std::uint8_t, kHeaderBytes>& bytes) {
    EntryHeader header;
    FieldReader in{bytes.data()};
    header.magic = in.u64();
    header.version = in.u32();
    header.headerBytes = in.u32();
    in.raw(header.key.data(), header.key.size());
    header.entry = in.u64();
    header.fftSize = in.i32();
    header.hopSize = in.i32();
    header.window = in.i32();
    header.minimumDecibels = in.u32();
    header.maximumDecibels = in.u32();
    header.coarseLevel = in.i32();
    header.tileFrames = in.i64();
    header.channel = in.i32();
    header.binCount = in.i32();
    header.firstFrame = in.i64();
    header.frameCount = in.i64();
    header.hop = in.i64();
    header.sourceFrames = in.i64();
    header.payloadBytes = in.u64();
    header.payloadCrc = in.u32();
    header.headerCrc = in.u32();
    return header;
}

[[nodiscard]] EntryHeader headerFor(const ContentKey& key, std::uint64_t entry,
                                    const TileLayout& layout, const StoredTile& tile) {
    EntryHeader header;
    header.key = key.bytes;
    header.entry = entry;
    header.fftSize = layout.config.fftSize;
    header.hopSize = layout.config.hopSize;
    header.window = static_cast<std::int32_t>(layout.config.window);
    header.minimumDecibels = std::bit_cast<std::uint32_t>(layout.config.minimumDecibels);
    header.maximumDecibels = std::bit_cast<std::uint32_t>(layout.config.maximumDecibels);
    header.coarseLevel = layout.coarseLevel;
    header.tileFrames = layout.tileFrames;
    header.channel = layout.channel;
    header.binCount = tile.binCount;
    header.firstFrame = tile.firstFrame;
    header.frameCount = tile.frameCount;
    header.hop = tile.hop;
    header.sourceFrames = tile.sourceFrames;
    header.payloadBytes = static_cast<std::uint64_t>(tile.magnitudes.size());
    header.payloadCrc = crc32(tile.magnitudes.data(), tile.magnitudes.size());
    return header;
}

/// Whether a header describes the analysis the caller is asking about. Refusing
/// on a mismatch matters although the settings are already in the key: a key
/// collision, a half-overwritten file or a name reused by a future version of
/// this code all land here, and the difference between refusing and trusting is
/// the difference between a rebuild and a wrong picture.
[[nodiscard]] bool describesSameAnalysis(const EntryHeader& header, const ContentKey& key,
                                         std::uint64_t entry, const TileLayout& layout) noexcept {
    return header.key == key.bytes && header.entry == entry &&
           header.fftSize == layout.config.fftSize && header.hopSize == layout.config.hopSize &&
           header.window == static_cast<std::int32_t>(layout.config.window) &&
           header.minimumDecibels == std::bit_cast<std::uint32_t>(layout.config.minimumDecibels) &&
           header.maximumDecibels == std::bit_cast<std::uint32_t>(layout.config.maximumDecibels) &&
           header.coarseLevel == layout.coarseLevel && header.tileFrames == layout.tileFrames &&
           header.channel == layout.channel;
}

[[nodiscard]] std::string hexOf(std::uint64_t value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (std::size_t i = 0; i < 16; ++i) {
        out[15 - i] = kDigits[static_cast<std::size_t>((value >> (4 * i)) & 0xFu)];
    }
    return out;
}

[[nodiscard]] std::string entryName(const ContentKey& key, std::uint64_t entry) {
    return key.hex() + "-" + hexOf(entry) + ".satile";
}

/// Unique enough that two processes writing the same entry at the same instant
/// do not collide on the temporary name. It does not have to be unguessable;
/// it has to be unrepeated.
[[nodiscard]] std::string temporaryName(const std::string& name) {
    static std::atomic<std::uint64_t> counter{
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
    return name + "." + hexOf(counter.fetch_add(1, std::memory_order_relaxed)) + ".tmp";
}

// --- Hashing the inputs ----------------------------------------------------

void hashBytes(Sha256& sha, const void* data, std::size_t bytes) {
    sha.update(data, bytes);
}

void hashU64(Sha256& sha, std::uint64_t value) {
    std::array<std::uint8_t, 8> bytes{};
    FieldWriter out{bytes.data()};
    out.u64(value);
    hashBytes(sha, bytes.data(), bytes.size());
}

void hashI64(Sha256& sha, std::int64_t value) {
    hashU64(sha, static_cast<std::uint64_t>(value));
}

void hashU32(Sha256& sha, std::uint32_t value) {
    std::array<std::uint8_t, 4> bytes{};
    FieldWriter out{bytes.data()};
    out.u32(value);
    hashBytes(sha, bytes.data(), bytes.size());
}

void hashI32(Sha256& sha, std::int32_t value) {
    hashU32(sha, static_cast<std::uint32_t>(value));
}

/// Every field is fixed width and hashed in a fixed order, so two different
/// layouts cannot serialise to the same bytes -- the ambiguity a variable-length
/// encoding would introduce is the one way a hash of the right things still
/// collides.
void hashLayout(Sha256& sha, std::string_view tag, const TileLayout& layout) {
    hashU64(sha, static_cast<std::uint64_t>(tag.size()));
    hashBytes(sha, tag.data(), tag.size());
    hashI32(sha, layout.config.fftSize);
    hashI32(sha, layout.config.hopSize);
    hashI32(sha, static_cast<std::int32_t>(layout.config.window));
    hashU32(sha, std::bit_cast<std::uint32_t>(layout.config.minimumDecibels));
    hashU32(sha, std::bit_cast<std::uint32_t>(layout.config.maximumDecibels));
    hashI32(sha, layout.coarseLevel);
    hashI64(sha, layout.tileFrames);
    hashI32(sha, layout.channel);
}

/// Read exactly `bytes` at `offset`, or fail. A short read here means the file
/// changed under us between sizing it and hashing it, which is a reason to
/// refuse to produce a key rather than to produce one over whatever arrived.
[[nodiscard]] bool readExactly(std::ifstream& stream, std::uint64_t offset, std::uint8_t* out,
                               std::size_t bytes) {
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream) {
        return false;
    }
    stream.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(bytes));
    return stream.gcount() == static_cast<std::streamsize>(bytes);
}

} // namespace

// --- ContentKey ------------------------------------------------------------

bool ContentKey::isNull() const noexcept {
    return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t b) { return b == 0; });
}

std::string ContentKey::hex() const {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        out.push_back(kDigits[static_cast<std::size_t>(byte >> 4)]);
        out.push_back(kDigits[static_cast<std::size_t>(byte & 0x0Fu)]);
    }
    return out;
}

Result<ContentKey> contentKeyForFile(const std::filesystem::path& audioPath,
                                     const TileLayout& layout) {
    std::error_code code;
    if (!std::filesystem::is_regular_file(audioPath, code) || code) {
        return Error{ErrorCode::NotFound, "no file to key on"};
    }
    const std::uintmax_t size = std::filesystem::file_size(audioPath, code);
    if (code) {
        return Error{ErrorCode::IoFailure, "could not size the file"};
    }
    const std::filesystem::file_time_type stamp = std::filesystem::last_write_time(audioPath, code);
    if (code) {
        return Error{ErrorCode::IoFailure, "could not read the file's modified time"};
    }
    std::ifstream stream{audioPath, std::ios::binary};
    if (!stream) {
        return Error{ErrorCode::IoFailure, "could not open the file to key on"};
    }

    Sha256 sha;
    hashLayout(sha, "sa-spectrogram-file-key/1", layout);
    hashU64(sha, static_cast<std::uint64_t>(size));
    hashI64(sha, static_cast<std::int64_t>(stamp.time_since_epoch().count()));

    const std::uint64_t total = static_cast<std::uint64_t>(size);
    const std::uint64_t sampled =
        kHeaderSampleBytes + static_cast<std::uint64_t>(kSampleBlocks) * kSampleBlockBytes;

    std::vector<std::uint8_t> block(static_cast<std::size_t>(kSampleBlockBytes));
    if (total <= sampled) {
        // Small enough that sampling would read most of it anyway, so read all
        // of it and have no caveat at all for this case.
        hashU32(sha, 0); // Discriminator: the whole file follows.
        std::uint64_t at = 0;
        while (at < total) {
            const auto want =
                static_cast<std::size_t>(std::min<std::uint64_t>(kSampleBlockBytes, total - at));
            if (!readExactly(stream, at, block.data(), want)) {
                return Error{ErrorCode::IoFailure, "the file changed while it was being keyed"};
            }
            hashBytes(sha, block.data(), want);
            at += want;
        }
    } else {
        hashU32(sha, 1); // Discriminator: a header and sampled blocks follow.
        if (!readExactly(stream, 0, block.data(), static_cast<std::size_t>(kHeaderSampleBytes))) {
            return Error{ErrorCode::IoFailure, "the file changed while it was being keyed"};
        }
        hashBytes(sha, block.data(), static_cast<std::size_t>(kHeaderSampleBytes));

        // Evenly spaced, the first starting just past the header and the last
        // ending exactly at the end of the file. The offset goes into the
        // digest with the block, so a block that moves is a different key.
        const std::uint64_t span = total - kHeaderSampleBytes - kSampleBlockBytes;
        for (int index = 0; index < kSampleBlocks; ++index) {
            const std::uint64_t offset =
                kHeaderSampleBytes + span * static_cast<std::uint64_t>(index) /
                                         static_cast<std::uint64_t>(kSampleBlocks - 1);
            if (!readExactly(stream, offset, block.data(),
                             static_cast<std::size_t>(kSampleBlockBytes))) {
                return Error{ErrorCode::IoFailure, "the file changed while it was being keyed"};
            }
            hashU64(sha, offset);
            hashBytes(sha, block.data(), static_cast<std::size_t>(kSampleBlockBytes));
        }
    }

    return ContentKey{sha.finish()};
}

Result<ContentKey> contentKeyForSource(const io::AudioSource& source, const TileLayout& layout) {
    const io::AudioFileInfo& info = source.info();
    if (layout.channel < 0 || layout.channel >= info.channelCount()) {
        return Error{ErrorCode::InvalidArgument, "no such channel to key on"};
    }

    Sha256 sha;
    hashLayout(sha, "sa-spectrogram-source-key/1", layout);
    hashU64(sha, std::bit_cast<std::uint64_t>(info.sampleRate.hz()));
    hashI32(sha, info.channelCount());
    hashI64(sha, info.frameCount);
    hashI32(sha, static_cast<std::int32_t>(info.format));

    if (info.frameCount <= 0) {
        return ContentKey{sha.finish()};
    }

    // Only the channel being analysed goes into the digest. A change confined
    // to another channel produces the same key, which is right for a tile of
    // this one -- the bytes really are the same -- and is worth knowing before
    // this key is used for anything other than keying these tiles.
    AudioBuffer block{info.layout, kSampleBlockFrames};
    const SampleCount span = std::max<SampleCount>(0, info.frameCount - kSampleBlockFrames);
    for (int index = 0; index < kSampleBlocks; ++index) {
        const SampleIndex offset =
            span * static_cast<SampleCount>(index) / static_cast<SampleCount>(kSampleBlocks - 1);
        const Result<SampleCount> got = source.read(offset, block.view());
        if (!got) {
            return got.error();
        }
        const SampleCount frames = std::min<SampleCount>(got.value(), kSampleBlockFrames);
        hashI64(sha, offset);
        hashI64(sha, frames);
        if (frames > 0) {
            // The bit patterns, not the values: this is an identity check, and
            // two samples that compare equal but differ in their bits are two
            // different inputs to the analysis.
            hashBytes(sha, block.channel(layout.channel),
                      static_cast<std::size_t>(frames) * sizeof(float));
        }
    }

    return ContentKey{sha.finish()};
}

// --- SpectrogramTileStore --------------------------------------------------

SpectrogramTileStore::SpectrogramTileStore(std::filesystem::path directory,
                                           std::uint64_t budgetBytes)
    : directory_(std::move(directory)), budgetBytes_(budgetBytes) {}

SpectrogramTileStore::~SpectrogramTileStore() = default;

std::uint64_t SpectrogramTileStore::entrySizeFor(std::uint64_t payloadBytes) noexcept {
    return static_cast<std::uint64_t>(kHeaderBytes) + payloadBytes;
}

std::filesystem::path SpectrogramTileStore::defaultDirectory() {
#if defined(_WIN32)
    // The wide form, not getenv: a profile path containing anything outside the
    // active code page comes back mangled through the narrow one, and MSVC
    // makes getenv a deprecation warning, which this build treats as an error.
    wchar_t* value = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&value, &length, L"LOCALAPPDATA") == 0 && value != nullptr) {
        const std::filesystem::path base{value};
        std::free(value);
        if (!base.empty()) {
            return base / L"Auscult" / L"spectrogram-tiles";
        }
    } else {
        std::free(value);
    }
#else
    if (const char* cache = std::getenv("XDG_CACHE_HOME"); cache != nullptr && *cache != '\0') {
        return std::filesystem::path{cache} / "sound-analyser" / "spectrogram-tiles";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path{home} / ".cache" / "sound-analyser" / "spectrogram-tiles";
    }
#endif
    std::error_code code;
    const std::filesystem::path temporary = std::filesystem::temp_directory_path(code);
    if (!code && !temporary.empty()) {
        return temporary / "sound-analyser" / "spectrogram-tiles";
    }
    return {};
}

Result<std::shared_ptr<SpectrogramTileStore>> SpectrogramTileStore::open(TileStoreOptions options) {
    std::filesystem::path directory =
        options.directory.empty() ? defaultDirectory() : std::move(options.directory);
    if (directory.empty()) {
        return Error{ErrorCode::NotFound, "no cache directory on this system"};
    }
    if (options.budgetBytes < entrySizeFor(0)) {
        return Error{ErrorCode::InvalidArgument, "a cache budget below one header holds nothing"};
    }

    std::error_code code;
    std::filesystem::create_directories(directory, code);
    // create_directories reports an error for a path that already exists as
    // something other than a directory, and no error for one that is already a
    // directory, so the state of the path is what decides rather than the call.
    if (!std::filesystem::is_directory(directory, code)) {
        return Error{ErrorCode::IoFailure, "could not create the cache directory"};
    }

    auto store = std::shared_ptr<SpectrogramTileStore>{
        new SpectrogramTileStore{std::move(directory), options.budgetBytes}};
    // Nothing else can see it yet, which is why neither of these takes the lock
    // that both would need afterwards.
    store->indexDirectory();
    // A budget smaller than what the last run left behind has to bite now
    // rather than at the next write, or the bound would not hold for a store
    // that is only ever read from.
    store->evictFor(0, {});
    return store;
}

void SpectrogramTileStore::indexDirectory() {
    std::error_code code;
    std::filesystem::directory_iterator walk{directory_, code};
    if (code) {
        return;
    }

    const auto now = std::filesystem::file_time_type::clock::now();
    std::vector<std::pair<std::filesystem::file_time_type, std::string>> found;
    for (const std::filesystem::directory_entry& entry : walk) {
        const std::filesystem::path& path = entry.path();
        const std::string name = path.filename().string();
        if (path.extension() == ".tmp") {
            // Left behind by a process that died between writing and renaming.
            // Anything recent may belong to one that is still writing, so it
            // costs a little disk until the next open rather than a race.
            const auto written = std::filesystem::last_write_time(path, code);
            if (!code && now - written > kStaleTemporaryAge) {
                std::filesystem::remove(path, code);
            }
            continue;
        }
        if (path.extension() != ".satile") {
            continue;
        }
        const std::uintmax_t size = std::filesystem::file_size(path, code);
        if (code) {
            continue;
        }
        const auto written = std::filesystem::last_write_time(path, code);
        found.emplace_back(code ? now : written, name);
        entries_[name] = Entry{static_cast<std::uint64_t>(size), 0};
        residentBytes_ += static_cast<std::uint64_t>(size);
    }

    // Seed the recency order from the modified times, so that a restart does
    // not make every entry equally old and evict in whatever order the
    // directory happened to list.
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& seen : found) {
        entries_[seen.second].lastUse = ++useCounter_;
    }
}

void SpectrogramTileStore::evictFor(std::uint64_t incoming, const std::string& keep) {
    std::error_code code;
    while (residentBytes_ + incoming > budgetBytes_) {
        // `keep` is the entry about to be replaced, and it is not a candidate.
        // Evicting it would reclaim bytes that `incoming` has already had
        // deducted from it -- the replacement only needs the difference -- and
        // the store would end up over its budget by the size of the entry it
        // thought it had removed.
        auto victim = entries_.end();
        for (auto candidate = entries_.begin(); candidate != entries_.end(); ++candidate) {
            if (candidate->first == keep) {
                continue;
            }
            if (victim == entries_.end() || candidate->second.lastUse < victim->second.lastUse) {
                victim = candidate;
            }
        }
        if (victim == entries_.end()) {
            return;
        }
        std::filesystem::remove(directory_ / victim->first, code);
        residentBytes_ -= std::min(residentBytes_, victim->second.bytes);
        entries_.erase(victim);
    }
}

Result<StoredTile> SpectrogramTileStore::read(const ContentKey& key, std::uint64_t entry,
                                              const TileLayout& layout) const {
    if (key.isNull()) {
        return Error{ErrorCode::InvalidArgument, "no content key to read under"};
    }
    const std::string name = entryName(key, entry);

    const std::lock_guard<std::mutex> lock{mutex_};
    const auto known = entries_.find(name);
    if (known == entries_.end()) {
        return Error{ErrorCode::NotFound, "not in the store"};
    }
    const std::filesystem::path path = directory_ / name;

    std::error_code code;
    const std::uintmax_t actualSize = std::filesystem::file_size(path, code);
    if (code) {
        // Removed underneath us -- by another process's eviction, or by someone
        // clearing the directory. A miss, not a fault.
        residentBytes_ -= std::min(residentBytes_, known->second.bytes);
        entries_.erase(known);
        return Error{ErrorCode::NotFound, "no longer in the store"};
    }

    const auto discard = [&]() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        const auto gone = entries_.find(name);
        if (gone != entries_.end()) {
            residentBytes_ -= std::min(residentBytes_, gone->second.bytes);
            entries_.erase(gone);
        }
    };

    if (actualSize < kHeaderBytes) {
        discard();
        return Error{ErrorCode::CorruptData, "stored entry is shorter than its header"};
    }

    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        return Error{ErrorCode::NotFound, "could not open the stored entry"};
    }

    std::array<std::uint8_t, kHeaderBytes> raw{};
    stream.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(kHeaderBytes));
    if (stream.gcount() != static_cast<std::streamsize>(kHeaderBytes)) {
        discard();
        return Error{ErrorCode::CorruptData, "stored entry ends inside its header"};
    }

    // Order matters here. The CRC is checked before any field is believed,
    // because a field read out of a corrupt header is a number an attacker or a
    // broken disk chose, and the only safe thing to do with one is to notice it
    // is not ours.
    const EntryHeader header = decodeHeader(raw);
    if (header.magic != kMagic) {
        discard();
        return Error{ErrorCode::CorruptData, "not a spectrogram tile file"};
    }
    if (header.headerBytes != static_cast<std::uint32_t>(kHeaderBytes) ||
        header.headerCrc != crc32(raw.data(), kHeaderBytes - 4)) {
        discard();
        return Error{ErrorCode::CorruptData, "stored entry's header does not check out"};
    }
    if (header.version != kFormatVersion) {
        // A different layout of the same kind of file. Refusing is the point of
        // the field: a future version that changes what a tile holds must not
        // have its files read as though they held what this one holds.
        discard();
        return Error{ErrorCode::CorruptData, "stored entry is of another format version"};
    }
    if (!describesSameAnalysis(header, key, entry, layout)) {
        return Error{ErrorCode::NotFound, "stored entry is of another analysis"};
    }

    // Every length is checked against the file that claims it before a byte is
    // allocated for it. This is the whole of the defence against a length field
    // chosen by a corrupt file: there is no path from that number to an
    // allocation or an index without passing through here.
    if (header.binCount <= 0 || header.frameCount < 0 || header.firstFrame < 0 ||
        header.payloadBytes > kMaximumPayloadBytes) {
        discard();
        return Error{ErrorCode::CorruptData, "stored entry claims an impossible shape"};
    }
    const std::uint64_t expected =
        static_cast<std::uint64_t>(header.frameCount) * static_cast<std::uint64_t>(header.binCount);
    if (expected > kMaximumPayloadBytes || expected != header.payloadBytes ||
        static_cast<std::uint64_t>(actualSize) != entrySizeFor(header.payloadBytes)) {
        discard();
        return Error{ErrorCode::CorruptData, "stored entry's length disagrees with its shape"};
    }

    StoredTile tile;
    tile.firstFrame = header.firstFrame;
    tile.frameCount = header.frameCount;
    tile.hop = header.hop;
    tile.sourceFrames = header.sourceFrames;
    tile.binCount = header.binCount;
    tile.magnitudes.resize(static_cast<std::size_t>(header.payloadBytes));
    if (header.payloadBytes > 0) {
        stream.read(reinterpret_cast<char*>(tile.magnitudes.data()),
                    static_cast<std::streamsize>(header.payloadBytes));
        if (stream.gcount() != static_cast<std::streamsize>(header.payloadBytes)) {
            discard();
            return Error{ErrorCode::CorruptData, "stored entry is shorter than it claims"};
        }
    }
    if (crc32(tile.magnitudes.data(), tile.magnitudes.size()) != header.payloadCrc) {
        discard();
        return Error{ErrorCode::CorruptData, "stored entry's contents do not check out"};
    }

    known->second.lastUse = ++useCounter_;
    // Carry the recency across restarts too. It is only a hint: a filesystem
    // mounted with relatime or noatime still updates a modified time, but the
    // resolution is the filesystem's business, which is why the in-memory
    // counter is what actually orders eviction within a run.
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), code);
    return tile;
}

Status SpectrogramTileStore::write(const ContentKey& key, std::uint64_t entry,
                                   const TileLayout& layout, const StoredTile& tile) {
    if (key.isNull()) {
        return Error{ErrorCode::InvalidArgument, "no content key to write under"};
    }
    if (tile.binCount <= 0 || tile.frameCount < 0 || tile.firstFrame < 0) {
        return Error{ErrorCode::InvalidArgument, "a tile of that shape cannot be stored"};
    }
    const std::uint64_t expected =
        static_cast<std::uint64_t>(tile.frameCount) * static_cast<std::uint64_t>(tile.binCount);
    if (expected != static_cast<std::uint64_t>(tile.magnitudes.size())) {
        return Error{ErrorCode::InvalidArgument, "tile shape and tile bytes disagree"};
    }
    if (expected > kMaximumPayloadBytes) {
        return Error{ErrorCode::InvalidArgument, "tile is larger than this format stores"};
    }

    const std::array<std::uint8_t, kHeaderBytes> raw =
        encodeHeader(headerFor(key, entry, layout, tile));
    const std::uint64_t size = entrySizeFor(expected);
    const std::string name = entryName(key, entry);

    const std::lock_guard<std::mutex> lock{mutex_};
    if (size > budgetBytes_) {
        return Error{ErrorCode::InvalidArgument, "entry is larger than the whole cache budget"};
    }

    const auto found = entries_.find(name);
    const std::uint64_t held = found == entries_.end() ? 0 : found->second.bytes;
    // Only the difference has to be made room for, because the entry being
    // replaced is already counted. Nothing held across this call survives it:
    // eviction erases from the map.
    evictFor(size > held ? size - held : 0, name);

    const std::filesystem::path target = directory_ / name;
    const std::filesystem::path temporary = directory_ / temporaryName(name);
    {
        std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
        if (!stream) {
            return Error{ErrorCode::IoFailure, "could not write to the cache directory"};
        }
        stream.write(reinterpret_cast<const char*>(raw.data()),
                     static_cast<std::streamsize>(raw.size()));
        if (!tile.magnitudes.empty()) {
            stream.write(reinterpret_cast<const char*>(tile.magnitudes.data()),
                         static_cast<std::streamsize>(tile.magnitudes.size()));
        }
        stream.flush();
        if (!stream) {
            std::error_code ignored;
            stream.close();
            std::filesystem::remove(temporary, ignored);
            return Error{ErrorCode::IoFailure, "could not write the whole entry"};
        }
    }

    // Rename rather than write in place, so that a reader -- in this process or
    // another -- sees either the entry that was there or the entry that is now,
    // and never the half of one that had been written when the power went off.
    std::error_code code;
    std::filesystem::rename(temporary, target, code);
    if (code) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return Error{ErrorCode::IoFailure, "could not put the entry in place"};
    }

    const auto previous = entries_.find(name);
    if (previous != entries_.end()) {
        residentBytes_ -= std::min(residentBytes_, previous->second.bytes);
        entries_.erase(previous);
    }
    entries_[name] = Entry{size, ++useCounter_};
    residentBytes_ += size;
    return {};
}

std::uint64_t SpectrogramTileStore::residentBytes() const noexcept {
    const std::lock_guard<std::mutex> lock{mutex_};
    return residentBytes_;
}

std::size_t SpectrogramTileStore::entryCount() const noexcept {
    const std::lock_guard<std::mutex> lock{mutex_};
    return entries_.size();
}

std::uint64_t SpectrogramTileStore::budgetBytes() const noexcept {
    return budgetBytes_;
}

void SpectrogramTileStore::clear() noexcept {
    const std::lock_guard<std::mutex> lock{mutex_};
    std::error_code code;
    for (const auto& [name, entry] : entries_) {
        std::filesystem::remove(directory_ / name, code);
    }
    entries_.clear();
    residentBytes_ = 0;
}

} // namespace sa::spectral
