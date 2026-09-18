#pragma once

#include <sa/analysis/Decibels.h>
#include <sa/analysis/KWeighting.h>
#include <sa/core/AudioBuffer.h>
#include <sa/core/ChannelLayout.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>

#include <cstdint>
#include <vector>

namespace sa::analysis {

/// One complete loudness read-out. LUFS throughout, except the range, which is
/// a difference and therefore LU.
struct LoudnessMeasurement {
    /// Loudness of the most recent 400 ms block.
    double momentaryLufs = kDecibelFloor;
    /// Loudness of the most recent 3 s window.
    double shortTermLufs = kDecibelFloor;
    /// Gated loudness of everything fed so far.
    double integratedLufs = kDecibelFloor;
    double maximumMomentaryLufs = kDecibelFloor;
    double maximumShortTermLufs = kDecibelFloor;
    /// 10th-to-95th percentile spread of the short-term values, EBU Tech 3342.
    double loudnessRangeLu = 0.0;
    SampleCount framesProcessed = 0;
    /// 400 ms blocks that cleared the absolute gate. Zero means the integrated
    /// value is the floor because nothing measurable was fed -- material
    /// shorter than one block, or silence -- rather than because it is quiet.
    std::int64_t gatedBlockCount = 0;
    /// 3 s windows that fed the loudness range.
    std::int64_t shortTermBlockCount = 0;
};

/// ITU-R BS.1770-4 / EBU R128 loudness meter.
///
/// Works in both directions: measure() takes a whole buffer, or create() a
/// meter and feed it blocks. The two produce bit-identical results, because
/// every accumulator is advanced in sample order regardless of how the input is
/// chopped up.
///
/// process() is allocation-free and safe on the audio thread. That is what
/// forces the gating design: BS.1770 gating is defined over the *whole* block
/// history, which a live meter cannot retain, so blocks are folded into a fixed
/// histogram instead (see the private section for what that costs).
///
/// **Conformance caveat.** This implementation has not been run against the
/// official EBU R128 / ITU-R BS.1770 test vector set -- those files are not in
/// the repository. What is verified is the algebra: the K-weighting derivation
/// against the coefficients printed in BS.1770-4, and the gating, channel
/// weighting and summation against invariants that hold for any correct
/// implementation (see LoudnessMeterTests.cpp). Treat "conformant" as a claim
/// still to be earned by the CI job that adds the real vectors
/// (docs/03-architecture.md §9).
class LoudnessMeter {
public:
    /// Sub-block the whole measurement is built from. 400 ms blocks advance one
    /// sub-block at a time, which is BS.1770's 75% overlap.
    static constexpr int kSubBlocksPerBlock = 4;
    /// 3 s of sub-blocks.
    static constexpr int kSubBlocksPerShortTerm = 30;
    /// Loudness range samples short-term loudness once a second, the 2/3
    /// overlap EBU Tech 3342 asks for. The displayed short-term value updates
    /// every sub-block regardless, because a meter that moves once a second
    /// looks broken.
    static constexpr int kShortTermHopSubBlocks = 10;
    static constexpr double kSubBlockSeconds = 0.1;

    static constexpr double kAbsoluteGateLufs = -70.0;
    static constexpr double kRelativeGateLu = -10.0;
    /// The loudness range uses its own, wider relative gate.
    static constexpr double kRangeGateLu = -20.0;
    static constexpr double kRangeLowPercentile = 0.10;
    static constexpr double kRangeHighPercentile = 0.95;

    /// The offset in BS.1770's loudness equation. It exists to cancel the
    /// K-weighting's gain at 1 kHz, so that a 1 kHz tone reads its own dBFS
    /// value -- which is why the filter and this constant must not be changed
    /// independently of each other.
    static constexpr double kLoudnessOffsetDb = -0.691;

    /// BS.1770-4 Table 3. Surround channels are weighted up because sound
    /// arriving from behind is heard as louder than the same sound in front.
    static constexpr double kSurroundWeight = 1.41;

    /// Meter for `rate` and `layout`.
    ///
    /// Fails for an unusable rate, an empty or oversized layout, or a layout
    /// with nothing measurable in it (LFE only).
    [[nodiscard]] static Result<LoudnessMeter> create(SampleRate rate, const ChannelLayout& layout);

    /// One-shot measurement of a whole buffer.
    [[nodiscard]] static Result<LoudnessMeasurement>
    measure(ConstAudioBufferView audio, SampleRate rate, const ChannelLayout& layout);

    /// Feed one block. Allocation-free, lock-free, audio-thread safe.
    ///
    /// A block whose channel count differs from the configured layout is
    /// ignored rather than partly consumed: half-processed audio would corrupt
    /// the integration silently, and the audio thread has no way to report an
    /// error. framesProcessed() lets the caller notice.
    void process(ConstAudioBufferView block) noexcept;

    /// Discard all history and filter state. Allocation-free.
    void reset() noexcept;

    [[nodiscard]] double momentaryLufs() const noexcept { return momentaryLufs_; }

    [[nodiscard]] double shortTermLufs() const noexcept { return shortTermLufs_; }

    [[nodiscard]] double maximumMomentaryLufs() const noexcept { return maximumMomentaryLufs_; }

    [[nodiscard]] double maximumShortTermLufs() const noexcept { return maximumShortTermLufs_; }

    /// Two-stage gated loudness of everything fed so far. Computed on demand
    /// from the histogram; allocation-free, so a UI meter may poll it.
    [[nodiscard]] double integratedLufs() const noexcept;

    /// EBU Tech 3342 loudness range, in LU. Zero when there is not yet anything
    /// to spread.
    [[nodiscard]] double loudnessRangeLu() const noexcept;

    [[nodiscard]] LoudnessMeasurement measurement() const noexcept;

    [[nodiscard]] SampleRate sampleRate() const noexcept { return rate_; }

    [[nodiscard]] int channelCount() const noexcept { return channelCount_; }

    /// BS.1770 weight G for `channel`; 0 for LFE and out-of-range indices.
    [[nodiscard]] double channelWeight(int channel) const noexcept;

    [[nodiscard]] SampleCount framesProcessed() const noexcept { return framesProcessed_; }

    /// Samples in one 100 ms sub-block at this rate.
    [[nodiscard]] SampleCount samplesPerSubBlock() const noexcept { return samplesPerSubBlock_; }

    /// BS.1770 weight for a speaker position, as a free-standing rule so the
    /// table can be inspected and tested without building a meter.
    [[nodiscard]] static double weightFor(Speaker speaker) noexcept;

private:
    /// Gating needs every block's energy, and a live meter runs unbounded, so
    /// blocks are folded into a fixed histogram rather than a growing list.
    ///
    /// Each bin keeps an *exact* sum of the energies that landed in it, so the
    /// only thing quantised is which side of the relative threshold the one
    /// straddling bin falls on. At 0.1 LU bins that is at most one bin's worth
    /// of energy out of the whole programme -- far inside EBU Tech 3341's
    /// +/-0.1 LU tolerance, and the same trade libebur128 makes.
    struct HistogramBin {
        std::int64_t count = 0;
        double energy = 0.0;
    };

    /// Blocks at or below the absolute gate never enter, so the histogram can
    /// start there. The top end is well above anything digital audio reaches
    /// once channel weights are summed; louder blocks clamp into the last bin
    /// and so are never *excluded* by a gate, only imprecisely placed.
    static constexpr double kHistogramMinimumLufs = kAbsoluteGateLufs;
    static constexpr double kHistogramBinWidthLu = 0.1;
    static constexpr int kHistogramBinCount = 1000;

    struct Histogram {
        std::vector<HistogramBin> bins;
        double totalEnergy = 0.0;
        std::int64_t totalCount = 0;

        void allocate();
        void clear() noexcept;
        void add(double loudness, double energy) noexcept;
        /// Mean energy of everything added, or 0 when nothing was.
        [[nodiscard]] double meanEnergy() const noexcept;
        /// Mean energy of the bins sitting above `threshold` LUFS.
        [[nodiscard]] double meanEnergyAbove(double threshold) const noexcept;
        [[nodiscard]] std::int64_t countAbove(double threshold) const noexcept;
        /// Loudness at `fraction` through the values above `threshold`.
        [[nodiscard]] double percentileAbove(double threshold, double fraction) const noexcept;
    };

    LoudnessMeter(SampleRate rate, const ChannelLayout& layout,
                  const KWeightingCoefficients& coefficients);

    /// Loudness of a weighted mean-square energy, floored rather than -inf.
    [[nodiscard]] static double loudnessFromEnergy(double energy) noexcept;

    /// Weighted mean square over the most recent `subBlockCount` sub-blocks.
    [[nodiscard]] double windowEnergy(int subBlockCount) const noexcept;

    void completeSubBlock() noexcept;

    SampleRate rate_;
    int channelCount_ = 0;
    SampleCount samplesPerSubBlock_ = 0;
    KWeightingCoefficients coefficients_;

    std::vector<double> weights_;
    std::vector<BiquadState> shelfState_;
    std::vector<BiquadState> highPassState_;
    /// Sum of squared K-weighted samples so far in the sub-block being filled.
    std::vector<double> subBlockSums_;
    /// Ring of the last kSubBlocksPerShortTerm sub-block sums, sub-block-major.
    /// Windows are re-summed from it rather than maintained incrementally, so
    /// no rounding error accumulates across a long programme.
    std::vector<double> history_;

    SampleCount subBlockFill_ = 0;
    std::int64_t completedSubBlocks_ = 0;
    SampleCount framesProcessed_ = 0;

    double momentaryLufs_ = kDecibelFloor;
    double shortTermLufs_ = kDecibelFloor;
    double maximumMomentaryLufs_ = kDecibelFloor;
    double maximumShortTermLufs_ = kDecibelFloor;

    Histogram blocks_;
    Histogram shortTermBlocks_;
};

} // namespace sa::analysis
