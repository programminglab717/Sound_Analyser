#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Cancellation.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/io/AudioSource.h>

#include <cstddef>
#include <vector>

namespace sa::io {

/// Min/max/RMS summary of one bin of samples.
///
/// Min and max are kept separately rather than a single peak magnitude because
/// asymmetric waveforms -- which is most real audio -- draw wrongly otherwise,
/// and because DC offset becomes invisible the moment you collapse them.
struct PeakFrame {
    float minimum = 0.0f;
    float maximum = 0.0f;
    float rms = 0.0f;
};

/// Multi-resolution min/max/RMS pyramid over a block of audio.
///
/// Drawing a waveform must never touch a decoder or re-scan the source, or
/// scrolling a long file stalls. The pyramid answers "what does this sample
/// range look like at this pixel width" as a bounded read from the nearest
/// resolution level, so cost depends on the pixel width of the view, not the
/// duration of the file.
///
/// Level 0 summarises `baseBinSize` samples per frame; each level above halves
/// the resolution. Total storage is bounded by 2x level 0 (a geometric series),
/// so a 2-hour 48 kHz stereo file at the default bin size costs roughly 64 MB.
///
/// This builds synchronously from an in-memory view. Background construction
/// with the visible region prioritised, and on-disk persistence keyed by
/// content hash, layer on top -- see docs/03-architecture.md §4.
class PeakPyramid {
public:
    static constexpr SampleCount kDefaultBaseBinSize = 256;

    /// Smallest level frame count worth keeping. Below this a caller is better
    /// served reading the level directly than descending further.
    static constexpr SampleCount kMinimumTopLevelFrames = 2;

    PeakPyramid() = default;

    /// Build a pyramid over an in-memory buffer. `baseBinSize` must be a power
    /// of two of at least 2 -- halving resolution per level relies on it.
    [[nodiscard]] static Result<PeakPyramid> build(ConstAudioBufferView source,
                                                   SampleCount baseBinSize = kDefaultBaseBinSize);

    /// Build by streaming from an AudioSource, never holding more than one
    /// block of audio.
    ///
    /// This is the production path: a two-hour 96 kHz file is several gigabytes
    /// of samples but only tens of megabytes of pyramid, so requiring the
    /// source in memory first would defeat the point. Reports progress and
    /// honours cancellation, since a user who opened the wrong file should not
    /// have to wait for it.
    [[nodiscard]] static Result<PeakPyramid>
    buildStreaming(const AudioSource& source, SampleCount baseBinSize = kDefaultBaseBinSize,
                   const JobMonitor& monitor = {});

    [[nodiscard]] bool isEmpty() const noexcept { return levels_.empty(); }

    [[nodiscard]] int levelCount() const noexcept { return static_cast<int>(levels_.size()); }

    [[nodiscard]] int channelCount() const noexcept { return channelCount_; }

    [[nodiscard]] SampleCount sourceFrames() const noexcept { return sourceFrames_; }

    [[nodiscard]] SampleCount baseBinSize() const noexcept { return baseBinSize_; }

    /// Samples summarised by one frame at `level`.
    [[nodiscard]] SampleCount binSizeAt(int level) const noexcept;

    /// Number of frames per channel at `level`.
    [[nodiscard]] SampleCount frameCountAt(int level) const noexcept;

    /// Frames actually summarised by frame `index` at `level`. The final frame
    /// of a level is usually partial, and treating it as full skews its RMS.
    [[nodiscard]] SampleCount samplesInFrame(int level, SampleCount index) const noexcept;

    [[nodiscard]] const PeakFrame& frameAt(int level, int channel,
                                           SampleCount index) const noexcept;

    /// Coarsest level whose bins still fit within `samplesPerPixel`, so that
    /// each output pixel aggregates at least one whole frame.
    [[nodiscard]] int levelForSamplesPerPixel(SampleCount samplesPerPixel) const noexcept;

    /// True when the requested zoom is finer than level 0 and the caller should
    /// read source samples directly instead. The pyramid does not retain the
    /// source, so it cannot answer honestly below its base resolution.
    [[nodiscard]] bool shouldReadSource(SampleCount samplesPerPixel) const noexcept {
        return samplesPerPixel < baseBinSize_;
    }

    /// Fill `out` with `outFrames` summaries spanning [startSample, endSample).
    ///
    /// This is the drawing entry point: one output frame per pixel column.
    /// Allocation-free. Ranges outside the source yield zeroed frames rather
    /// than an error, so a view scrolled past the end still draws.
    void query(int channel, SampleIndex startSample, SampleIndex endSample, PeakFrame* out,
               int outFrames) const noexcept;

    /// Total bytes held by the level data.
    [[nodiscard]] std::size_t memoryFootprint() const noexcept;

private:
    /// One resolution level. Frames are channel-major: all frames of channel 0,
    /// then channel 1, and so on, because drawing reads one channel at a time.
    struct Level {
        SampleCount binSize = 0;
        SampleCount frameCount = 0;
        std::vector<PeakFrame> frames;
    };

    [[nodiscard]] PeakFrame aggregate(int level, int channel, SampleIndex startSample,
                                      SampleIndex endSample) const noexcept;

    /// Fold level 0 upward. Shared by both build paths so the two cannot drift.
    void buildUpperLevels();

    std::vector<Level> levels_;
    int channelCount_ = 0;
    SampleCount sourceFrames_ = 0;
    SampleCount baseBinSize_ = kDefaultBaseBinSize;
};

} // namespace sa::io
