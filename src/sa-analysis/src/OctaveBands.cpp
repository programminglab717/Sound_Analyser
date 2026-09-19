#include <sa/analysis/OctaveBands.h>
#include <sa/analysis/Spectrum.h>
#include <sa/dsp/Window.h>

#include <algorithm>
#include <cmath>

namespace sa::analysis {

namespace {

/// The preferred nominal centres, as they are printed on a display.
///
/// Written out rather than computed, because they are rounded rather than
/// exact: the third-octave band above 1000 is called 1250 and its exact centre
/// is 1258.9. Computing them would give the exact numbers, which nobody writes
/// on an axis, and rounding them arbitrarily would give 1260 for one band and
/// 1250 for another depending on where the rounding landed. This is the series
/// everybody uses.
constexpr double kThirdOctaveCentres[] = {
    20.0,   25.0,   31.5,   40.0,   50.0,   63.0,    80.0,    100.0,   125.0,   160.0,  200.0,
    250.0,  315.0,  400.0,  500.0,  630.0,  800.0,   1000.0,  1250.0,  1600.0,  2000.0, 2500.0,
    3150.0, 4000.0, 5000.0, 6300.0, 8000.0, 10000.0, 12500.0, 16000.0, 20000.0,
};

constexpr double kOctaveCentres[] = {
    31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0,
};

/// Equivalent noise bandwidth of a window, in bins.
///
/// The correction that makes a summed band mean what it says. A window spreads
/// a sinusoid across several bins -- a Hann window puts half the amplitude in
/// each neighbour -- so adding the bins up counts the same energy more than
/// once, and a full-scale sine comes out at +1.76 dB rather than 0.
///
/// The factor that removes it is the window's noise bandwidth, N * sum(w^2) /
/// sum(w)^2, which is 1.5 bins for Hann and is exactly the 1.76 dB observed.
/// That is not a coincidence: the same number describes both the tone being
/// over-counted and the bandwidth a bin really occupies for noise, which is
/// why one division fixes a tone measurement and a noise measurement at once.
[[nodiscard]] double noiseBandwidthBins(const dsp::Window& window) noexcept {
    double sum = 0.0;
    double sumOfSquares = 0.0;
    for (int i = 0; i < window.length(); ++i) {
        const double value = window[i];
        sum += value;
        sumOfSquares += value * value;
    }
    if (!(sum > 0.0)) {
        return 1.0;
    }
    return static_cast<double>(window.length()) * sumOfSquares / (sum * sum);
}

} // namespace

std::vector<Band> bandLayout(SampleRate rate, const OctaveBandSettings& settings) {
    const bool third = settings.width == BandWidth::ThirdOctave;
    // Half the bandwidth in octaves: a third-octave band runs from a sixth of
    // an octave below its centre to a sixth above.
    const double halfWidth = third ? 1.0 / 6.0 : 0.5;
    const double nyquist = rate.hz() * 0.5;

    std::vector<Band> bands;
    const auto add = [&](double centre) {
        Band band;
        band.centreHz = centre;
        band.lowHz = centre * std::exp2(-halfWidth);
        band.highHz = centre * std::exp2(halfWidth);
        // A band whose top is past Nyquist is not measurable and is left out
        // rather than reported as quiet, which is what truncating it would
        // amount to saying.
        if (band.highHz <= nyquist) {
            bands.push_back(band);
        }
    };

    if (third) {
        for (const double centre : kThirdOctaveCentres) {
            add(centre);
        }
    } else {
        for (const double centre : kOctaveCentres) {
            add(centre);
        }
    }
    return bands;
}

Result<std::vector<Band>> measureBands(ConstAudioBufferView audio, SampleRate rate,
                                       const OctaveBandSettings& settings) {
    std::vector<Band> bands = bandLayout(rate, settings);
    if (audio.channelCount() <= 0 || audio.frames() <= 0) {
        return bands;
    }

    SpectrumSettings spectrum;
    spectrum.fftSize = settings.fftSize;
    auto analyser = SpectrumAnalyser::create(rate, spectrum);
    if (!analyser) {
        return analyser.error();
    }
    analyser.value().add(audio, 0);
    if (analyser.value().frameCount() == 0) {
        return bands;
    }

    const std::vector<float> average = analyser.value().averageDb();
    const dsp::Window window{spectrum.window, settings.fftSize};
    const double perBinCorrection = noiseBandwidthBins(window);

    for (Band& band : bands) {
        // Sum energy, not decibels. A band is how much is in it, and adding
        // logarithms would be multiplying the amplitudes.
        double energy = 0.0;
        int counted = 0;
        for (int bin = 0; bin < static_cast<int>(average.size()); ++bin) {
            const double hz = analyser.value().binFrequency(bin);
            if (hz < band.lowHz) {
                continue;
            }
            if (hz >= band.highHz) {
                break;
            }
            energy += std::pow(10.0, average[static_cast<std::size_t>(bin)] / 10.0);
            ++counted;
        }

        if (counted == 0) {
            // Narrower than one bin. Take the nearest bin rather than
            // reporting silence: at 20 Hz with an 8192-point transform the
            // band is 4.6 Hz wide and a bin is 5.9, so the band genuinely
            // falls between two of them and the nearest is the honest answer.
            const auto perBin = rate.hz() / settings.fftSize;
            const auto nearest = std::clamp(static_cast<int>(std::lround(band.centreHz / perBin)),
                                            0, static_cast<int>(average.size()) - 1);
            energy = std::pow(10.0, average[static_cast<std::size_t>(nearest)] / 10.0);
        }

        band.levelDb =
            std::max(10.0 * std::log10(std::max(energy / perBinCorrection, 1e-30)), kDecibelFloor);
    }
    return bands;
}

} // namespace sa::analysis
