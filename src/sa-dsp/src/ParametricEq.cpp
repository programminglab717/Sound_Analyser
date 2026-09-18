#include <sa/dsp/ParametricEq.h>

namespace sa::dsp {

Result<ParametricEq> ParametricEq::create(SampleRate rate) {
    if (!rate.isValid()) {
        return Error{ErrorCode::InvalidArgument, "sample rate is not a usable audio rate"};
    }
    return ParametricEq{rate};
}

Result<BiquadCoefficients> ParametricEq::designBand(SampleRate rate, const EqBand& band) {
    // A disabled band is still designed -- it just loads unity gain. Validating
    // it anyway means enabling a band later cannot fail, so the UI never has to
    // explain that a switch did nothing.
    const Result<BiquadCoefficients> designed = BiquadCoefficients::design(rate, band.filter);
    if (!designed) {
        return designed.error();
    }
    return band.enabled ? designed.value() : BiquadCoefficients::passthrough();
}

const EqBand* ParametricEq::band(int index) const noexcept {
    if (index < 0 || index >= bandCount_) {
        return nullptr;
    }
    return &bands_[static_cast<std::size_t>(index)];
}

Result<int> ParametricEq::addBand(const EqBand& band) {
    if (bandCount_ >= kMaxBands) {
        return Error{ErrorCode::OutOfRange, "the EQ is full"};
    }
    const Result<BiquadCoefficients> coefficients = designBand(rate_, band);
    if (!coefficients) {
        return coefficients.error();
    }
    // Appending touches no existing section, so the other bands keep ringing
    // through the change instead of being cut off.
    if (const Status status = cascade_.append(coefficients.value()); !status) {
        return status.error();
    }

    const int index = bandCount_;
    bands_[static_cast<std::size_t>(index)] = band;
    ++bandCount_;
    return index;
}

Status ParametricEq::setBand(int index, const EqBand& band) {
    if (index < 0 || index >= bandCount_) {
        return Error{ErrorCode::OutOfRange, "band index out of range"};
    }
    const Result<BiquadCoefficients> coefficients = designBand(rate_, band);
    if (!coefficients) {
        return coefficients.error();
    }
    if (const Status status = cascade_.setSection(index, coefficients.value()); !status) {
        return status;
    }
    bands_[static_cast<std::size_t>(index)] = band;
    return {};
}

Status ParametricEq::removeBand(int index) {
    if (index < 0 || index >= bandCount_) {
        return Error{ErrorCode::OutOfRange, "band index out of range"};
    }

    for (int i = index; i + 1 < bandCount_; ++i) {
        bands_[static_cast<std::size_t>(i)] = bands_[static_cast<std::size_t>(i + 1)];
    }
    --bandCount_;

    cascade_.clear();
    for (int i = 0; i < bandCount_; ++i) {
        const Result<BiquadCoefficients> coefficients =
            designBand(rate_, bands_[static_cast<std::size_t>(i)]);
        if (!coefficients) {
            return coefficients.error();
        }
        if (const Status status = cascade_.append(coefficients.value()); !status) {
            return status;
        }
    }
    return {};
}

void ParametricEq::clearBands() noexcept {
    bandCount_ = 0;
    cascade_.clear();
}

Status ParametricEq::setSampleRate(SampleRate rate) {
    if (!rate.isValid()) {
        return Error{ErrorCode::InvalidArgument, "sample rate is not a usable audio rate"};
    }

    // Design every band before loading any of them: a band above the new
    // Nyquist must leave the EQ exactly as it was, not half converted.
    std::array<BiquadCoefficients, static_cast<std::size_t>(kMaxBands)> designed{};
    for (int i = 0; i < bandCount_; ++i) {
        const Result<BiquadCoefficients> coefficients =
            designBand(rate, bands_[static_cast<std::size_t>(i)]);
        if (!coefficients) {
            return coefficients.error();
        }
        designed[static_cast<std::size_t>(i)] = coefficients.value();
    }

    rate_ = rate;
    for (int i = 0; i < bandCount_; ++i) {
        if (const Status status = cascade_.setSection(i, designed[static_cast<std::size_t>(i)]);
            !status) {
            return status;
        }
    }
    // The state was accumulated at the old rate and means nothing at the new
    // one, so it goes rather than being reinterpreted.
    cascade_.reset();
    return {};
}

} // namespace sa::dsp
