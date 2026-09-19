#include <sa/analysis/Provenance.h>
#include <sa/analysis/Spectrum.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sa::analysis {

namespace {

/// Smooth the spectrum onto a log frequency axis before looking for an edge.
///
/// Two reasons, and both matter. A raw bin-by-bin curve is noisy enough that
/// any threshold finds an "edge" somewhere, and the bins are linearly spaced,
/// so a fixed number of them is a wide interval at 1 kHz and a narrow one at
/// 16 kHz -- exactly backwards for a question asked in octaves.
[[nodiscard]] std::vector<double> smoothOntoOctaves(const std::vector<float>& bins,
                                                    const SpectrumAnalyser& analyser,
                                                    double lowestHz, double highestHz, int steps) {
    std::vector<double> out(static_cast<std::size_t>(steps), SpectrumAnalyser::kSilenceDb);
    if (bins.empty() || steps <= 0 || !(highestHz > lowestHz)) {
        return out;
    }
    const double perStep = std::log2(highestHz / lowestHz) / static_cast<double>(steps);

    for (int step = 0; step < steps; ++step) {
        const double from = lowestHz * std::exp2(perStep * step);
        const double to = lowestHz * std::exp2(perStep * (step + 1));

        // Mean energy across the interval, not mean decibels: averaging
        // logarithms lets one dead bin drag a band down by tens of dB, which is
        // precisely the false edge this is trying not to find.
        double total = 0.0;
        int counted = 0;
        for (int bin = 0; bin < static_cast<int>(bins.size()); ++bin) {
            const double hz = analyser.binFrequency(bin);
            if (hz < from) {
                continue;
            }
            if (hz >= to) {
                break;
            }
            total += std::pow(10.0, bins[static_cast<std::size_t>(bin)] / 10.0);
            ++counted;
        }
        if (counted > 0) {
            out[static_cast<std::size_t>(step)] =
                10.0 * std::log10(std::max(total / counted, 1e-30));
        }
    }
    return out;
}

} // namespace

int effectiveBitDepth(ConstAudioBufferView audio, int declaredBits) noexcept {
    if (declaredBits <= 0 || declaredBits > 32 || audio.channelCount() <= 0 ||
        audio.frames() <= 0) {
        return 0;
    }

    // Every sample of an N-bit file is an integer multiple of one code. OR the
    // magnitudes together and the lowest bit still set is the smallest step the
    // file actually uses: if it is bit 8, the bottom eight bits are zero in
    // every sample and the file is a 16-bit master in a 24-bit wrapper.
    //
    // Exact rather than statistical, which is why this is worth having: it does
    // not say "probably padded", it says which bits are never touched.
    const double scale = std::exp2(declaredBits - 1);
    std::uint64_t used = 0;
    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        const float* samples = audio.channel(channel);
        for (SampleCount i = 0; i < audio.frames(); ++i) {
            const double value = std::clamp(static_cast<double>(samples[i]), -1.0, 1.0) * scale;
            used |= static_cast<std::uint64_t>(std::llabs(std::llround(value)));
        }
    }
    if (used == 0) {
        // Silence uses no bits at all. Saying "16" about it would be inventing
        // a measurement from nothing.
        return 0;
    }

    int deadBits = 0;
    while (((used >> deadBits) & 1u) == 0) {
        ++deadBits;
    }
    return std::max(0, declaredBits - deadBits);
}

Result<Provenance> examineProvenance(ConstAudioBufferView audio, SampleRate rate,
                                     const ProvenanceSettings& settings) {
    Provenance out;
    out.frames = audio.frames();
    out.declaredBits = settings.declaredBits;
    if (audio.channelCount() <= 0 || audio.frames() <= 0 || !(rate.hz() > 0.0)) {
        return out;
    }

    auto analyser = SpectrumAnalyser::create(rate);
    if (!analyser) {
        return analyser.error();
    }
    // The first channel only, as elsewhere: a spectrum of a stereo sum shows a
    // comb wherever the channels disagree in phase, which would be a picture of
    // the summing rather than of the material.
    analyser.value().add(audio, 0);
    if (analyser.value().frameCount() == 0) {
        return out;
    }
    out.valid = true;

    if (settings.declaredBits > 0) {
        out.effectiveBits = effectiveBitDepth(audio, settings.declaredBits);
        out.isPadded = out.effectiveBits > 0 && out.effectiveBits < settings.declaredBits;
    }

    const std::vector<float> average = analyser.value().averageDb();
    const double nyquist = rate.hz() * 0.5;
    const double highest = std::min(settings.highestInterestingHz, nyquist * 0.999);
    constexpr double kLowestHz = 500.0; // Below a cutoff, above most of the music's shape.
    if (!(highest > kLowestHz * 2.0)) {
        return out;
    }

    // A sixteenth of an octave per step: fine enough to place a cliff to within
    // a few per cent, coarse enough that each step averages many bins.
    const auto steps = static_cast<int>(std::round(16.0 * std::log2(highest / kLowestHz)));
    const std::vector<double> curve =
        smoothOntoOctaves(average, analyser.value(), kLowestHz, highest, steps);
    if (curve.size() < 4) {
        return out;
    }

    // The reference is the loudest the programme gets in the band being
    // searched, so "fell 40 dB" is measured against the material rather than
    // against full scale, and a quiet recording is not read as band-limited.
    const double reference = *std::max_element(curve.begin(), curve.end());

    // Walk down from the top looking for the last place the curve was still
    // within the drop of the reference. Everything above that is the dead band.
    int lastAlive = -1;
    for (int i = static_cast<int>(curve.size()) - 1; i >= 0; --i) {
        if (curve[static_cast<std::size_t>(i)] > reference - settings.cutoffDropDb) {
            lastAlive = i;
            break;
        }
    }
    if (lastAlive < 0 || lastAlive >= static_cast<int>(curve.size()) - 1) {
        // Either nothing is alive anywhere, or the spectrum runs right to the
        // top of the band. Neither is a cutoff.
        return out;
    }

    const double perStep = std::log2(highest / kLowestHz) / static_cast<double>(steps);
    out.cutoffHz = kLowestHz * std::exp2(perStep * (lastAlive + 1));

    // How far it falls just above the edge, and how quickly. The width is what
    // separates an encoder from a microphone: both roll off, one of them over a
    // quarter of an octave.
    const auto widthSteps =
        std::max(1, static_cast<int>(std::round(settings.cutoffWidthOctaves / perStep)));
    const int after = std::min(lastAlive + widthSteps, static_cast<int>(curve.size()) - 1);
    out.cutoffDropDb =
        curve[static_cast<std::size_t>(lastAlive)] - curve[static_cast<std::size_t>(after)];
    out.hasSteepCutoff = out.cutoffDropDb >= settings.cutoffDropDb;

    return out;
}

} // namespace sa::analysis
