#pragma once

#include <sa/analysis/Decibels.h>
#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>

#include <vector>

namespace sa::analysis {

/// The plain, ungated numbers a level panel shows, alongside the gated ones.
struct SignalStatistics {
    /// Largest sample magnitude on any channel, linear.
    double samplePeak = 0.0;
    double samplePeakDbfs = kDecibelFloor;
    /// Root mean square over every channel and every sample.
    double rms = 0.0;
    double rmsDbfs = kDecibelFloor;
    /// Mean sample value of whichever channel is furthest from zero, sign kept.
    /// Averaging across channels instead would let +0.1 on left and -0.1 on
    /// right cancel to a clean zero, hiding precisely the fault being looked for.
    double dcOffset = 0.0;
    /// Sample peak over RMS. A measure of how much transient is left: a heavily
    /// limited master sits near 6 dB, an unprocessed acoustic recording near 20.
    double crestFactorDb = 0.0;
    /// True peak over gated loudness -- the headroom that survives loudness
    /// normalisation, and the number that decides whether a platform will turn
    /// a master down and leave it flat. Left at the floor until a loudness
    /// measurement is supplied, since this meter does not gate.
    double peakToLoudnessRatioDb = kDecibelFloor;
    SampleCount frames = 0;
    int channels = 0;
};

/// Peak-to-loudness ratio, in dB. Both arguments are already logarithmic, so
/// this is a subtraction -- it exists so the sign convention is written down
/// once instead of at every call site.
[[nodiscard]] double peakToLoudnessRatioDb(double truePeakDbtp, double integratedLufs) noexcept;

/// Streaming accumulator for the ungated statistics.
///
/// process() is allocation-free and audio-thread safe, and feeding the same
/// audio in one call or a hundred gives bit-identical results.
class SignalStatisticsMeter {
public:
    [[nodiscard]] static Result<SignalStatisticsMeter> create(int channelCount);

    /// One-shot statistics for a whole buffer. PLR is filled in only if a true
    /// peak and integrated loudness are supplied.
    [[nodiscard]] static Result<SignalStatistics> measure(ConstAudioBufferView audio,
                                                          double truePeakDbtp = kDecibelFloor,
                                                          double integratedLufs = kDecibelFloor);

    /// Feed one block. A block whose channel count differs from the configured
    /// one is ignored, as in LoudnessMeter::process.
    void process(ConstAudioBufferView block) noexcept;

    void reset() noexcept;

    /// Statistics for everything fed so far. Supply a true peak and integrated
    /// loudness to have PLR computed; leave them out and it stays at the floor.
    [[nodiscard]] SignalStatistics statistics(double truePeakDbtp = kDecibelFloor,
                                              double integratedLufs = kDecibelFloor) const noexcept;

    [[nodiscard]] int channelCount() const noexcept { return channelCount_; }

    [[nodiscard]] SampleCount framesProcessed() const noexcept { return frames_; }

private:
    explicit SignalStatisticsMeter(int channelCount);

    int channelCount_ = 0;
    std::vector<double> peaks_;
    std::vector<double> sumsOfSquares_;
    std::vector<double> sums_;
    SampleCount frames_ = 0;
};

} // namespace sa::analysis
