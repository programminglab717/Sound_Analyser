#include "FrameAnalysis.h"

#include <sa/spectral/SpectrogramTiles.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace sa::spectral {

SpectrogramTiles::SpectrogramTiles(std::shared_ptr<const io::AudioSource> source, int channel,
                                   Settings settings, int binCount, SampleCount fineFrameCount)
    : source_(std::move(source)), channel_(channel), settings_(settings), binCount_(binCount),
      sourceFrames_(source_->info().frameCount), fineFrameCount_(fineFrameCount) {}

SpectrogramTiles::SpectrogramTiles(SpectrogramTiles&& other) noexcept
    : source_(std::move(other.source_)), channel_(other.channel_),
      settings_(std::move(other.settings_)), binCount_(other.binCount_),
      sourceFrames_(other.sourceFrames_), fineFrameCount_(other.fineFrameCount_),
      overview_(std::move(other.overview_)), tiles_(std::move(other.tiles_)),
      order_(std::move(other.order_)), where_(std::move(other.where_)),
      residentBytes_(other.residentBytes_) {
    other.residentBytes_ = 0;
}

SpectrogramTiles& SpectrogramTiles::operator=(SpectrogramTiles&& other) noexcept {
    if (this != &other) {
        source_ = std::move(other.source_);
        channel_ = other.channel_;
        settings_ = std::move(other.settings_);
        binCount_ = other.binCount_;
        sourceFrames_ = other.sourceFrames_;
        fineFrameCount_ = other.fineFrameCount_;
        overview_ = std::move(other.overview_);
        tiles_ = std::move(other.tiles_);
        order_ = std::move(other.order_);
        where_ = std::move(other.where_);
        residentBytes_ = other.residentBytes_;
        other.residentBytes_ = 0;
    }
    return *this;
}

SpectrogramTiles::~SpectrogramTiles() = default;

Result<SpectrogramTiles> SpectrogramTiles::create(std::shared_ptr<const io::AudioSource> source,
                                                  int channel, Settings settings) {
    if (!source) {
        return Error{ErrorCode::InvalidArgument, "a tiled spectrogram needs a source"};
    }
    if (settings.coarseLevel < 0 || settings.coarseLevel > 24) {
        return Error{ErrorCode::InvalidArgument, "coarseLevel outside the supported range"};
    }
    if (settings.tileFrames <= 0) {
        return Error{ErrorCode::InvalidArgument, "a tile needs at least one frame"};
    }
    // Tile boundaries have to land on overview-frame boundaries, or a column
    // drawn partly from each would be reading two different grids.
    const SampleCount group = SampleCount{1} << settings.coarseLevel;
    if (settings.tileFrames % group != 0) {
        return Error{ErrorCode::InvalidArgument, "tileFrames must be a multiple of 2^coarseLevel"};
    }

    auto probe = detail::FrameAnalyser::create(*source, channel, settings.config);
    if (!probe) {
        return probe.error();
    }
    return SpectrogramTiles{std::move(source), channel, settings, probe.value().binCount(),
                            probe.value().frameCount()};
}

Status SpectrogramTiles::buildOverview(const JobMonitor& monitor) {
    auto built = SpectrogramPyramid::buildDecimated(*source_, channel_, settings_.config,
                                                    settings_.coarseLevel, monitor);
    if (!built) {
        return built.error();
    }
    overview_ = std::move(built).value();
    return {};
}

std::size_t SpectrogramTiles::tileBytes(const Tile& tile) const noexcept {
    return tile.magnitudes.size();
}

void SpectrogramTiles::touch(SampleCount tileIndex) {
    const auto found = where_.find(tileIndex);
    if (found == where_.end()) {
        return;
    }
    order_.splice(order_.begin(), order_, found->second);
}

void SpectrogramTiles::evictTo(std::size_t budget) {
    while (residentBytes_ > budget && !order_.empty()) {
        const SampleCount victim = order_.back();
        const auto found = tiles_.find(victim);
        if (found != tiles_.end()) {
            residentBytes_ -= tileBytes(found->second);
            tiles_.erase(found);
        }
        where_.erase(victim);
        order_.pop_back();
    }
}

Status SpectrogramTiles::ensureDetail(SampleIndex startSample, SampleIndex endSample,
                                      const JobMonitor& monitor) {
    if (fineFrameCount_ <= 0 || endSample <= startSample) {
        return {};
    }

    const SampleCount hop = settings_.config.hopSize;
    // The frames whose windows touch [startSample, endSample). A frame's window
    // begins at f*hop - padding, so a sample is covered by a span of frames
    // ending at floor(sample / hop); widening by one either side is cheaper
    // than being subtly short at the edges.
    const SampleCount first = std::max<SampleCount>(0, startSample / hop - 1);
    const SampleCount last = std::min<SampleCount>(fineFrameCount_ - 1, endSample / hop + 1);
    if (last < first) {
        return {};
    }

    const SampleCount firstTile = first / settings_.tileFrames;
    const SampleCount lastTile = last / settings_.tileFrames;
    const auto bins = static_cast<std::size_t>(binCount_);

    // What this request needs, so eviction cannot throw away a tile that is
    // about to be drawn in order to make room for another one in the same
    // request. A request larger than the budget is clamped rather than
    // thrashing: the tiles that fit are kept and the rest fall back to the
    // overview, which is the same thing the display does while they build.
    const std::size_t perTile = static_cast<std::size_t>(settings_.tileFrames) * bins;
    const auto wanted = static_cast<std::size_t>(lastTile - firstTile + 1) * perTile;

    for (SampleCount index = firstTile; index <= lastTile; ++index) {
        if (monitor.shouldCancel()) {
            return Error{ErrorCode::Cancelled, "cancelled"};
        }
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            if (tiles_.find(index) != tiles_.end()) {
                touch(index);
                continue;
            }
        }

        Tile tile;
        tile.index = index;
        tile.firstFrame = index * settings_.tileFrames;
        tile.frameCount = std::min(settings_.tileFrames, fineFrameCount_ - tile.firstFrame);
        if (tile.frameCount <= 0) {
            continue;
        }
        tile.magnitudes.assign(static_cast<std::size_t>(tile.frameCount) * bins, std::uint8_t{0});

        // One analyser per tile. Its read buffer sweeps forward through the
        // tile, so the audio under a tile is read once.
        auto made = detail::FrameAnalyser::create(*source_, channel_, settings_.config);
        if (!made) {
            return made.error();
        }
        for (SampleCount frame = 0; frame < tile.frameCount; ++frame) {
            if (monitor.shouldCancel()) {
                return Error{ErrorCode::Cancelled, "cancelled"};
            }
            const Status status = made.value().analyse(tile.firstFrame + frame,
                                                       tile.magnitudes.data() +
                                                           static_cast<std::size_t>(frame) * bins);
            if (!status) {
                return status;
            }
        }

        {
            const std::lock_guard<std::mutex> lock{mutex_};
            const std::size_t bytes = tileBytes(tile);
            // Evict before inserting, and keep room for what this request still
            // needs where the budget allows it.
            const std::size_t reserve = std::min(wanted, settings_.detailBudgetBytes);
            evictTo(settings_.detailBudgetBytes > reserve ? settings_.detailBudgetBytes - bytes
                                                          : settings_.detailBudgetBytes);
            tiles_.emplace(index, std::move(tile));
            order_.push_front(index);
            where_[index] = order_.begin();
            residentBytes_ += bytes;
            evictTo(settings_.detailBudgetBytes);
        }
        monitor.report(static_cast<double>(index - firstTile + 1) /
                       static_cast<double>(lastTile - firstTile + 1));
    }
    return {};
}

void SpectrogramTiles::clearDetail() noexcept {
    const std::lock_guard<std::mutex> lock{mutex_};
    tiles_.clear();
    order_.clear();
    where_.clear();
    residentBytes_ = 0;
}

std::size_t SpectrogramTiles::residentDetailBytes() const noexcept {
    const std::lock_guard<std::mutex> lock{mutex_};
    return residentBytes_;
}

std::size_t SpectrogramTiles::residentTileCount() const noexcept {
    const std::lock_guard<std::mutex> lock{mutex_};
    return tiles_.size();
}

const std::uint8_t* SpectrogramTiles::fineFrame(SampleCount frame) const noexcept {
    const std::lock_guard<std::mutex> lock{mutex_};
    return fineFrameLocked(frame);
}

float SpectrogramTiles::toDecibels(std::uint8_t value) const noexcept {
    const float span = settings_.config.maximumDecibels - settings_.config.minimumDecibels;
    return settings_.config.minimumDecibels + span * static_cast<float>(value) / 255.0f;
}

const std::uint8_t* SpectrogramTiles::fineFrameLocked(SampleCount frame) const noexcept {
    if (frame < 0 || frame >= fineFrameCount_) {
        return nullptr;
    }
    const SampleCount index = frame / settings_.tileFrames;
    const auto found = tiles_.find(index);
    if (found == tiles_.end()) {
        return nullptr;
    }
    const SampleCount offset = frame - found->second.firstFrame;
    if (offset < 0 || offset >= found->second.frameCount) {
        return nullptr;
    }
    return found->second.magnitudes.data() +
           static_cast<std::size_t>(offset) * static_cast<std::size_t>(binCount_);
}

bool SpectrogramTiles::detailWorthwhile(SampleCount samplesPerColumn) const noexcept {
    const SampleCount overviewHop =
        settings_.config.hopSize * (SampleCount{1} << settings_.coarseLevel);
    return samplesPerColumn < overviewHop;
}

void SpectrogramTiles::render(SampleIndex startSample, SampleIndex endSample,
                              const float* rowBinEdges, int rows, int columns,
                              std::uint8_t* out) const noexcept {
    if (rows <= 0 || columns <= 0 || out == nullptr || rowBinEdges == nullptr) {
        return;
    }
    std::fill_n(out, static_cast<std::size_t>(rows) * static_cast<std::size_t>(columns),
                std::uint8_t{0});
    if (endSample <= startSample) {
        return;
    }

    const auto span = static_cast<double>(endSample - startSample);
    const auto samplesPerColumn = static_cast<SampleCount>(span / columns);
    const bool wantDetail = detailWorthwhile(std::max<SampleCount>(1, samplesPerColumn));

    // One lock for the whole picture, not one per frame lookup.
    //
    // Correctness first: holding it throughout is what makes a tile pointer
    // safe to dereference at all. Taking and releasing it per frame left a
    // window in which a worker could evict the tile between the check that it
    // was resident and the read of it -- a use-after-free, and the kind that
    // appears once a fortnight under scrolling. It is also faster, and the set
    // of resident tiles cannot change mid-render, so the picture is of one
    // moment rather than of several.
    const std::lock_guard<std::mutex> lock{mutex_};

    // Column by column, because whether detail is available varies along the
    // picture: the part that has been scrolled over is sharp and the part just
    // scrolled into is not, and drawing the whole thing at the coarser of the
    // two would throw away work already done.
    std::vector<std::uint8_t> scratch(static_cast<std::size_t>(rows));
    for (int column = 0; column < columns; ++column) {
        const auto from = startSample + static_cast<SampleIndex>(span * column / columns);
        const auto to = startSample + static_cast<SampleIndex>(span * (column + 1) / columns);

        bool drawn = false;
        if (wantDetail) {
            const SampleCount hop = settings_.config.hopSize;
            const SampleCount firstFrame = std::max<SampleCount>(0, from / hop);
            const SampleCount lastFrame = std::min<SampleCount>(
                fineFrameCount_ - 1, std::max<SampleCount>(firstFrame, (to - 1) / hop));

            // Every frame this column needs has to be resident, or the column
            // would be drawn from a subset of its own span and read as a dip.
            bool complete = fineFrameCount_ > 0;
            for (SampleCount frame = firstFrame; complete && frame <= lastFrame; ++frame) {
                complete = fineFrameLocked(frame) != nullptr;
            }

            if (complete) {
                std::fill(scratch.begin(), scratch.end(), std::uint8_t{0});
                for (SampleCount frame = firstFrame; frame <= lastFrame; ++frame) {
                    const std::uint8_t* magnitudes = fineFrameLocked(frame);
                    if (magnitudes == nullptr) {
                        continue;
                    }
                    for (int row = 0; row < rows; ++row) {
                        const float low = rowBinEdges[row];
                        const float high = rowBinEdges[row + 1];
                        const int firstBin =
                            std::clamp(static_cast<int>(std::floor(low)), 0, binCount_ - 1);
                        const int lastBin = std::clamp(static_cast<int>(std::ceil(high)) - 1,
                                                       firstBin, binCount_ - 1);
                        std::uint8_t loudest = scratch[static_cast<std::size_t>(row)];
                        for (int bin = firstBin; bin <= lastBin; ++bin) {
                            loudest = std::max(loudest, magnitudes[bin]);
                        }
                        scratch[static_cast<std::size_t>(row)] = loudest;
                    }
                }
                for (int row = 0; row < rows; ++row) {
                    out[static_cast<std::size_t>(row) * static_cast<std::size_t>(columns) +
                        static_cast<std::size_t>(column)] = scratch[static_cast<std::size_t>(row)];
                }
                drawn = true;
            }
        }

        if (!drawn && !overview_.isEmpty()) {
            // One column of the overview, through the pyramid's own renderer so
            // the two cannot disagree about the edge convention.
            std::vector<std::uint8_t> one(static_cast<std::size_t>(rows));
            overview_.render(from, to, rowBinEdges, rows, 1, one.data());
            for (int row = 0; row < rows; ++row) {
                out[static_cast<std::size_t>(row) * static_cast<std::size_t>(columns) +
                    static_cast<std::size_t>(column)] = one[static_cast<std::size_t>(row)];
            }
        }
    }
}

} // namespace sa::spectral
