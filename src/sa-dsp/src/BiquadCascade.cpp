#include <sa/dsp/BiquadCascade.h>
#include <sa/dsp/Decibels.h>

#include <cmath>
#include <numbers>

namespace sa::dsp {

Result<BiquadCascade> BiquadCascade::butterworth(FilterType type, int order, SampleRate rate,
                                                 double frequency) {
    if (type != FilterType::LowPass && type != FilterType::HighPass) {
        return Error{ErrorCode::InvalidArgument, "Butterworth designs low-pass or high-pass only"};
    }
    if (order < 2 || order % 2 != 0) {
        return Error{ErrorCode::InvalidArgument, "Butterworth order must be even and at least 2"};
    }
    if (order / 2 > kMaxSections) {
        return Error{ErrorCode::OutOfRange, "Butterworth order exceeds the cascade capacity"};
    }

    BiquadCascade cascade;
    for (int k = 0; k < order / 2; ++k) {
        const double angle = std::numbers::pi * static_cast<double>(2 * k + 1) /
                             (2.0 * static_cast<double>(order));
        const double q = 0.5 / std::cos(angle);

        const Result<BiquadCoefficients> section =
            (type == FilterType::LowPass) ? BiquadCoefficients::lowPass(rate, frequency, q)
                                          : BiquadCoefficients::highPass(rate, frequency, q);
        if (!section) {
            return section.error();
        }
        if (const Status status = cascade.append(section.value()); !status) {
            return status.error();
        }
    }
    return cascade;
}

Status BiquadCascade::append(const BiquadCoefficients& coefficients) {
    if (sectionCount_ >= kMaxSections) {
        return Error{ErrorCode::OutOfRange, "cascade is full"};
    }
    sections_[static_cast<std::size_t>(sectionCount_)] = Biquad{coefficients};
    ++sectionCount_;
    return {};
}

Status BiquadCascade::setSection(int index, const BiquadCoefficients& coefficients) {
    if (index < 0 || index >= sectionCount_) {
        return Error{ErrorCode::OutOfRange, "section index out of range"};
    }
    sections_[static_cast<std::size_t>(index)].setCoefficients(coefficients);
    return {};
}

const BiquadCoefficients* BiquadCascade::sectionCoefficients(int index) const noexcept {
    if (index < 0 || index >= sectionCount_) {
        return nullptr;
    }
    return &sections_[static_cast<std::size_t>(index)].coefficients();
}

void BiquadCascade::reset() noexcept {
    for (int i = 0; i < sectionCount_; ++i) {
        sections_[static_cast<std::size_t>(i)].reset();
    }
}

void BiquadCascade::process(const float* input, float* output, SampleCount count) noexcept {
    // Sample-at-a-time through the whole chain rather than section-at-a-time
    // over the block: the state stays in registers across the sections, and no
    // scratch buffer is needed -- which is what makes this allocation-free.
    for (SampleCount i = 0; i < count; ++i) {
        output[i] = processSample(input[i]);
    }
}

std::complex<double> BiquadCascade::response(double normalisedFrequency) const noexcept {
    std::complex<double> total{1.0, 0.0};
    for (int i = 0; i < sectionCount_; ++i) {
        total *= sections_[static_cast<std::size_t>(i)].coefficients().response(normalisedFrequency);
    }
    return total;
}

double BiquadCascade::magnitudeDb(SampleRate rate, double frequency) const noexcept {
    if (!rate.isValid()) {
        return kSilenceDecibels;
    }
    return gainToDecibels(std::abs(response(frequency / rate.hz())));
}

bool BiquadCascade::isStable() const noexcept {
    for (int i = 0; i < sectionCount_; ++i) {
        if (!sections_[static_cast<std::size_t>(i)].coefficients().isStable()) {
            return false;
        }
    }
    return true;
}

} // namespace sa::dsp
