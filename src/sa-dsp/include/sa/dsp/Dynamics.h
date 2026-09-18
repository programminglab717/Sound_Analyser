#pragma once

#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/dsp/EnvelopeFollower.h>
#include <sa/dsp/TruePeakDetector.h>

#include <vector>

/// Dynamics processors: compressor, limiter, gate, expander.
///
/// All four share one topology, chosen because it is the one that behaves
/// predictably enough to be tested against arithmetic rather than by ear:
///
///   1. A **detector** turns the incoming sample into a level in decibels.
///   2. A **gain computer** maps that level to a target gain through a static
///      transfer curve -- the threshold, ratio and knee, with no memory.
///   3. An **attack/release smoother** moves the applied gain towards that
///      target in the log domain.
///   4. The smoothed gain multiplies the signal.
///
/// Smoothing the *gain* rather than the *level* is the decision that matters.
/// Smoothing the level first and then computing gain sends the control signal
/// through the knee's curvature, so the gain reaches 63% of its travel at a
/// time that depends on how far over threshold the signal is -- the attack
/// control then means something different at every input level. Smoothing the
/// gain makes the configured time constant exactly the time constant of the
/// gain change, at any level, which is both what a user expects from the
/// control and what makes the behaviour measurable.
///
/// Compressor and Limiter detect on the sample magnitude directly, with no
/// added lag, because their job is to catch the peak that would clip. Gate and
/// Expander run a peak detector first: their decision is a threshold crossing,
/// and a signal crosses zero twice per cycle, so without an envelope the
/// comparison is remade -- and reversed -- hundreds of times a second.
///
/// Every processor also exposes those steps as two halves: detect() turns a
/// sample into a level, applyDetected() turns a level into a processed sample.
/// Nothing is gained by calling them separately on one channel -- processSample()
/// is exactly the two in sequence -- but the split is what lets StereoLink share
/// one sidechain across a pair without any of the four gain computers being
/// written twice. See StereoLink.h.
///
/// Every process() is allocation-free and lock-free. Construction and parameter
/// changes are not audio-thread work.
namespace sa::dsp {

struct CompressorSettings {
    double thresholdDb = -20.0;
    /// Input-to-output ratio above the threshold. 1 is no compression.
    double ratio = 4.0;
    double attackSeconds = 0.010;
    double releaseSeconds = 0.100;
    /// Total width of the soft knee, centred on the threshold: the curve bends
    /// from kneeDb/2 below it to kneeDb/2 above. Zero is a hard knee.
    double kneeDb = 6.0;
    double makeupGainDb = 0.0;
};

/// Static compressor transfer curve: the gain in decibels (always <= 0) applied
/// to a steady signal at `levelDb`.
///
/// Exposed because it is the whole character of the processor and has no
/// memory: a curve display can draw it, and a test can check the knee without
/// waiting out an envelope.
[[nodiscard]] double compressorGainDb(const CompressorSettings& settings, double levelDb) noexcept;

/// Downward compressor.
class Compressor {
public:
    using Settings = CompressorSettings;

    [[nodiscard]] static Result<Compressor> create(SampleRate rate,
                                                   const CompressorSettings& settings = {});

    [[nodiscard]] const CompressorSettings& settings() const noexcept { return settings_; }

    /// Replaces the parameters without clearing the envelope, so a change does
    /// not let the signal jump through uncompressed.
    [[nodiscard]] Status setSettings(const CompressorSettings& settings);

    void reset() noexcept { smoother_.reset(0.0); }

    [[nodiscard]] float processSample(float input) noexcept;

    /// The detector half: the level the gain computer will be asked about. A
    /// compressor detects on the sample magnitude directly, so this has no
    /// state of its own -- it is still a member rather than a free function
    /// because the other three processors' detectors do.
    [[nodiscard]] double detect(float input) noexcept;

    /// The rest: gain computer, smoother, multiply. `level` is a linear
    /// magnitude, not decibels, so that a stereo link can blend two channels'
    /// levels without a silent channel dragging the blend to the silence floor.
    [[nodiscard]] float applyDetected(float input, double level) noexcept;

    /// `input` and `output` may alias. A non-positive count is a no-op.
    void process(const float* input, float* output, SampleCount count) noexcept;

    void processInPlace(float* samples, SampleCount count) noexcept {
        process(samples, samples, count);
    }

    /// Gain reduction currently applied, in decibels and positive. Excludes
    /// makeup gain, which is what a gain-reduction meter shows.
    [[nodiscard]] double gainReductionDb() const noexcept { return smoother_.value(); }

private:
    Compressor(SampleRate rate, const CompressorSettings& settings) noexcept;

    SampleRate rate_;
    CompressorSettings settings_;
    AttackReleaseSmoother smoother_;
};

struct ExpanderSettings {
    double thresholdDb = -40.0;
    /// Output-to-input ratio below the threshold. 1 is no expansion; 2 means
    /// a signal 10 dB under the threshold comes out 20 dB under it.
    double ratio = 2.0;
    /// Attack is the time to *open* -- an expander's gain rises when the signal
    /// rises, the opposite direction to a compressor's.
    double attackSeconds = 0.005;
    double releaseSeconds = 0.100;
    double kneeDb = 6.0;
    double makeupGainDb = 0.0;
    /// Decay of the peak detector feeding the threshold comparison. Zero makes
    /// the detector instantaneous, which is exact but chatters on anything but
    /// a constant-magnitude signal.
    double detectorSeconds = 0.010;
};

/// Static downward-expander transfer curve: gain in decibels (always <= 0).
[[nodiscard]] double expanderGainDb(const ExpanderSettings& settings, double levelDb) noexcept;

/// Downward expander: everything below the threshold is pushed further down.
class Expander {
public:
    using Settings = ExpanderSettings;

    [[nodiscard]] static Result<Expander> create(SampleRate rate,
                                                 const ExpanderSettings& settings = {});

    [[nodiscard]] const ExpanderSettings& settings() const noexcept { return settings_; }

    [[nodiscard]] Status setSettings(const ExpanderSettings& settings);

    void reset() noexcept;

    [[nodiscard]] float processSample(float input) noexcept;

    /// Advances the peak detector and returns its output as a linear magnitude.
    [[nodiscard]] double detect(float input) noexcept;

    [[nodiscard]] float applyDetected(float input, double level) noexcept;

    void process(const float* input, float* output, SampleCount count) noexcept;

    void processInPlace(float* samples, SampleCount count) noexcept {
        process(samples, samples, count);
    }

    [[nodiscard]] double gainReductionDb() const noexcept { return -smoother_.value(); }

private:
    Expander(SampleRate rate, const ExpanderSettings& settings) noexcept;

    SampleRate rate_;
    ExpanderSettings settings_;
    AttackReleaseSmoother detector_;
    AttackReleaseSmoother smoother_;
};

struct GateSettings {
    /// Level at which a closed gate opens.
    double thresholdDb = -40.0;
    /// How far below the open threshold the level must fall before the gate
    /// starts closing. Without this gap a signal sitting on the threshold
    /// switches the gate on and off continuously -- the chatter that makes
    /// naive gates unusable on a decaying tail.
    double hysteresisDb = 3.0;
    double attackSeconds = 0.001;
    /// Minimum time the gate stays open after the level drops below the close
    /// threshold. Bridges the gap between syllables or between drum hits.
    double holdSeconds = 0.010;
    double releaseSeconds = 0.100;
    /// Attenuation applied when fully closed, in decibels and not positive.
    /// Finite rather than silence, because a gate that mutes completely makes
    /// its own action more obvious than the noise it removed.
    double rangeDb = -80.0;
    /// Decay of the peak detector feeding the threshold comparison.
    double detectorSeconds = 0.010;
};

/// Noise gate with hysteresis and hold.
class Gate {
public:
    using Settings = GateSettings;

    [[nodiscard]] static Result<Gate> create(SampleRate rate, const GateSettings& settings = {});

    [[nodiscard]] const GateSettings& settings() const noexcept { return settings_; }

    [[nodiscard]] Status setSettings(const GateSettings& settings);

    void reset() noexcept;

    [[nodiscard]] float processSample(float input) noexcept;

    /// Advances the peak detector and returns its output as a linear magnitude.
    [[nodiscard]] double detect(float input) noexcept;

    /// Makes the open/closed decision from `level` and applies the result, so a
    /// linked pair opens and closes together rather than one channel at a time.
    [[nodiscard]] float applyDetected(float input, double level) noexcept;

    void process(const float* input, float* output, SampleCount count) noexcept;

    void processInPlace(float* samples, SampleCount count) noexcept {
        process(samples, samples, count);
    }

    /// True while the gate is passing audio. Includes the hold period, so it
    /// stays true through a short dropout.
    [[nodiscard]] bool isOpen() const noexcept { return open_; }

    [[nodiscard]] double gainReductionDb() const noexcept { return -smoother_.value(); }

private:
    Gate(SampleRate rate, const GateSettings& settings) noexcept;

    void applySettings(const GateSettings& settings) noexcept;

    SampleRate rate_;
    GateSettings settings_;
    AttackReleaseSmoother detector_;
    AttackReleaseSmoother smoother_;
    SampleCount holdSamples_ = 0;
    SampleCount holdRemaining_ = 0;
    bool open_ = false;
};

struct LimiterSettings {
    /// Output ceiling. Nothing leaves the limiter above this.
    double ceilingDb = -0.3;
    double releaseSeconds = 0.050;
    /// How far ahead the limiter sees. The audio is delayed by this much, so
    /// the gain is already down when the peak arrives instead of being yanked
    /// down on top of it. Zero is legal and gives a zero-latency limiter that
    /// still respects the ceiling, at the cost of distorting the transients it
    /// catches.
    double lookAheadSeconds = 0.005;
    /// Measure the peak between the samples as well as on them.
    ///
    /// Off by default, and the default is a judgement rather than an oversight.
    /// Turning it on changes what "the ceiling" means -- a sample-peak limiter
    /// aiming at -0.3 dBFS leaves the samples at -0.3, a true-peak one leaves
    /// them wherever they have to sit for the *reconstruction* to stay at -0.3,
    /// which on dense material is a decibel or more lower. It also costs
    /// oversampling * TruePeakDetector::kTapsPerPhase multiplies a sample and
    /// adds the detector's latency. A master bound for a lossy encoder wants it
    /// on; a gain stage inside a chain does not.
    bool truePeak = false;
    /// Oversampling factor for the inter-sample detector. Fixed at
    /// construction: changing it would mean rebuilding the filter table, which
    /// is not audio-thread work.
    ///
    /// 8 rather than BS.1770-4's minimum of 4 because a limiter and a meter
    /// want different things from the same filter. Both miss a peak that falls
    /// between two oversampled points; for a meter that is a reading a fraction
    /// of a decibel low, for a limiter it is an overshoot past a ceiling it
    /// promised. Measured worst case for a steady tone: 0.42 dB at 4x, 0.09 dB
    /// at 8x.
    int oversampling = 8;
};

/// Brickwall peak limiter with look-ahead.
///
/// The ceiling is guaranteed, not approximated. The smoothed gain does almost
/// all of the work -- the look-ahead window means it has already fallen far
/// enough by the time the peak reaches the output -- but the final gain is
/// additionally clamped to whatever the outgoing sample itself requires. So an
/// envelope that has not quite caught up costs a fraction of a decibel of extra
/// reduction on one sample rather than an overshoot, and the guarantee holds
/// regardless of the settings, including with no look-ahead at all.
///
/// With LimiterSettings::truePeak off, this limits **sample** peaks, and
/// inter-sample peaks -- which appear only once a DAC or a lossy decoder
/// reconstructs the waveform -- can still exceed the ceiling. That is why
/// mastering practice leaves the ceiling at -1 dBFS rather than 0.
///
/// With it on, the detector is an oversampled reconstruction (TruePeakDetector)
/// and the ceiling applies to what the reconstruction reaches, not to the
/// samples. Two things are worth being precise about:
///
///   * The **sample** ceiling is still guaranteed exactly, by the same clamp as
///     before. The inter-sample ceiling is guaranteed only as far as the
///     detector can see -- an interpolator samples the reconstruction on a
///     finite grid, so a peak between two grid points is missed by however much
///     the waveform curves in between. The residual is measured in
///     TruePeakTests rather than assumed.
///   * The latency grows by TruePeakDetector::latencySamples(), because the
///     detector cannot describe a sample until it has seen half a filter past
///     it. latencySamples() reports the total; a host that does not compensate
///     will hear the track drift.
class Limiter {
public:
    /// Longest look-ahead the delay line is sized for at construction.
    /// Bounding it is what lets setSettings() stay allocation-free.
    static constexpr double kMaxLookAheadSeconds = 0.020;

    using Settings = LimiterSettings;

    [[nodiscard]] static Result<Limiter> create(SampleRate rate,
                                                const LimiterSettings& settings = {});

    [[nodiscard]] const LimiterSettings& settings() const noexcept { return settings_; }

    /// Changing the look-ahead or the true-peak switch re-points the delay
    /// line's read head, which steps over whatever was in flight. Do it between
    /// blocks, not under a signal. Changing the oversampling factor is rejected
    /// rather than obeyed: it is the one parameter that would have to allocate.
    [[nodiscard]] Status setSettings(const LimiterSettings& settings);

    void reset() noexcept;

    /// Total delay through the limiter, in samples: the look-ahead, plus the
    /// inter-sample detector's own latency when it is switched on. A host that
    /// does not compensate by this much will hear the limited track drift
    /// behind everything else.
    [[nodiscard]] SampleCount latencySamples() const noexcept { return audioDelaySamples_; }

    [[nodiscard]] float processSample(float input) noexcept;

    /// The detector half: the level the ceiling is measured against. That is
    /// the sample magnitude, or the inter-sample peak around the sample
    /// TruePeakDetector::latencySamples() back when truePeak is on.
    [[nodiscard]] double detect(float input) noexcept;

    /// The rest: the look-ahead window, the smoother, the delay line and the
    /// ceiling clamp. `level` is what the ceiling is applied to, so a linked
    /// pair given the same level applies the same gain to both channels --
    /// including through the clamp, which is otherwise the one place a link
    /// could still move the image.
    [[nodiscard]] float applyDetected(float input, double level) noexcept;

    void process(const float* input, float* output, SampleCount count) noexcept;

    void processInPlace(float* samples, SampleCount count) noexcept {
        process(samples, samples, count);
    }

    [[nodiscard]] double gainReductionDb() const noexcept;

private:
    Limiter(SampleRate rate, const LimiterSettings& settings);

    void applySettings(const LimiterSettings& settings) noexcept;

    /// Slides the look-ahead window forward one sample and returns the largest
    /// gain reduction any sample still inside it will need.
    ///
    /// A monotonically decreasing wedge in a ring buffer: each new value evicts
    /// every smaller one behind it, because a smaller earlier sample can never
    /// be the window maximum again. Amortised constant time, where rescanning
    /// the window would be O(look-ahead) per sample -- 480 multiplies a sample
    /// at 96 kHz and 5 ms, which is the difference between free and not.
    [[nodiscard]] double slideWindow(double requiredDb) noexcept;

    SampleRate rate_;
    LimiterSettings settings_;
    AttackReleaseSmoother smoother_;
    TruePeakDetector detector_;

    std::vector<float> delay_;
    /// The detected level for each delayed sample, so that the clamp at the
    /// output can be made against the same number the gain was computed from
    /// rather than against the sample it happens to land on.
    std::vector<float> detected_;
    SampleCount delayCapacity_ = 0;
    SampleCount writeIndex_ = 0;
    SampleCount lookAheadSamples_ = 0;
    /// How far the audio is delayed: the look-ahead, plus the detector's
    /// latency when true-peak detection is on. The level ring is read at the
    /// look-ahead alone, because the level written at a given step already
    /// describes a sample the detector's latency further back -- which is
    /// exactly what keeps the two aligned.
    SampleCount audioDelaySamples_ = 0;
    SampleCount detectorLatency_ = 0;

    std::vector<double> windowValues_;
    std::vector<SampleIndex> windowPositions_;
    SampleCount windowHead_ = 0;
    SampleCount windowCount_ = 0;
    SampleIndex position_ = 0;

    double ceilingGain_ = 1.0;
    double appliedGain_ = 1.0;
};

} // namespace sa::dsp
