#include <sa/dsp/Resampler.h>
#include <sa/dsp/WindowedSinc.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <vector>

namespace sa::dsp {

namespace {

/// The four numbers a quality preset is: how long the prototype is, how finely
/// it is sampled, how hard the window is pulled in, and where the cutoff sits.
///
/// `cutoff` is the -6 dB point as a fraction of the *lower* of the two rates,
/// so a preset describes the same filter whether it is used to up- or
/// downsample. Everything above 0.5 there is the region an image or an alias
/// would land in, which is why the stopband is measured from 0.5 and the
/// passband edge is quoted below the cutoff rather than at it.
///
/// The two presets were chosen by scanning length against beta against cutoff
/// and taking the cheapest point that reached the target rejection; the figures
/// each one actually measures are pinned in ResamplerTests.cpp. Raising beta
/// deepens the stopband and widens the transition, so beta and cutoff have to
/// move together -- a beta that is too high for the cutoff pushes the
/// transition past Nyquist and the rejection collapses, which is exactly what
/// the scan showed at cutoffs above 0.47.
///
/// `phases` is how finely the prototype is sampled, and it sets a floor of its
/// own. The table is interpolated linearly between entries, which leaves an
/// image of the signal at the table's own sampling rate, about (cutoff/phases)^2
/// down: -134 dB at 1024 entries per sample, -111 dB at 256. Each preset's
/// floor sits well below its own stopband, which is all the number has to
/// achieve -- and it is paid for in cache, since the table is the only thing
/// the inner loop reads besides the signal. Best's is 64 * 1024 * 4 bytes,
/// a quarter of a megabyte; Fast's is 24 KB.
struct KernelDesign {
    int halfWidth;
    int phases;
    double beta;
    double cutoff;
};

[[nodiscard]] KernelDesign designFor(ResamplerQuality quality) noexcept {
    switch (quality) {
    case ResamplerQuality::Fast:
        return KernelDesign{24, 256, 9.0, 0.44};
    case ResamplerQuality::Best:
        return KernelDesign{64, 1024, 14.0, 0.465};
    }
    return KernelDesign{64, 1024, 14.0, 0.465};
}

[[nodiscard]] SampleCount roundUpToPowerOfTwo(SampleCount value) noexcept {
    SampleCount result = 1;
    while (result < value) {
        result *= 2;
    }
    return result;
}

/// Input samples the filter reaches on each side of an output sample, at a
/// given stretch.
[[nodiscard]] SampleCount reachFor(int halfWidth, double stretch) noexcept {
    return static_cast<SampleCount>(std::floor(static_cast<double>(halfWidth) * stretch)) + 1;
}

[[nodiscard]] SampleCount historyFor(int halfWidth, double ratio) noexcept {
    const double stretch = std::max(1.0, 1.0 / ratio);
    // Both sides of the filter, plus the sample the read position sits on, plus
    // slack so that a caller feeding one sample at a time never overwrites a
    // tap the current output still needs.
    return roundUpToPowerOfTwo(2 * reachFor(halfWidth, stretch) + 4);
}

/// True if the rate is a whole number of hertz, which is what lets the phase be
/// stepped in integers. Every rate in use is -- 44100, 48000, 88200 -- but a
/// rate recovered from a device clock or a drift estimate need not be, and
/// pretending that one is rational would be worse than admitting it is not.
[[nodiscard]] bool isWholeRate(SampleRate rate) noexcept {
    const double hz = rate.hz();
    return rate.isValid() && std::floor(hz) == hz;
}

} // namespace

Resampler::Resampler(const ResamplerSpec& spec, double ratio, double lowestRatio)
    : quality_(spec.quality) {
    buildTable(spec.quality);

    const double widestStretch = std::max(1.0, 1.0 / lowestRatio);
    maximumTapsPerSide_ = static_cast<int>(reachFor(halfWidth_, widestStretch));

    const SampleCount capacity = historyFor(halfWidth_, lowestRatio);
    history_.assign(static_cast<std::size_t>(capacity), 0.0f);
    historyMask_ = capacity - 1;

    applyRatio(ratio);
    configureRates(spec.inputRate, spec.outputRate);
}

void Resampler::buildTable(ResamplerQuality quality) {
    const KernelDesign design = designFor(quality);
    halfWidth_ = design.halfWidth;
    phases_ = design.phases;
    tableLimit_ = halfWidth_ * phases_;

    // Two extra entries would do -- the last coefficient an output can ask for
    // is at tableLimit_, and interpolation reads one past it. Four, because the
    // cost is nothing and it means a rounding error at the far edge of the
    // filter cannot become an out-of-bounds read.
    std::vector<double> values(static_cast<std::size_t>(tableLimit_) + 1, 0.0);
    table_.assign(static_cast<std::size_t>(tableLimit_) + 4, 0.0f);

    double integral = 0.0;
    for (int i = 0; i <= tableLimit_; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(phases_);
        const double position = t / static_cast<double>(halfWidth_);
        const double value = 2.0 * design.cutoff * sinc(2.0 * design.cutoff * t) *
                             kaiserWindow(position, design.beta);
        values[static_cast<std::size_t>(i)] = value;
        // The prototype is symmetric, so every entry but the centre counts
        // twice towards the integral over the whole filter.
        integral += (i == 0) ? value : 2.0 * value;
    }
    integral /= static_cast<double>(phases_);

    // Normalising the *integral* to one, rather than the sum of any one set of
    // taps, is what makes the gain independent of where an output sample falls
    // between two input samples. The gain at a given fractional position is the
    // integral plus the filter's response at the sampling images, and those sit
    // in the stopband -- so the passband ripple a caller measures is the
    // stopband depth, not a separate figure that needs its own correction.
    const double scale = integral != 0.0 ? 1.0 / integral : 1.0;
    for (int i = 0; i <= tableLimit_; ++i) {
        table_[static_cast<std::size_t>(i)] =
            static_cast<float>(values[static_cast<std::size_t>(i)] * scale);
    }
}

Result<Resampler> Resampler::create(const ResamplerSpec& spec) {
    if (!spec.inputRate.isValid()) {
        return Error{ErrorCode::InvalidArgument, "input rate is not a usable audio rate"};
    }
    if (!spec.outputRate.isValid()) {
        return Error{ErrorCode::InvalidArgument, "output rate is not a usable audio rate"};
    }
    if (spec.quality != ResamplerQuality::Fast && spec.quality != ResamplerQuality::Best) {
        return Error{ErrorCode::InvalidArgument, "unknown resampler quality"};
    }

    const double ratio = spec.outputRate.hz() / spec.inputRate.hz();
    if (!std::isfinite(ratio) || ratio < kLowestRatio || ratio > kHighestRatio) {
        return Error{ErrorCode::OutOfRange, "rate ratio is outside 1/128 .. 128"};
    }
    if (!std::isfinite(spec.lowestRatio) || spec.lowestRatio < 0.0) {
        return Error{ErrorCode::InvalidArgument, "lowest ratio must be finite and not negative"};
    }
    if (spec.lowestRatio > 0.0 && spec.lowestRatio < kLowestRatio) {
        return Error{ErrorCode::OutOfRange, "lowest ratio is below 1/128"};
    }
    if (spec.lowestRatio > ratio) {
        return Error{ErrorCode::InvalidArgument,
                     "lowest ratio is above the spec's own ratio, so it reserves nothing"};
    }

    return Resampler{spec, ratio, spec.lowestRatio > 0.0 ? spec.lowestRatio : ratio};
}

void Resampler::applyRatio(double ratio) noexcept {
    ratio_ = ratio;
    // Upsampling leaves the prototype alone: the input is already band limited
    // to its own Nyquist, and narrowing the filter further would only throw
    // away signal. Downsampling has to stretch it, because the band that has to
    // survive is the *output's*, and it is narrower.
    stretch_ = std::max(1.0, 1.0 / ratio);
    tableStep_ = static_cast<double>(phases_) / stretch_;
    spanSamples_ = reachFor(halfWidth_, stretch_);
}

void Resampler::configureRates(SampleRate inputRate, SampleRate outputRate) noexcept {
    const double ratio = outputRate.hz() / inputRate.hz();
    applyRatio(ratio);

    if (isWholeRate(inputRate) && isWholeRate(outputRate)) {
        const auto input = static_cast<std::int64_t>(inputRate.hz());
        const auto output = static_cast<std::int64_t>(outputRate.hz());
        const std::int64_t divisor = std::gcd(input, output);
        const std::int64_t reducedInput = input / divisor;
        const std::int64_t reducedOutput = output / divisor;

        // One output sample advances the read position by inputRate/outputRate
        // input samples. Held as a whole part and a remainder over a fixed
        // denominator, that advance is exact for as long as the stream runs --
        // where accumulating the same quantity in a double drifts, slowly, but
        // in one direction.
        const double previous = fraction();
        if (!exactPhase_ || denominator_ != reducedOutput) {
            numerator_ = std::clamp<std::int64_t>(
                std::llround(previous * static_cast<double>(reducedOutput)), 0, reducedOutput - 1);
        }
        exactPhase_ = true;
        denominator_ = reducedOutput;
        stepWhole_ = static_cast<SampleCount>(reducedInput / reducedOutput);
        stepNumerator_ = reducedInput % reducedOutput;
        stepFraction_ = 0.0;
    } else {
        const double step = 1.0 / ratio;
        fraction_ = fraction();
        exactPhase_ = false;
        stepWhole_ = static_cast<SampleCount>(std::floor(step));
        stepFraction_ = step - std::floor(step);
        numerator_ = 0;
        denominator_ = 1;
        stepNumerator_ = 0;
    }

    identity_ = stepWhole_ == 1 && stepNumerator_ == 0 && stepFraction_ == 0.0 && fraction() == 0.0;
}

Status Resampler::setRates(SampleRate inputRate, SampleRate outputRate) noexcept {
    if (!inputRate.isValid() || !outputRate.isValid()) {
        return Error{ErrorCode::InvalidArgument, "sample rate is not a usable audio rate"};
    }
    const double ratio = outputRate.hz() / inputRate.hz();
    if (!std::isfinite(ratio) || ratio < kLowestRatio || ratio > kHighestRatio) {
        return Error{ErrorCode::OutOfRange, "rate ratio is outside 1/128 .. 128"};
    }
    if (!fitsHistory(ratio)) {
        return Error{ErrorCode::OutOfRange,
                     "ratio needs more history than the spec reserved -- set lowestRatio"};
    }
    configureRates(inputRate, outputRate);
    return {};
}

Status Resampler::setRatio(double outputPerInput) noexcept {
    if (!std::isfinite(outputPerInput) || outputPerInput < kLowestRatio ||
        outputPerInput > kHighestRatio) {
        return Error{ErrorCode::OutOfRange, "ratio is outside 1/128 .. 128"};
    }
    if (!fitsHistory(outputPerInput)) {
        return Error{ErrorCode::OutOfRange,
                     "ratio needs more history than the spec reserved -- set lowestRatio"};
    }

    applyRatio(outputPerInput);
    const double step = 1.0 / outputPerInput;
    fraction_ = fraction();
    exactPhase_ = false;
    numerator_ = 0;
    denominator_ = 1;
    stepNumerator_ = 0;
    stepWhole_ = static_cast<SampleCount>(std::floor(step));
    stepFraction_ = step - std::floor(step);
    identity_ = stepWhole_ == 1 && stepFraction_ == 0.0 && fraction_ == 0.0;
    return {};
}

bool Resampler::fitsHistory(double ratio) const noexcept {
    // Two reserves, not one. The history is rounded up to a power of two, so it
    // often has room for a ratio slightly beyond the one it was sized for -- but
    // the tap count is not rounded, and a ratio that needs more taps than
    // create() reserved would be silently truncated to fit. A truncated
    // windowed sinc is a different filter with worse rejection, and nothing
    // downstream would say so. Both have to fit.
    const double stretch = std::max(1.0, 1.0 / ratio);
    return reachFor(halfWidth_, stretch) <= maximumTapsPerSide_ &&
           historyFor(halfWidth_, ratio) <= historyMask_ + 1;
}

double Resampler::fraction() const noexcept {
    if (!exactPhase_) {
        return fraction_;
    }
    return static_cast<double>(numerator_) / static_cast<double>(denominator_);
}

int Resampler::tapsPerOutput() const noexcept {
    if (identity_) {
        return 1;
    }
    return static_cast<int>(2.0 * static_cast<double>(halfWidth_) * stretch_) + 1;
}

SampleCount Resampler::maximumOutputFor(SampleCount inputCount) const noexcept {
    if (inputCount <= 0) {
        return 0;
    }
    // The read position can lag the newest input sample by the filter's reach,
    // so the next block can produce the outputs that lag owes as well as the
    // ones the new input earns. One extra sample covers the rounding.
    const double bound = (static_cast<double>(inputCount) + 1.0) * ratio_;
    return static_cast<SampleCount>(std::ceil(bound)) + 1;
}

void Resampler::reset() noexcept {
    std::fill(history_.begin(), history_.end(), 0.0f);
    written_ = 0;
    fed_ = 0;
    readWhole_ = 0;
    numerator_ = 0;
    fraction_ = 0.0;
    identity_ = stepWhole_ == 1 && stepNumerator_ == 0 && stepFraction_ == 0.0;
}

float Resampler::sampleAt(SampleIndex index) const noexcept {
    // Indices before the start of the stream are negative, and the mask maps
    // them onto slots that reset() left at zero and that nothing has reached
    // yet -- so the filter reads silence before the signal starts, which is
    // what the first outputs of a stream should be convolved with.
    return history_[static_cast<std::size_t>(index & historyMask_)];
}

double Resampler::coefficientAt(double index) const noexcept {
    const auto whole = static_cast<int>(index);
    const double fraction = index - static_cast<double>(whole);
    const double low = static_cast<double>(table_[static_cast<std::size_t>(whole)]);
    const double high = static_cast<double>(table_[static_cast<std::size_t>(whole) + 1]);
    return low + fraction * (high - low);
}

bool Resampler::canEmit() const noexcept {
    // The filter is centred, so an output sample needs input from both sides of
    // it: the newest sample fed has to be past the right-hand edge of the
    // window before the output at the middle of it can be computed.
    return identity_ ? readWhole_ < written_ : readWhole_ + spanSamples_ < written_;
}

void Resampler::push(float sample) noexcept {
    history_[static_cast<std::size_t>(written_ & historyMask_)] = sample;
    ++written_;
}

float Resampler::emit() const noexcept {
    if (identity_) {
        return sampleAt(readWhole_);
    }

    const double phase = fraction();
    const double reach = static_cast<double>(halfWidth_) * stretch_;

    // Taps at or below the output time, then taps above it. Splitting the sum
    // this way is what lets the table hold only non-negative time: both halves
    // walk away from the centre, so both index the same half of a symmetric
    // filter.
    const int leftTaps = std::min(static_cast<int>(reach - phase) + 1, maximumTapsPerSide_);
    const int rightTaps = std::min(static_cast<int>(reach + phase), maximumTapsPerSide_);

    double sum = 0.0;
    double index = phase * tableStep_;
    SampleIndex tap = readWhole_;
    for (int i = 0; i < leftTaps; ++i) {
        sum += coefficientAt(index) * static_cast<double>(sampleAt(tap));
        index += tableStep_;
        --tap;
    }

    index = (1.0 - phase) * tableStep_;
    tap = readWhole_ + 1;
    for (int i = 0; i < rightTaps; ++i) {
        sum += coefficientAt(index) * static_cast<double>(sampleAt(tap));
        index += tableStep_;
        ++tap;
    }

    // Stretching the prototype spreads the same unit area over more taps, so
    // the sum has to be brought back down by the same factor.
    return static_cast<float>(sum / stretch_);
}

void Resampler::advance() noexcept {
    readWhole_ += stepWhole_;
    if (exactPhase_) {
        numerator_ += stepNumerator_;
        if (numerator_ >= denominator_) {
            numerator_ -= denominator_;
            ++readWhole_;
        }
    } else {
        fraction_ += stepFraction_;
        if (fraction_ >= 1.0) {
            fraction_ -= 1.0;
            ++readWhole_;
        }
    }
}

ResamplerProgress Resampler::process(const float* input, SampleCount inputCount, float* output,
                                     SampleCount outputCapacity) noexcept {
    ResamplerProgress progress;
    if (output == nullptr || outputCapacity <= 0) {
        return progress;
    }
    // A call with no input is not a no-op: the previous call may have stopped
    // because its output buffer filled, and the outputs it could not write are
    // still owed. Offering no input is how a caller collects them.
    if (input == nullptr || inputCount < 0) {
        inputCount = 0;
    }

    while (progress.outputProduced < outputCapacity) {
        while (!canEmit() && progress.inputConsumed < inputCount) {
            push(input[progress.inputConsumed]);
            ++progress.inputConsumed;
            ++fed_;
        }
        if (!canEmit()) {
            break;
        }
        output[progress.outputProduced] = emit();
        ++progress.outputProduced;
        advance();
    }
    return progress;
}

SampleCount Resampler::flush(float* output, SampleCount outputCapacity) noexcept {
    if (output == nullptr || outputCapacity <= 0) {
        return 0;
    }

    SampleCount produced = 0;
    // An output sample belongs to the stream if the input time it sits at falls
    // inside the input that was actually fed. That is what makes the total
    // exactly ceil(fed * ratio), independent of how the input was blocked.
    while (produced < outputCapacity && readWhole_ < fed_) {
        while (!canEmit()) {
            push(0.0f);
        }
        output[produced] = emit();
        ++produced;
        advance();
    }
    return produced;
}

} // namespace sa::dsp
