#pragma once

#include <sa/analysis/Decibels.h>
#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>

#include <vector>

namespace sa::analysis {

/// Inter-sample peak meter, reported in dBTP. Follows the ITU-R BS.1770-4
/// Annex 2 method but **not** its exact filter -- see the conformance caveat
/// below before quoting a reading as a standards measurement.
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
/// **Conformance caveat, and the direction of the error.** BS.1770-4 Annex 2
/// specifies a particular 48-tap polyphase FIR (12 taps per phase, Table 3).
/// That table is not reproduced here -- transcribing it from memory would be
/// worse than not using it -- so this uses a Blackman-Harris windowed sinc of
/// kTapsPerPhase taps per phase instead.
///
/// **This meter under-reads, and by how much depends on frequency.** Measured
/// against signals whose true peak is known by construction -- built at 16x,
/// band-limited, decimated, so the fine waveform *is* the reconstruction and
/// its maximum is the answer:
///
///     tone at      error at 4x   error at 16x
///     0.20 rate      -0.02 dB      -0.00 dB
///     0.30 rate      -0.04 dB      -0.00 dB
///     0.40 rate      -0.44 dB      -0.03 dB
///     0.45 rate      -0.12 dB      -0.10 dB
///     0.47 rate      -0.20 dB      -0.20 dB
///
/// An earlier version of this comment said readings "may come out marginally
/// higher than a reference implementation rather than lower". That was wrong,
/// and wrong in the dangerous direction: a true-peak meter that reads low tells
/// an engineer they are under the ceiling when they are over it. The error was
/// not visible in the tests because a steady tone hides it -- every raw sample
/// is also a peak candidate, so when the tone eventually lands near its own
/// crest the raw value covers for the interpolator's droop. It shows on short
/// transients near Nyquist, where it cannot.
///
/// **Where the audio is already in memory, do not accept this.** Use
/// exactTruePeakDbtp() below, which reconstructs the signal instead of
/// interpolating it and has no filter to droop. The metering panel and sa-cli
/// both do, so the number a user is shown for a file is exact; this meter is
/// what a live display would use, where the answer has to arrive in a block and
/// cannot cost a transform.
///
/// For that live case: budget around half a dB of headroom at 4x if the
/// material has energy above 0.4 of the sample rate, or use 16x, which costs
/// four times the work and halves the worst case. Matching the standard exactly
/// still needs its own filter table, which is tracked in docs/04-roadmap.md.
///
/// Verified here: phase 0 reproduces input samples exactly, every phase has
/// unity DC gain, and a peak falling exactly between samples is recovered to
/// within 0.001 dB. Not verified: agreement with the standard's own filter on
/// real programme material.
/// True peak of a whole buffer, exactly, for offline work.
///
/// TruePeakMeter is a streaming, allocation-free, audio-thread-safe meter, and
/// that is what makes it approximate: a real-time interpolator is a finite
/// filter, and a finite filter droops. Ours reads up to 0.44 dB low at 4x on
/// bright transients.
///
/// When the audio is already in memory and nothing is waiting on the answer --
/// a metering panel, a batch report, a compliance check before a file ships --
/// there is no reason to accept that. This reconstructs the signal exactly
/// instead, and is the reading to quote when it matters. It is not usable in a
/// live meter: it allocates and transforms the whole buffer.
///
/// The worst channel wins, which is what "the true peak of this programme"
/// means.
[[nodiscard]] Result<double> exactTruePeakDbtp(ConstAudioBufferView audio, int factor = 16);

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
