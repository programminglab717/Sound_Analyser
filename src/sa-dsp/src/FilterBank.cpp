#include <sa/dsp/FilterBank.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <utility>

namespace sa::dsp {

namespace {

/// Decades between neighbouring centres.
///
/// The base-ten system's step: 10^(1/10) from one third-octave centre to the
/// next, 10^(3/10) from one octave centre to the next. 10^(3/10) is 1.99526
/// rather than 2, and that single factor is the whole of the difference
/// between this convention and the base-two one.
[[nodiscard]] constexpr double centreStepDecades(BandSpacing spacing) noexcept {
    return spacing == BandSpacing::Octave ? 0.3 : 0.1;
}

/// Half a band's width, in octaves: an edge sits 2^(this) above the centre and
/// 2^(-this) below it.
///
/// Centres spaced in powers of ten and edges in powers of two is not an
/// oversight. It is the convention the FFT-integrated band display in this
/// repository already uses, and matching it is what lets a band measured
/// through this bank be compared against the same band measured by summing
/// bins. A fully base-ten edge would be a factor of 10^(3/10) to the same
/// power -- 1.12202 rather than 1.12246 for a third-octave -- which moves an
/// edge by 0.04% of itself, across a band that is 23% wide.
[[nodiscard]] constexpr double halfWidthOctaves(BandSpacing spacing) noexcept {
    return spacing == BandSpacing::Octave ? 0.5 : 1.0 / 6.0;
}

[[nodiscard]] Status checkRange(const FilterBankSettings& settings) {
    if (!std::isfinite(settings.lowestCentreHz) || !std::isfinite(settings.highestCentreHz)) {
        return Error{ErrorCode::InvalidArgument, "band range must be finite"};
    }
    if (settings.lowestCentreHz < 1.0) {
        return Error{ErrorCode::InvalidArgument, "band range starts below 1 Hz"};
    }
    if (settings.highestCentreHz < settings.lowestCentreHz) {
        return Error{ErrorCode::InvalidArgument, "band range is inverted"};
    }
    return {};
}

[[nodiscard]] Status checkOrder(int order) {
    if (order < 2 || order % 2 != 0) {
        return Error{ErrorCode::InvalidArgument, "band order must be even and at least 2"};
    }
    if (order > FilterBank::kMaxOrder) {
        return Error{ErrorCode::OutOfRange, "band order exceeds the cascade capacity"};
    }
    return {};
}

/// The Butterworth band-pass sections for one band.
///
/// An order-N low-pass prototype has its poles evenly spaced on the unit
/// semicircle at exp(j pi (2k+N+1)/(2N)). The low-pass-to-band-pass
/// substitution s -> (s^2 + w0^2)/(B s) sends each of them to a quadratic
/// s^2 - p B s + w0^2, which is one biquad; so a prototype of order N becomes
/// N sections and 2N poles, and 2N is the order this counts in.
///
/// w0 and B come from the *pre-warped* edges, tan(pi f / fs), because the
/// bilinear transform that follows compresses frequency by exactly that
/// function. Pre-warping first is what puts the realised -3 dB points on the
/// band edges themselves instead of a percent or two inside them.
///
/// Each section carries one factor of the numerator, B s, which the bilinear
/// transform turns into B(1 - z^-2) -- a zero at DC and a zero at Nyquist,
/// which is what a band-pass biquad has. Every factor of the transfer function
/// is therefore accounted for exactly once, so the sections multiply back to
/// the design and the cascade is unity at the band centre with no normalising
/// pass to get wrong.
[[nodiscard]] Result<BiquadCascade> designBand(SampleRate rate, const FilterBand& band, int order) {
    const int sections = order / 2;
    const double lower = std::tan(std::numbers::pi * band.lowHz / rate.hz());
    const double upper = std::tan(std::numbers::pi * band.highHz / rate.hz());
    const double centreSquared = lower * upper;
    const double width = upper - lower;

    BiquadCascade cascade;

    // Bilinear transform of (B s) / (s^2 + linear s + constant) under
    // s = (1 - z^-1)/(1 + z^-1), which is the substitution the pre-warping
    // above was chosen to match.
    const auto appendSection = [&](double linear, double constant) -> Status {
        const double scale = 1.0 / (1.0 + linear + constant);
        BiquadCoefficients coefficients;
        coefficients.b0 = width * scale;
        coefficients.b1 = 0.0;
        coefficients.b2 = -width * scale;
        coefficients.a1 = 2.0 * (constant - 1.0) * scale;
        coefficients.a2 = (1.0 - linear + constant) * scale;
        return cascade.append(coefficients);
    };

    for (int k = 0; k < sections / 2; ++k) {
        // One prototype pole above the real axis. It and its conjugate give
        // four band-pass poles between them; each root found here, paired with
        // its own conjugate, is one section with real coefficients.
        const double angle = std::numbers::pi * static_cast<double>(2 * k + sections + 1) /
                             (2.0 * static_cast<double>(sections));
        const std::complex<double> pole{std::cos(angle), std::sin(angle)};
        const std::complex<double> midpoint = pole * width * 0.5;
        const std::complex<double> offset = std::sqrt(midpoint * midpoint - centreSquared);
        const std::complex<double> roots[] = {midpoint + offset, midpoint - offset};
        for (const std::complex<double>& root : roots) {
            if (const Status status = appendSection(-2.0 * root.real(), std::norm(root)); !status) {
                return status.error();
            }
        }
    }

    if (sections % 2 != 0) {
        // An odd-order prototype also has a real pole at -1, whose quadratic
        // s^2 + B s + w0^2 is already real. No root-finding for this one, and
        // what comes out is exactly the second-order band-pass of Q = w0/B
        // that a single biquad would have given on its own.
        if (const Status status = appendSection(width, centreSquared); !status) {
            return status.error();
        }
    }
    return cascade;
}

} // namespace

double bandCentreHz(BandSpacing spacing, int index) noexcept {
    return kBandReferenceHz *
           std::pow(10.0, centreStepDecades(spacing) * static_cast<double>(index));
}

Result<std::vector<FilterBand>> bandLayout(const FilterBankSettings& settings) {
    if (const Status status = checkRange(settings); !status) {
        return status.error();
    }

    // Invert the definition to get the indices whose centres fall in the
    // range, widened by the rounding in the names of the limits themselves --
    // see kBandNameTolerance.
    const double step = centreStepDecades(settings.spacing);
    const double half = halfWidthOctaves(settings.spacing);
    const double widened = 1.0 + kBandNameTolerance;
    const double first = std::log10(settings.lowestCentreHz / (widened * kBandReferenceHz)) / step;
    const double last = std::log10(settings.highestCentreHz * widened / kBandReferenceHz) / step;

    const int firstIndex = static_cast<int>(std::ceil(first));
    const int lastIndex = static_cast<int>(std::floor(last));

    std::vector<FilterBand> bands;
    bands.reserve(static_cast<std::size_t>(std::max(0, lastIndex - firstIndex + 1)));
    for (int index = firstIndex; index <= lastIndex; ++index) {
        FilterBand band;
        band.index = index;
        band.centreHz = bandCentreHz(settings.spacing, index);
        band.lowHz = band.centreHz * std::exp2(-half);
        band.highHz = band.centreHz * std::exp2(half);
        bands.push_back(band);
    }
    return bands;
}

Result<FilterBank> FilterBank::create(SampleRate rate, const FilterBankSettings& settings) {
    if (!rate.isValid()) {
        return Error{ErrorCode::InvalidArgument, "sample rate is not a usable audio rate"};
    }
    if (const Status status = checkOrder(settings.order); !status) {
        return status.error();
    }
    Result<std::vector<FilterBand>> layout = bandLayout(settings);
    if (!layout) {
        return layout.error();
    }

    FilterBank bank;
    bank.rate_ = rate;
    bank.settings_ = settings;

    const double nyquist = rate.hz() * 0.5;
    for (const FilterBand& band : layout.value()) {
        // Strictly below Nyquist, not at it: the part of a band that reaches
        // past Nyquist is not in the signal at all, so a filter here would be
        // measuring something narrower than the band it is named after and
        // saying nothing about the difference.
        if (!(band.highHz < nyquist)) {
            bank.unavailable_.push_back(UnavailableBand{band, BandUnavailability::AboveNyquist});
            continue;
        }

        Result<BiquadCascade> cascade = designBand(rate, band, settings.order);
        if (!cascade) {
            return cascade.error();
        }
        // The Jury test, which is exact rather than sampled, and which is also
        // false for a coefficient that has come out as a NaN or an infinity.
        // A band that fails it is reported missing rather than handed over:
        // the caller can see a gap, and cannot see that a level came from a
        // filter that was ringing.
        if (!cascade.value().isStable()) {
            bank.unavailable_.push_back(UnavailableBand{band, BandUnavailability::Unstable});
            continue;
        }

        bank.bands_.push_back(band);
        bank.filters_.push_back(std::move(cascade).value());
    }
    return bank;
}

BiquadCascade* FilterBank::filter(int index) noexcept {
    if (index < 0 || index >= bandCount()) {
        return nullptr;
    }
    return &filters_[static_cast<std::size_t>(index)];
}

const BiquadCascade* FilterBank::filter(int index) const noexcept {
    if (index < 0 || index >= bandCount()) {
        return nullptr;
    }
    return &filters_[static_cast<std::size_t>(index)];
}

void FilterBank::reset() noexcept {
    for (BiquadCascade& cascade : filters_) {
        cascade.reset();
    }
}

Result<std::vector<BandLevel>> FilterBank::measure(ConstAudioBufferView audio, int channel) {
    if (channel < 0 || channel >= audio.channelCount()) {
        return Error{ErrorCode::OutOfRange, "channel index outside the buffer"};
    }

    const float* samples = audio.channel(channel);
    const SampleCount frames = audio.frames();

    std::vector<BandLevel> levels;
    levels.reserve(bands_.size());
    for (std::size_t i = 0; i < bands_.size(); ++i) {
        BiquadCascade& cascade = filters_[i];
        cascade.reset();

        // Square the band signal as it comes out, so the measurement needs no
        // scratch buffer however long the input is.
        double energy = 0.0;
        for (SampleCount frame = 0; frame < frames; ++frame) {
            const double filtered = static_cast<double>(cascade.processSample(samples[frame]));
            energy += filtered * filtered;
        }

        BandLevel level;
        level.band = bands_[i];
        level.levelDb = frames > 0 ? gainToDecibels(std::sqrt(energy / static_cast<double>(frames)))
                                   : kSilenceDecibels;
        levels.push_back(level);
    }
    return levels;
}

} // namespace sa::dsp
