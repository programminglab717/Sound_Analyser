#include <sa/dsp/Biquad.h>
#include <sa/dsp/Decibels.h>

#include <cmath>
#include <numbers>

namespace sa::dsp {

namespace {

/// The intermediates every cookbook shape is built from: the cutoff in radians
/// per sample and the bandwidth term alpha. All eight shapes share a
/// denominator assembled from these; only the numerator differs.
struct Cookbook {
    double cosOmega = 0.0;
    double sinOmega = 0.0;
    double alpha = 0.0;
};

[[nodiscard]] Result<Cookbook> prepare(SampleRate rate, double frequency, double q) {
    if (!rate.isValid()) {
        return Error{ErrorCode::InvalidArgument, "sample rate is not a usable audio rate"};
    }
    if (!std::isfinite(frequency) || frequency <= 0.0 || frequency >= rate.hz() * 0.5) {
        return Error{ErrorCode::InvalidArgument, "frequency must lie strictly inside (0, Nyquist)"};
    }
    if (!std::isfinite(q) || q <= 0.0) {
        return Error{ErrorCode::InvalidArgument, "Q must be positive"};
    }

    const double omega = 2.0 * std::numbers::pi * frequency / rate.hz();
    Cookbook terms;
    terms.cosOmega = std::cos(omega);
    terms.sinOmega = std::sin(omega);
    terms.alpha = terms.sinOmega / (2.0 * q);
    return terms;
}

[[nodiscard]] Status checkGain(double gainDb) {
    if (!std::isfinite(gainDb)) {
        return Error{ErrorCode::InvalidArgument, "gain must be finite"};
    }
    return {};
}

/// Divides through by a0 so the difference equation never has to.
[[nodiscard]] BiquadCoefficients normalise(double b0, double b1, double b2, double a0, double a1,
                                           double a2) noexcept {
    const double scale = 1.0 / a0;
    return BiquadCoefficients{b0 * scale, b1 * scale, b2 * scale, a1 * scale, a2 * scale};
}

} // namespace

Result<BiquadCoefficients> BiquadCoefficients::lowPass(SampleRate rate, double frequency,
                                                       double q) {
    const Result<Cookbook> terms = prepare(rate, frequency, q);
    if (!terms) {
        return terms.error();
    }
    const Cookbook& t = terms.value();
    const double shared = 1.0 - t.cosOmega;
    return normalise(shared * 0.5, shared, shared * 0.5, 1.0 + t.alpha, -2.0 * t.cosOmega,
                     1.0 - t.alpha);
}

Result<BiquadCoefficients> BiquadCoefficients::highPass(SampleRate rate, double frequency,
                                                        double q) {
    const Result<Cookbook> terms = prepare(rate, frequency, q);
    if (!terms) {
        return terms.error();
    }
    const Cookbook& t = terms.value();
    const double shared = 1.0 + t.cosOmega;
    return normalise(shared * 0.5, -shared, shared * 0.5, 1.0 + t.alpha, -2.0 * t.cosOmega,
                     1.0 - t.alpha);
}

Result<BiquadCoefficients> BiquadCoefficients::bandPass(SampleRate rate, double frequency,
                                                        double q) {
    const Result<Cookbook> terms = prepare(rate, frequency, q);
    if (!terms) {
        return terms.error();
    }
    const Cookbook& t = terms.value();
    return normalise(t.alpha, 0.0, -t.alpha, 1.0 + t.alpha, -2.0 * t.cosOmega, 1.0 - t.alpha);
}

Result<BiquadCoefficients> BiquadCoefficients::notch(SampleRate rate, double frequency, double q) {
    const Result<Cookbook> terms = prepare(rate, frequency, q);
    if (!terms) {
        return terms.error();
    }
    const Cookbook& t = terms.value();
    return normalise(1.0, -2.0 * t.cosOmega, 1.0, 1.0 + t.alpha, -2.0 * t.cosOmega, 1.0 - t.alpha);
}

Result<BiquadCoefficients> BiquadCoefficients::allPass(SampleRate rate, double frequency,
                                                       double q) {
    const Result<Cookbook> terms = prepare(rate, frequency, q);
    if (!terms) {
        return terms.error();
    }
    const Cookbook& t = terms.value();
    return normalise(1.0 - t.alpha, -2.0 * t.cosOmega, 1.0 + t.alpha, 1.0 + t.alpha,
                     -2.0 * t.cosOmega, 1.0 - t.alpha);
}

Result<BiquadCoefficients> BiquadCoefficients::peaking(SampleRate rate, double frequency, double q,
                                                       double gainDb) {
    if (const Status status = checkGain(gainDb); !status) {
        return status.error();
    }
    const Result<Cookbook> terms = prepare(rate, frequency, q);
    if (!terms) {
        return terms.error();
    }
    const Cookbook& t = terms.value();

    // A is the square root of the gain, not the gain: the peaking section
    // applies A to the numerator and 1/A to the denominator, so the two
    // together produce a boost of A^2 at the centre.
    const double a = std::pow(10.0, gainDb / 40.0);
    return normalise(1.0 + t.alpha * a, -2.0 * t.cosOmega, 1.0 - t.alpha * a, 1.0 + t.alpha / a,
                     -2.0 * t.cosOmega, 1.0 - t.alpha / a);
}

Result<BiquadCoefficients> BiquadCoefficients::lowShelf(SampleRate rate, double frequency, double q,
                                                        double gainDb) {
    if (const Status status = checkGain(gainDb); !status) {
        return status.error();
    }
    const Result<Cookbook> terms = prepare(rate, frequency, q);
    if (!terms) {
        return terms.error();
    }
    const Cookbook& t = terms.value();

    const double a = std::pow(10.0, gainDb / 40.0);
    const double rootA = std::sqrt(a);
    const double sum = a + 1.0;
    const double difference = a - 1.0;
    const double slope = 2.0 * rootA * t.alpha;

    return normalise(
        a * (sum - difference * t.cosOmega + slope), 2.0 * a * (difference - sum * t.cosOmega),
        a * (sum - difference * t.cosOmega - slope), sum + difference * t.cosOmega + slope,
        -2.0 * (difference + sum * t.cosOmega), sum + difference * t.cosOmega - slope);
}

Result<BiquadCoefficients> BiquadCoefficients::highShelf(SampleRate rate, double frequency,
                                                         double q, double gainDb) {
    if (const Status status = checkGain(gainDb); !status) {
        return status.error();
    }
    const Result<Cookbook> terms = prepare(rate, frequency, q);
    if (!terms) {
        return terms.error();
    }
    const Cookbook& t = terms.value();

    const double a = std::pow(10.0, gainDb / 40.0);
    const double rootA = std::sqrt(a);
    const double sum = a + 1.0;
    const double difference = a - 1.0;
    const double slope = 2.0 * rootA * t.alpha;

    return normalise(
        a * (sum + difference * t.cosOmega + slope), -2.0 * a * (difference + sum * t.cosOmega),
        a * (sum + difference * t.cosOmega - slope), sum - difference * t.cosOmega + slope,
        2.0 * (difference - sum * t.cosOmega), sum - difference * t.cosOmega - slope);
}

Result<BiquadCoefficients> BiquadCoefficients::design(SampleRate rate, const FilterSpec& spec) {
    switch (spec.type) {
    case FilterType::LowPass:
        return lowPass(rate, spec.frequency, spec.q);
    case FilterType::HighPass:
        return highPass(rate, spec.frequency, spec.q);
    case FilterType::BandPass:
        return bandPass(rate, spec.frequency, spec.q);
    case FilterType::Notch:
        return notch(rate, spec.frequency, spec.q);
    case FilterType::Peaking:
        return peaking(rate, spec.frequency, spec.q, spec.gainDb);
    case FilterType::LowShelf:
        return lowShelf(rate, spec.frequency, spec.q, spec.gainDb);
    case FilterType::HighShelf:
        return highShelf(rate, spec.frequency, spec.q, spec.gainDb);
    case FilterType::AllPass:
        return allPass(rate, spec.frequency, spec.q);
    }
    return Error{ErrorCode::InvalidArgument, "unknown filter type"};
}

std::complex<double> BiquadCoefficients::response(double normalisedFrequency) const noexcept {
    const double omega = -2.0 * std::numbers::pi * normalisedFrequency;
    const std::complex<double> z1{std::cos(omega), std::sin(omega)};
    const std::complex<double> z2 = z1 * z1;

    const std::complex<double> numerator = b0 + b1 * z1 + b2 * z2;
    const std::complex<double> denominator = 1.0 + a1 * z1 + a2 * z2;
    return numerator / denominator;
}

double BiquadCoefficients::magnitudeDb(SampleRate rate, double frequency) const noexcept {
    if (!rate.isValid()) {
        return kSilenceDecibels;
    }
    return gainToDecibels(std::abs(response(frequency / rate.hz())));
}

} // namespace sa::dsp
