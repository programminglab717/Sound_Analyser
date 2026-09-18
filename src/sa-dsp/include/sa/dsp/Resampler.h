#pragma once

#include <sa/core/Result.h>
#include <sa/core/Types.h>

#include <cstdint>
#include <vector>

/// Sample rate conversion.
///
/// One polyphase windowed-sinc interpolator, which is what answers all three of
/// the demands this product makes of a converter at once:
///
///   * **Any ratio.** Import converts 44.1 to 48 and back, which is rational;
///     varispeed and drift correction are not. Both are the same arithmetic
///     here -- the filter is stored as a finely sampled prototype rather than
///     as a fixed set of phases, so an output sample may land anywhere between
///     two input samples and still find a coefficient.
///   * **Streaming.** The engine hands the converter whatever block size the
///     device gives it. State carries across calls, so a file converted in one
///     call and the same file converted in blocks of 37 produce identical
///     output.
///   * **Real-time safety.** Every table and buffer is allocated in create().
///     process() touches nothing but its arguments and the members.
///
/// The filter is a Kaiser-windowed sinc. Kaiser because beta buys stopband
/// attenuation continuously, so a quality preset can be designed to the number
/// it needs rather than to whatever a fixed window shape happens to give.
namespace sa::dsp {

/// How much filter a conversion spends. Fast holds aliases at the noise floor
/// of 16-bit delivery and Best a further 45 dB below it; the other difference is
/// where the passband has to stop. Every figure quoted here is measured, and the
/// measurements are in ResamplerTests.
enum class ResamplerQuality {
    /// 48 taps and a 24 KB table. Cheap enough for a preview or a scrub, and
    /// honest enough to master through at a pinch.
    Fast,
    /// 128 taps and a 256 KB table. What an export should use. The table is per
    /// instance and per channel, which is worth knowing before building sixty
    /// of them.
    Best,
};

struct ResamplerSpec {
    SampleRate inputRate = kSampleRate48000;
    SampleRate outputRate = kSampleRate48000;
    ResamplerQuality quality = ResamplerQuality::Best;
    /// The most extreme downsampling this instance will later be asked to
    /// switch to, as an output-per-input ratio. Downsampling stretches the
    /// filter, so it decides how long the history has to be -- and history is
    /// allocated once, in create(). Zero means "never more than the ratio the
    /// spec was built with", which is what a fixed conversion wants.
    double lowestRatio = 0.0;
};

/// What one streaming call managed to do. Neither number is the whole of what
/// was offered: the converter stops as soon as either the input runs out or the
/// output buffer fills, so a caller loops until the input is consumed.
struct ResamplerProgress {
    SampleCount inputConsumed = 0;
    SampleCount outputProduced = 0;
};

class Resampler {
public:
    using Quality = ResamplerQuality;
    using Spec = ResamplerSpec;
    using Progress = ResamplerProgress;

    /// Ratio limits. The bound exists because the history buffer is sized from
    /// the ratio: 1/128 is already a 16384-tap filter per output sample, which
    /// is far past any rate pair that exists.
    static constexpr double kLowestRatio = 1.0 / 128.0;
    static constexpr double kHighestRatio = 128.0;

    [[nodiscard]] static Result<Resampler> create(const ResamplerSpec& spec = {});

    [[nodiscard]] ResamplerQuality quality() const noexcept { return quality_; }

    /// Output samples per input sample.
    [[nodiscard]] double ratio() const noexcept { return ratio_; }

    /// Retunes the conversion without clearing the history, so a ratio can be
    /// swept under a running signal. Allocation-free, and therefore audio-thread
    /// safe, but it fails rather than allocates if the new ratio needs more
    /// history than create() reserved -- see ResamplerSpec::lowestRatio.
    [[nodiscard]] Status setRatio(double outputPerInput) noexcept;

    /// Sets the ratio from a pair of rates, and keeps it exact. Where the ratio
    /// is a ratio of whole rates -- 147/160 for 48 kHz to 44.1 kHz -- the phase
    /// is stepped in integers, so the sampling instants stay exactly on the
    /// grid however long the stream runs. setRatio() cannot do that: it has only
    /// a real number to work from, and a real number accumulates.
    [[nodiscard]] Status setRates(SampleRate inputRate, SampleRate outputRate) noexcept;

    /// True while the conversion is exact in the sense above.
    [[nodiscard]] bool hasExactPhase() const noexcept { return exactPhase_; }

    /// Taps summed per output sample at the current ratio. Downsampling
    /// stretches the filter, so this grows as the ratio falls.
    [[nodiscard]] int tapsPerOutput() const noexcept;

    /// Input samples the filter needs on each side of an output sample. The
    /// first output therefore cannot appear until this many samples have been
    /// fed. It is not latency: output sample k still describes input time
    /// k / ratio, so nothing downstream needs to compensate for anything.
    [[nodiscard]] SampleCount leadInSamples() const noexcept { return spanSamples_; }

    /// An upper bound on the outputs that feeding `inputCount` more samples can
    /// produce. For sizing a buffer -- it is deliberately generous by a sample
    /// or two rather than exact.
    [[nodiscard]] SampleCount maximumOutputFor(SampleCount inputCount) const noexcept;

    void reset() noexcept;

    /// Converts as much as fits. Consumes input until either the input runs out
    /// or the output buffer is full, and reports both counts. `input` and
    /// `output` must not overlap. A null or empty input is legal and collects
    /// the outputs a previous, output-limited call could not write.
    ///
    /// The stream is aligned, not delayed: output sample k carries input time
    /// k / ratio, counting from the first sample ever fed.
    [[nodiscard]] ResamplerProgress process(const float* input, SampleCount inputCount,
                                            float* output, SampleCount outputCapacity) noexcept;

    /// Drains the outputs that the input already fed still owes, by running the
    /// filter out over zeros. Call it repeatedly until it returns 0; together
    /// with process() it produces exactly ceil(inputCount * ratio) samples for a
    /// stream of inputCount samples.
    ///
    /// Terminal for the stream: the zeros it pushes are real history, so a
    /// process() call after a flush() continues a signal with a gap in it.
    /// reset() starts a new stream.
    [[nodiscard]] SampleCount flush(float* output, SampleCount outputCapacity) noexcept;

    /// True once flush() has produced everything the fed input owes.
    [[nodiscard]] bool isDrained() const noexcept { return readWhole_ >= fed_; }

private:
    Resampler(const ResamplerSpec& spec, double ratio, double lowestRatio);

    void buildTable(ResamplerQuality quality);

    /// Recomputes everything the ratio alone decides -- the stretch, the step
    /// through the table and the filter's reach. The step through the *signal*
    /// is set by the caller, because only it knows whether the phase is exact.
    void applyRatio(double ratio) noexcept;

    void configureRates(SampleRate inputRate, SampleRate outputRate) noexcept;

    /// True if `ratio` needs no more history than create() reserved.
    [[nodiscard]] bool fitsHistory(double ratio) const noexcept;

    [[nodiscard]] double fraction() const noexcept;

    [[nodiscard]] float sampleAt(SampleIndex index) const noexcept;

    [[nodiscard]] double coefficientAt(double index) const noexcept;

    [[nodiscard]] bool canEmit() const noexcept;

    void push(float sample) noexcept;

    [[nodiscard]] float emit() const noexcept;

    void advance() noexcept;

    ResamplerQuality quality_ = ResamplerQuality::Best;
    double ratio_ = 1.0;

    // Prototype filter, sampled at phases_ entries per input sample and stored
    // for non-negative time only -- it is symmetric, and halving it halves the
    // cache footprint of the inner loop.
    std::vector<float> table_;
    int halfWidth_ = 0;
    int phases_ = 0;
    int tableLimit_ = 0;

    // Downsampling stretches the prototype so the cutoff follows the lower of
    // the two rates. Upsampling never stretches it: the input is already band
    // limited to its own Nyquist.
    double stretch_ = 1.0;
    double tableStep_ = 0.0;
    SampleCount spanSamples_ = 0;
    int maximumTapsPerSide_ = 0;

    std::vector<float> history_;
    SampleCount historyMask_ = 0;
    /// Absolute index of the next input sample to be written, counting zeros
    /// pushed by flush().
    SampleCount written_ = 0;
    /// The same count excluding flush() zeros: how much real signal exists.
    SampleCount fed_ = 0;

    /// Whole part of the input time the next output sample sits at.
    SampleIndex readWhole_ = 0;
    /// Fractional part, as numerator_/denominator_ when the phase is exact and
    /// as fraction_ otherwise.
    std::int64_t numerator_ = 0;
    std::int64_t denominator_ = 1;
    double fraction_ = 0.0;
    bool exactPhase_ = true;

    SampleCount stepWhole_ = 1;
    std::int64_t stepNumerator_ = 0;
    double stepFraction_ = 0.0;

    /// A ratio of exactly one, landing on exactly the input samples, is a copy.
    /// Filtering it would only remove the top of the band the caller already
    /// has, and a 48 kHz to 48 kHz conversion is expected to be a null test.
    bool identity_ = false;
};

} // namespace sa::dsp
