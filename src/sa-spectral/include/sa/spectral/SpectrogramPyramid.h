#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Cancellation.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/dsp/Window.h>
#include <sa/io/AudioSource.h>

#include <cstdint>
#include <vector>

namespace sa::spectral {

/// Analysis settings for a spectrogram.
struct SpectrogramConfig {
    int fftSize = 2048;
    int hopSize = 512;
    dsp::WindowType window = dsp::WindowType::Hann;

    /// Displayed dynamic range. Magnitudes are quantised to 8 bits across this
    /// span -- roughly 0.47 dB per step over the default 120 dB, which is finer
    /// than a display can show and a third the memory of 16-bit.
    float minimumDecibels = -120.0f;
    float maximumDecibels = 0.0f;
};

/// Multi-resolution spectrogram over a single channel.
///
/// Level 0 is the STFT. Each level above halves the time resolution by taking
/// the **maximum** of adjacent frames rather than their mean, so a transient
/// present at full resolution is still present when zoomed out -- the same
/// reasoning as the peak pyramid's min/max. Mean-combining would make clicks
/// fade out exactly as the user zooms out to look for them.
///
/// Building upper levels from the level below means the STFT runs once, not
/// once per zoom level.
///
/// Magnitudes are stored as 8-bit log-magnitude, which is what the renderer
/// uploads as a texture; colour mapping and contrast happen in a shader, so
/// changing either costs no recomputation.
class SpectrogramPyramid {
public:
    SpectrogramPyramid() = default;

    /// Build over one channel of `source`.
    [[nodiscard]] static Result<SpectrogramPyramid> build(ConstAudioBufferView source, int channel,
                                                          const SpectrogramConfig& config = {});

    /// Build from a file without holding it.
    ///
    /// The one-shot form above needs the whole decoded channel resident, which
    /// for the recordings this product exists for is a second copy of something
    /// already too big. This reads in blocks and keeps one analysis window of
    /// history, so the audio costs a fixed amount however long the file is.
    ///
    /// The result is bit-identical to build() on the same audio, which is
    /// asserted rather than assumed: the framing has to match exactly, and a
    /// streaming implementation that quietly shifts every frame by a hop is a
    /// spectrogram that disagrees with the waveform beside it.
    [[nodiscard]] static Result<SpectrogramPyramid>
    buildStreaming(const io::AudioSource& source, int channel, const SpectrogramConfig& config = {},
                   const JobMonitor& monitor = {});

    [[nodiscard]] bool isEmpty() const noexcept { return levels_.empty(); }

    [[nodiscard]] int levelCount() const noexcept { return static_cast<int>(levels_.size()); }

    [[nodiscard]] int binCount() const noexcept { return binCount_; }

    [[nodiscard]] SampleCount sourceFrames() const noexcept { return sourceFrames_; }

    [[nodiscard]] const SpectrogramConfig& config() const noexcept { return config_; }

    /// Samples advanced per spectrogram frame at `level`.
    [[nodiscard]] SampleCount hopAt(int level) const noexcept;
    [[nodiscard]] SampleCount frameCountAt(int level) const noexcept;

    /// Quantised magnitude at a specific cell. Returns 0 (silence) out of range.
    [[nodiscard]] std::uint8_t magnitudeAt(int level, SampleCount frame, int bin) const noexcept;

    /// Coarsest level whose frames still fit within `samplesPerColumn`.
    [[nodiscard]] int levelForSamplesPerColumn(SampleCount samplesPerColumn) const noexcept;

    /// Render a tile.
    ///
    /// Fills `out` with `columns` x `rows` quantised magnitudes, row-major from
    /// `firstBin`, covering [startSample, endSample). Cells are combined by
    /// maximum, so nothing visible at full resolution disappears at this one.
    /// Allocation-free; out-of-range areas read as silence.
    void render(SampleIndex startSample, SampleIndex endSample, int firstBin, int rows, int columns,
                std::uint8_t* out) const noexcept;

    /// Render a tile onto an arbitrary frequency scale.
    ///
    /// `rowBinEdges` holds `rows + 1` bin positions, bottom row first and
    /// monotonically increasing; row r covers [rowBinEdges[r], rowBinEdges[r+1]).
    /// Edges are fractional. A row spanning several bins takes their maximum --
    /// the same combining rule as everywhere else in this class, so a narrow
    /// peak never disappears. A row narrower than one bin instead interpolates
    /// between its neighbours, because the alternative is drawing the stretched
    /// low end of a log axis as a stack of flat blocks.
    ///
    /// The caller owns the scale. A linear axis, a log axis and a mel axis are
    /// all just different edge tables, so the pyramid does not need to know
    /// which one is on screen.
    void render(SampleIndex startSample, SampleIndex endSample, const float* rowBinEdges, int rows,
                int columns, std::uint8_t* out) const noexcept;

    /// Pointer to `binCount()` contiguous magnitudes for one frame at `level`,
    /// or nullptr out of range.
    ///
    /// This is the renderer's real entry point: a level's frames are already
    /// laid out as a texture, so the visible range uploads directly and the
    /// shader does the scaling and colour mapping. render() below does that
    /// scaling on the CPU instead, which is useful for tests, thumbnails and
    /// headless work but is not how the UI should draw.
    [[nodiscard]] const std::uint8_t* frameData(int level, SampleCount frame) const noexcept;

    /// Convert a stored value back to decibels, for readouts and tests.
    [[nodiscard]] float toDecibels(std::uint8_t value) const noexcept;

    [[nodiscard]] std::size_t memoryFootprint() const noexcept;

private:
    /// Build every level above 0 by max-combining pairs from the one below.
    /// Both builds call it, so the two cannot drift.
    void buildUpperLevels();

    struct Level {
        SampleCount hop = 0;
        SampleCount frameCount = 0;
        std::vector<std::uint8_t> magnitudes; // frame-major, binCount per frame
    };

    SpectrogramConfig config_;
    std::vector<Level> levels_;
    int binCount_ = 0;
    SampleCount sourceFrames_ = 0;
};

} // namespace sa::spectral
