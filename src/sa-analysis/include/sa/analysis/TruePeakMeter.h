#pragma once

#include <sa/analysis/Decibels.h>
#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>

#include <vector>

namespace sa::analysis {

/// Inter-sample peak meter, ITU-R BS.1770-4 Annex 2, reported in dBTP.
///
/// Sample peak is not what a converter or a lossy encoder sees. Reconstructing
/// the signal between samples routinely finds another 1-3 dB, and a master that
/// measures 0.0 dBFS can clip a D/A stage or an MP3 decoder hard. This meter
/// finds those peaks by polyphase interpolation.
///
/// No sample rate is needed: the interpolator works in normalised frequency, so
/// the same coefficients are correct at every rate. What the rate does decide is
/// how much oversampling BS.1770-4 asks for -- it wants an effective rate of at
/// least 192 kHz, which 4x delivers from 48 kHz upward.
///
/// **Conformance caveat.** BS.1770-4 Annex 2 specifies a particular 48-tap
/// polyphase FIR (12 taps per phase, Table 3). That table is not reproduced
/// here -- transcribing it from memory would be worse than not using it -- so
/// this uses a Blackman-Harris windowed sinc of kTapsPerPhase taps per phase
/// instead. It is a longer and flatter filter than the standard's, which means
/// readings may come out marginally *higher* than a reference implementation
/// rather than lower. Verified here: phase 0 reproduces input samples exactly,
/// every phase has unity DC gain, and a peak falling exactly between samples is
/// recovered to within 0.001 dB. Not verified: agreement with the standard's
/// own filter on real programme material.
class TruePeakMeter {
public:
    /// BS.1770-4's minimum at 48 kHz, and the default everywhere.
    static constexpr int kDefaultOversampling = 4;
    static constexpr int kMinimumOversampling = 4;
    static constexpr int kMaximumOversampling = 16;

    /// Taps per polyphase branch. Odd, because it makes the prototype length
    /// odd too, which puts the centre tap on a whole input sample: phase 0 then
    /// becomes an exact unit impulse and the interpolated points land on exact
    /// 1/oversampling boundaries. An even-length prototype offsets the whole
    /// grid by half an output sample and quietly misses peaks.
    static constexpr int kTapsPerPhase = 25;

    [[nodiscard]] static Result<TruePeakMeter> create(int channelCount,
                                                      int oversampling = kDefaultOversampling);

    /// One-shot true peak of a whole buffer, in dBTP.
    [[nodiscard]] static Result<double> measureDbtp(ConstAudioBufferView audio,
                                                    int oversampling = kDefaultOversampling);

    /// Feed one block. Allocation-free, audio-thread safe. A block whose
    /// channel count differs from the configured one is ignored, as in
    /// LoudnessMeter::process.
    void process(ConstAudioBufferView block) noexcept;

    void reset() noexcept;

    /// Highest inter-sample magnitude seen on any channel, linear.
    [[nodiscard]] double truePeak() const noexcept;

    [[nodiscard]] double truePeakDbtp() const noexcept { return amplitudeToDecibels(truePeak()); }

    [[nodiscard]] double channelTruePeak(int channel) const noexcept;

    [[nodiscard]] double channelTruePeakDbtp(int channel) const noexcept {
        return amplitudeToDecibels(channelTruePeak(channel));
    }

    [[nodiscard]] int channelCount() const noexcept { return channelCount_; }

    [[nodiscard]] int oversampling() const noexcept { return oversampling_; }

    [[nodiscard]] SampleCount framesProcessed() const noexcept { return framesProcessed_; }

private:
    TruePeakMeter(int channelCount, int oversampling);

    int channelCount_ = 0;
    int oversampling_ = 0;
    /// Polyphase coefficients, phase-major: phase p tap k at p * kTapsPerPhase + k.
    std::vector<double> phases_;
    /// Per-channel delay line, each stored twice back to back so that the most
    /// recent kTapsPerPhase samples are always contiguous and the inner loop
    /// needs no modulo.
    std::vector<double> delay_;
    std::vector<double> peaks_;
    int writeIndex_ = 0;
    SampleCount framesProcessed_ = 0;
};

} // namespace sa::analysis
