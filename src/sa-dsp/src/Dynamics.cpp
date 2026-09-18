#include <sa/dsp/Decibels.h>
#include <sa/dsp/Dynamics.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace sa::dsp {

namespace {

[[nodiscard]] Status checkTime(double seconds, const char* what) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return Error{ErrorCode::InvalidArgument, std::string{what} + " must be finite and not negative"};
    }
    return {};
}

[[nodiscard]] Status checkLevel(double decibels, const char* what) {
    if (!std::isfinite(decibels)) {
        return Error{ErrorCode::InvalidArgument, std::string{what} + " must be finite"};
    }
    return {};
}

[[nodiscard]] Status checkRatio(double ratio) {
    if (!std::isfinite(ratio) || ratio < 1.0) {
        return Error{ErrorCode::InvalidArgument, "ratio must be finite and at least 1"};
    }
    return {};
}

[[nodiscard]] Status checkKnee(double kneeDb) {
    if (!std::isfinite(kneeDb) || kneeDb < 0.0) {
        return Error{ErrorCode::InvalidArgument, "knee width must be finite and not negative"};
    }
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
// Gain computers
// ---------------------------------------------------------------------------

double compressorGainDb(const CompressorSettings& settings, double levelDb) noexcept {
    const double over = levelDb - settings.thresholdDb;
    // Negative: the amount of gain lost per decibel of overshoot.
    const double slope = 1.0 / settings.ratio - 1.0;

    // Inside the knee the curve is the quadratic that joins the two straight
    // segments with a matching first derivative at both ends. Anything less
    // than C1 continuity here is audible as a change in the compressor's
    // character as the signal crosses the threshold.
    if (settings.kneeDb > 0.0 && 2.0 * std::abs(over) <= settings.kneeDb) {
        const double distance = over + settings.kneeDb * 0.5;
        return slope * distance * distance / (2.0 * settings.kneeDb);
    }
    return over > 0.0 ? slope * over : 0.0;
}

double expanderGainDb(const ExpanderSettings& settings, double levelDb) noexcept {
    const double over = levelDb - settings.thresholdDb;
    // Positive: extra decibels lost per decibel below the threshold.
    const double slope = settings.ratio - 1.0;

    if (settings.kneeDb > 0.0 && 2.0 * std::abs(over) <= settings.kneeDb) {
        const double distance = over - settings.kneeDb * 0.5;
        return -slope * distance * distance / (2.0 * settings.kneeDb);
    }
    return over < 0.0 ? slope * over : 0.0;
}

// ---------------------------------------------------------------------------
// Compressor
// ---------------------------------------------------------------------------

Compressor::Compressor(SampleRate rate, const CompressorSettings& settings) noexcept
    : rate_(rate), settings_(settings) {
    smoother_.setTimes(settings.attackSeconds, settings.releaseSeconds, rate);
}

Result<Compressor> Compressor::create(SampleRate rate, const CompressorSettings& settings) {
    if (!rate.isValid()) {
        return Error{ErrorCode::InvalidArgument, "sample rate is not a usable audio rate"};
    }
    Compressor compressor{rate, settings};
    if (const Status status = compressor.setSettings(settings); !status) {
        return status.error();
    }
    return compressor;
}

Status Compressor::setSettings(const CompressorSettings& settings) {
    if (const Status status = checkLevel(settings.thresholdDb, "threshold"); !status) {
        return status;
    }
    if (const Status status = checkRatio(settings.ratio); !status) {
        return status;
    }
    if (const Status status = checkTime(settings.attackSeconds, "attack"); !status) {
        return status;
    }
    if (const Status status = checkTime(settings.releaseSeconds, "release"); !status) {
        return status;
    }
    if (const Status status = checkKnee(settings.kneeDb); !status) {
        return status;
    }
    if (const Status status = checkLevel(settings.makeupGainDb, "makeup gain"); !status) {
        return status;
    }

    settings_ = settings;
    smoother_.setTimes(settings.attackSeconds, settings.releaseSeconds, rate_);
    return {};
}

float Compressor::processSample(float input) noexcept {
    const double levelDb = gainToDecibels(static_cast<double>(input));
    // The smoother tracks reduction rather than gain, so more reduction is
    // "up" and the attack coefficient applies to clamping down. That is what
    // the attack control means on a compressor.
    const double reductionDb = -compressorGainDb(settings_, levelDb);
    const double smoothed = smoother_.process(reductionDb);
    const double gain = decibelsToGain(settings_.makeupGainDb - smoothed);
    return static_cast<float>(static_cast<double>(input) * gain);
}

void Compressor::process(const float* input, float* output, SampleCount count) noexcept {
    for (SampleCount i = 0; i < count; ++i) {
        output[i] = processSample(input[i]);
    }
}

// ---------------------------------------------------------------------------
// Expander
// ---------------------------------------------------------------------------

Expander::Expander(SampleRate rate, const ExpanderSettings& settings) noexcept
    : rate_(rate), settings_(settings) {
    detector_.setTimes(0.0, settings.detectorSeconds, rate);
    smoother_.setTimes(settings.attackSeconds, settings.releaseSeconds, rate);
    smoother_.reset(0.0);
}

Result<Expander> Expander::create(SampleRate rate, const ExpanderSettings& settings) {
    if (!rate.isValid()) {
        return Error{ErrorCode::InvalidArgument, "sample rate is not a usable audio rate"};
    }
    Expander expander{rate, settings};
    if (const Status status = expander.setSettings(settings); !status) {
        return status.error();
    }
    return expander;
}

Status Expander::setSettings(const ExpanderSettings& settings) {
    if (const Status status = checkLevel(settings.thresholdDb, "threshold"); !status) {
        return status;
    }
    if (const Status status = checkRatio(settings.ratio); !status) {
        return status;
    }
    if (const Status status = checkTime(settings.attackSeconds, "attack"); !status) {
        return status;
    }
    if (const Status status = checkTime(settings.releaseSeconds, "release"); !status) {
        return status;
    }
    if (const Status status = checkKnee(settings.kneeDb); !status) {
        return status;
    }
    if (const Status status = checkLevel(settings.makeupGainDb, "makeup gain"); !status) {
        return status;
    }
    if (const Status status = checkTime(settings.detectorSeconds, "detector"); !status) {
        return status;
    }

    settings_ = settings;
    // The detector rises instantly and falls with its own constant: a peak
    // envelope, not a smoothed one. Anything slower on the way up would let a
    // transient through before the expander had opened.
    detector_.setTimes(0.0, settings.detectorSeconds, rate_);
    smoother_.setTimes(settings.attackSeconds, settings.releaseSeconds, rate_);
    return {};
}

void Expander::reset() noexcept {
    detector_.reset(0.0);
    smoother_.reset(0.0);
}

float Expander::processSample(float input) noexcept {
    const double level = detector_.process(std::abs(static_cast<double>(input)));
    const double targetDb = expanderGainDb(settings_, gainToDecibels(level));
    // Here the smoother tracks gain, so rising is the expander opening -- which
    // is what attack means on an expander, the opposite convention to the
    // compressor above.
    const double smoothed = smoother_.process(targetDb);
    const double gain = decibelsToGain(settings_.makeupGainDb + smoothed);
    return static_cast<float>(static_cast<double>(input) * gain);
}

void Expander::process(const float* input, float* output, SampleCount count) noexcept {
    for (SampleCount i = 0; i < count; ++i) {
        output[i] = processSample(input[i]);
    }
}

// ---------------------------------------------------------------------------
// Gate
// ---------------------------------------------------------------------------

Gate::Gate(SampleRate rate, const GateSettings& settings) noexcept : rate_(rate) {
    applySettings(settings);
    smoother_.reset(settings.rangeDb);
}

void Gate::applySettings(const GateSettings& settings) noexcept {
    settings_ = settings;
    detector_.setTimes(0.0, settings.detectorSeconds, rate_);
    smoother_.setTimes(settings.attackSeconds, settings.releaseSeconds, rate_);
    holdSamples_ = secondsToSamples(settings.holdSeconds, rate_);
    holdRemaining_ = std::min(holdRemaining_, holdSamples_);
}

Result<Gate> Gate::create(SampleRate rate, const GateSettings& settings) {
    if (!rate.isValid()) {
        return Error{ErrorCode::InvalidArgument, "sample rate is not a usable audio rate"};
    }
    Gate gate{rate, settings};
    if (const Status status = gate.setSettings(settings); !status) {
        return status.error();
    }
    return gate;
}

Status Gate::setSettings(const GateSettings& settings) {
    if (const Status status = checkLevel(settings.thresholdDb, "threshold"); !status) {
        return status;
    }
    if (!std::isfinite(settings.hysteresisDb) || settings.hysteresisDb < 0.0) {
        return Error{ErrorCode::InvalidArgument, "hysteresis must be finite and not negative"};
    }
    if (const Status status = checkTime(settings.attackSeconds, "attack"); !status) {
        return status;
    }
    if (const Status status = checkTime(settings.holdSeconds, "hold"); !status) {
        return status;
    }
    if (const Status status = checkTime(settings.releaseSeconds, "release"); !status) {
        return status;
    }
    if (!std::isfinite(settings.rangeDb) || settings.rangeDb > 0.0) {
        return Error{ErrorCode::InvalidArgument, "range must be finite and not positive"};
    }
    if (const Status status = checkTime(settings.detectorSeconds, "detector"); !status) {
        return status;
    }

    applySettings(settings);
    return {};
}

void Gate::reset() noexcept {
    detector_.reset(0.0);
    // A gate starts closed. Starting open would let the noise it exists to
    // remove through for one release period after every seek.
    smoother_.reset(settings_.rangeDb);
    holdRemaining_ = 0;
    open_ = false;
}

float Gate::processSample(float input) noexcept {
    const double level = detector_.process(std::abs(static_cast<double>(input)));
    const double levelDb = gainToDecibels(level);

    if (levelDb > settings_.thresholdDb) {
        open_ = true;
        holdRemaining_ = holdSamples_;
    } else if (levelDb < settings_.thresholdDb - settings_.hysteresisDb) {
        if (holdRemaining_ > 0) {
            --holdRemaining_;
        } else {
            open_ = false;
        }
    }
    // Between the two thresholds the gate keeps whatever state it had. That
    // band is the hysteresis, and sitting in it decides nothing.

    const double smoothed = smoother_.process(open_ ? 0.0 : settings_.rangeDb);
    return static_cast<float>(static_cast<double>(input) * decibelsToGain(smoothed));
}

void Gate::process(const float* input, float* output, SampleCount count) noexcept {
    for (SampleCount i = 0; i < count; ++i) {
        output[i] = processSample(input[i]);
    }
}

// ---------------------------------------------------------------------------
// Limiter
// ---------------------------------------------------------------------------

Limiter::Limiter(SampleRate rate, const LimiterSettings& settings) : rate_(rate) {
    // Size for the longest look-ahead the limiter will ever be asked for, so
    // that every later parameter change is a change of offsets only.
    const SampleCount maximum = secondsToSamples(kMaxLookAheadSeconds, rate);
    delayCapacity_ = maximum + 1;
    delay_.assign(static_cast<std::size_t>(delayCapacity_), 0.0f);
    // The window spans look-ahead + 1 sample positions, and the wedge never
    // holds more entries than the window holds positions.
    windowValues_.assign(static_cast<std::size_t>(maximum + 2), 0.0);
    windowPositions_.assign(static_cast<std::size_t>(maximum + 2), 0);

    applySettings(settings);
}

void Limiter::applySettings(const LimiterSettings& settings) noexcept {
    settings_ = settings;
    lookAheadSamples_ = secondsToSamples(settings.lookAheadSeconds, rate_);
    lookAheadSamples_ = std::clamp<SampleCount>(lookAheadSamples_, 0, delayCapacity_ - 1);
    ceilingGain_ = decibelsToGain(settings.ceilingDb);

    // Attack is a fifth of the look-ahead, not the whole of it: after five time
    // constants the envelope is 99.3% of the way down, so by the moment the
    // peak reaches the output the smoother has done all but a fraction of a
    // decibel of the work and the ceiling clamp has nothing left to do. Using
    // the full look-ahead as the constant would leave the envelope 37% short
    // and hand every transient to the clamp.
    smoother_.setTimes(settings.lookAheadSeconds * 0.2, settings.releaseSeconds, rate_);
}

Result<Limiter> Limiter::create(SampleRate rate, const LimiterSettings& settings) {
    if (!rate.isValid()) {
        return Error{ErrorCode::InvalidArgument, "sample rate is not a usable audio rate"};
    }
    if (!std::isfinite(settings.lookAheadSeconds) || settings.lookAheadSeconds < 0.0 ||
        settings.lookAheadSeconds > kMaxLookAheadSeconds) {
        return Error{ErrorCode::InvalidArgument, "look-ahead is outside the supported range"};
    }

    Limiter limiter{rate, settings};
    if (const Status status = limiter.setSettings(settings); !status) {
        return status.error();
    }
    return limiter;
}

Status Limiter::setSettings(const LimiterSettings& settings) {
    if (const Status status = checkLevel(settings.ceilingDb, "ceiling"); !status) {
        return status;
    }
    if (const Status status = checkTime(settings.releaseSeconds, "release"); !status) {
        return status;
    }
    if (!std::isfinite(settings.lookAheadSeconds) || settings.lookAheadSeconds < 0.0 ||
        settings.lookAheadSeconds > kMaxLookAheadSeconds) {
        return Error{ErrorCode::InvalidArgument, "look-ahead is outside the supported range"};
    }

    applySettings(settings);
    return {};
}

void Limiter::reset() noexcept {
    std::fill(delay_.begin(), delay_.end(), 0.0f);
    writeIndex_ = 0;
    windowHead_ = 0;
    windowCount_ = 0;
    position_ = 0;
    smoother_.reset(0.0);
    appliedGain_ = 1.0;
}

double Limiter::slideWindow(double requiredDb) noexcept {
    const auto capacity = static_cast<SampleCount>(windowValues_.size());

    // Anything at or below the arriving value is dominated by it for as long as
    // it remains in the window, so it can be dropped now.
    while (windowCount_ > 0 &&
           windowValues_[static_cast<std::size_t>((windowHead_ + windowCount_ - 1) % capacity)] <=
               requiredDb) {
        --windowCount_;
    }
    const auto slot = static_cast<std::size_t>((windowHead_ + windowCount_) % capacity);
    windowValues_[slot] = requiredDb;
    windowPositions_[slot] = position_;
    ++windowCount_;

    const SampleIndex oldest = position_ - lookAheadSamples_;
    while (windowCount_ > 0 &&
           windowPositions_[static_cast<std::size_t>(windowHead_)] < oldest) {
        windowHead_ = (windowHead_ + 1) % capacity;
        --windowCount_;
    }
    return windowValues_[static_cast<std::size_t>(windowHead_)];
}

float Limiter::processSample(float input) noexcept {
    const double arriving = static_cast<double>(input);
    const double requiredDb = std::max(0.0, gainToDecibels(arriving) - settings_.ceilingDb);
    const double windowMaximum = slideWindow(requiredDb);
    const double smoothedDb = smoother_.process(windowMaximum);

    delay_[static_cast<std::size_t>(writeIndex_)] = input;
    const SampleCount readIndex = (writeIndex_ + delayCapacity_ - lookAheadSamples_) % delayCapacity_;
    const float delayed = delay_[static_cast<std::size_t>(readIndex)];
    writeIndex_ = (writeIndex_ + 1) % delayCapacity_;
    ++position_;

    // The clamp, in linear gain rather than decibels so that the product below
    // lands on the ceiling exactly rather than a logarithm's worth away from it.
    const double magnitude = std::abs(static_cast<double>(delayed));
    const double ceiling = magnitude > ceilingGain_ ? ceilingGain_ / magnitude : 1.0;
    appliedGain_ = std::min(decibelsToGain(-smoothedDb), ceiling);
    return static_cast<float>(static_cast<double>(delayed) * appliedGain_);
}

void Limiter::process(const float* input, float* output, SampleCount count) noexcept {
    for (SampleCount i = 0; i < count; ++i) {
        output[i] = processSample(input[i]);
    }
}

double Limiter::gainReductionDb() const noexcept {
    return -gainToDecibels(appliedGain_);
}

} // namespace sa::dsp
