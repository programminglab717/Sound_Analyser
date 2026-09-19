#pragma once

#include <sa/core/Cancellation.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/io/AudioSource.h>
#include <sa/spectral/SpectrogramPyramid.h>

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace sa::spectral {

/// A spectrogram for a file too long to hold one of.
///
/// The eager pyramid costs about 1.3 GB per hour of stereo at the display
/// settings, which is not a cache, it is a refusal to open the file. This holds
/// two things instead:
///
///  - An **overview**, built once in a single streaming pass and always
///    resident. Its level 0 is the maximum of every 2^coarseLevel STFT frames,
///    so it costs that fraction of the full pyramid -- tens of megabytes for a
///    three-hour recording -- and because the combining is by maximum rather
///    than by sampling, a click one frame wide is still visible in it. That is
///    what makes it usable as a fallback rather than merely as a placeholder.
///
///  - **Detail tiles** at full resolution, built on demand for the range being
///    looked at and evicted least-recently-used under a byte budget.
///
/// The fine frames are computed either way: the overview pass runs the same
/// STFT the full build would. What changes is that it does not *store* the
/// expensive part. Opening a long file therefore costs what it already cost in
/// time and a small fraction of what it cost in memory.
///
/// Not a general cache. It is tied to one source, one channel and one
/// configuration, because every one of those changes every byte in it.
class SpectrogramTiles {
public:
    struct Settings {
        SpectrogramConfig config;

        /// How much the overview is decimated. 6 means an overview frame is 64
        /// STFT frames, so the overview costs a sixty-fourth of level 0 plus
        /// its own upper levels.
        int coarseLevel = 6;

        /// Level-0 frames in one detail tile. A tile is `tileFrames * bins`
        /// bytes -- about a megabyte at the display settings.
        SampleCount tileFrames = 1024;

        /// Ceiling on resident detail. The overview is not counted: it is not
        /// evictable, and a budget that could evict it would be a budget that
        /// can leave the window with nothing to draw.
        std::size_t detailBudgetBytes = std::size_t{192} << 20;
    };

    [[nodiscard]] static Result<SpectrogramTiles>
    create(std::shared_ptr<const io::AudioSource> source, int channel, Settings settings);

    /// The least decimation whose overview fits in `budgetBytes`.
    ///
    /// Zero for anything short enough that the full pyramid fits, and that case
    /// matters more than it looks: at decimation zero the overview *is* the
    /// ordinary pyramid, so a six-second file is drawn exactly as it was before
    /// any of this existed. Fixing the decimation at a constant instead made
    /// short files blocky -- the first version drew a six-second probe from
    /// nine overview frames across nine hundred columns, which the render test
    /// noticed as the picture losing more than half its distinct colours.
    [[nodiscard]] static int coarseLevelFor(SampleCount sourceFrames,
                                            const SpectrogramConfig& config,
                                            std::size_t budgetBytes) noexcept;

    SpectrogramTiles(SpectrogramTiles&&) noexcept;
    SpectrogramTiles& operator=(SpectrogramTiles&&) noexcept;
    ~SpectrogramTiles();

    /// Build the always-resident overview. One pass over the file.
    [[nodiscard]] Status buildOverview(const JobMonitor& monitor = {});

    [[nodiscard]] bool hasOverview() const noexcept { return !overview_.isEmpty(); }

    [[nodiscard]] const SpectrogramPyramid& overview() const noexcept { return overview_; }

    /// Build whatever detail tiles cover [startSample, endSample) and are not
    /// already resident, evicting least-recently-used ones to stay in budget.
    ///
    /// Tiles already resident are touched rather than rebuilt, so scrolling
    /// back and forth over the same stretch costs nothing after the first pass.
    [[nodiscard]] Status ensureDetail(SampleIndex startSample, SampleIndex endSample,
                                      const JobMonitor& monitor = {});

    /// Drop every detail tile. The overview is kept.
    void clearDetail() noexcept;

    [[nodiscard]] std::size_t residentDetailBytes() const noexcept;
    [[nodiscard]] std::size_t residentTileCount() const noexcept;

    [[nodiscard]] int binCount() const noexcept { return binCount_; }

    [[nodiscard]] SampleCount sourceFrames() const noexcept { return sourceFrames_; }

    [[nodiscard]] const Settings& settings() const noexcept { return settings_; }

    /// Level-0 frames covering the whole source.
    [[nodiscard]] SampleCount fineFrameCount() const noexcept { return fineFrameCount_; }

    /// Quantised magnitudes for one level-0 frame, or nullptr when the tile
    /// holding it is not resident.
    ///
    /// **The pointer is only valid until the next ensureDetail() or
    /// clearDetail()**, because eviction frees the tile under it. That makes
    /// this safe for a test, which owns both calls, and unsafe for the
    /// renderer, which does not -- so render() below does not use it. Reading a
    /// tile while a worker may evict it is a use-after-free, and this interface
    /// cannot prevent that; it can only say so.
    [[nodiscard]] const std::uint8_t* fineFrame(SampleCount frame) const noexcept;

    /// Analysis settings, so a view can read the decibel range without
    /// reaching through to the overview.
    [[nodiscard]] const SpectrogramConfig& config() const noexcept { return settings_.config; }

    /// Convert a stored value back to decibels, for readouts.
    [[nodiscard]] float toDecibels(std::uint8_t value) const noexcept;

    /// Render a tile onto an arbitrary frequency scale, exactly as
    /// SpectrogramPyramid::render does and with the same edge convention.
    ///
    /// Each output column is drawn from full-resolution detail where the tiles
    /// covering it are resident, and from the overview where they are not. A
    /// part-built view therefore has a visible boundary between sharp and
    /// coarse rather than being blank, which is the right way round: something
    /// correct-but-coarse beats nothing, and the boundary moves away as the
    /// tiles arrive.
    void render(SampleIndex startSample, SampleIndex endSample, const float* rowBinEdges, int rows,
                int columns, std::uint8_t* out) const noexcept;

    /// Whether a column covering this many samples wants detail at all. Above
    /// the overview's own hop there is nothing detail can add.
    [[nodiscard]] bool detailWorthwhile(SampleCount samplesPerColumn) const noexcept;

private:
    SpectrogramTiles(std::shared_ptr<const io::AudioSource> source, int channel, Settings settings,
                     int binCount, SampleCount fineFrameCount);

    struct Tile {
        SampleCount index = 0;
        SampleCount firstFrame = 0;
        SampleCount frameCount = 0;
        std::vector<std::uint8_t> magnitudes;
    };

    /// Called with the lock held.
    void touch(SampleCount tileIndex);
    void evictTo(std::size_t budget);
    [[nodiscard]] std::size_t tileBytes(const Tile& tile) const noexcept;
    [[nodiscard]] const std::uint8_t* fineFrameLocked(SampleCount frame) const noexcept;

    std::shared_ptr<const io::AudioSource> source_;
    int channel_ = 0;
    Settings settings_;
    int binCount_ = 0;
    SampleCount sourceFrames_ = 0;
    SampleCount fineFrameCount_ = 0;

    SpectrogramPyramid overview_;

    /// Guards the tile map and the LRU order only. The overview is written once
    /// by buildOverview and read-only afterwards.
    ///
    /// A mutex rather than a lock-free scheme because the access pattern is a
    /// worker adding a megabyte every few milliseconds against a reader
    /// touching it once a frame, and because this project has already spent a
    /// day on one data race in exactly this pair of threads.
    mutable std::mutex mutex_;
    std::unordered_map<SampleCount, Tile> tiles_;
    mutable std::list<SampleCount> order_; // Front is most recently used.
    std::unordered_map<SampleCount, std::list<SampleCount>::iterator> where_;
    std::size_t residentBytes_ = 0;
};

} // namespace sa::spectral
