#include <sa/analysis/RoomAcoustics.h>
#include <sa/dsp/Biquad.h>
#include <sa/dsp/FilterBank.h>

#include <algorithm>
#include <cmath>

namespace sa::analysis {

namespace {

/// Where the impulse response actually begins.
///
/// ISO 3382 starts the analysis at the direct sound, and a recording almost
/// always has some silence or pre-ringing ahead of it. Counting that as early
/// energy would flatter C50 and D50 on every measurement, by an amount that
/// depends on how the file happened to be trimmed.
///
/// The threshold is 20 dB below the peak rather than the peak itself: the
/// leading edge of the direct sound is what matters, not its maximum, and on
/// a band-limited impulse those are several samples apart.
[[nodiscard]] SampleIndex findDirectSound(const float* samples, SampleCount count) noexcept {
    double peak = 0.0;
    for (SampleCount i = 0; i < count; ++i) {
        peak = std::max(peak, std::abs(static_cast<double>(samples[i])));
    }
    if (!(peak > 0.0)) {
        return 0;
    }
    const double threshold = peak * 0.1; // -20 dB.
    for (SampleCount i = 0; i < count; ++i) {
        if (std::abs(static_cast<double>(samples[i])) >= threshold) {
            return i;
        }
    }
    return 0;
}

/// Where the decay stops being decay and becomes the noise it was recorded in.
///
/// Integrating a noisy tail backwards adds a constant to the whole curve, which
/// bends the end of it flat and drags any slope fitted through that region
/// towards a longer reverberation time than the room has. ISO 3382 points at
/// Lundeby's iterative method for finding the crossing point; this is the
/// simpler version: estimate the noise from the last tenth of the response and
/// truncate where the squared signal falls into it.
///
/// Simpler, and said to be simpler rather than described as the standard's
/// method. It is enough to keep a noise tail from inflating T30, which is the
/// failure that matters; it will be less good than Lundeby on a measurement
/// whose noise floor drifts.
[[nodiscard]] SampleCount truncationPoint(const float* samples, SampleCount count) noexcept {
    if (count < 20) {
        return count;
    }
    const SampleCount tailFrom = count - count / 10;
    double noise = 0.0;
    for (SampleCount i = tailFrom; i < count; ++i) {
        noise += static_cast<double>(samples[i]) * samples[i];
    }
    noise /= static_cast<double>(count - tailFrom);
    if (!(noise > 0.0)) {
        return count; // Silent tail: nothing to truncate, the decay is clean.
    }

    // Walk forward over a short window and stop where the local energy has
    // fallen to within 10 dB of the noise. Windowed because a single sample of
    // an impulse response dips into the noise constantly without the decay
    // having ended.
    const SampleCount window = std::max<SampleCount>(64, count / 200);
    const double stop = noise * 10.0;
    for (SampleCount at = 0; at + window <= count; at += window) {
        double energy = 0.0;
        for (SampleCount i = at; i < at + window; ++i) {
            energy += static_cast<double>(samples[i]) * samples[i];
        }
        energy /= static_cast<double>(window);
        if (energy < stop) {
            return at;
        }
    }
    return count;
}

/// Backward-integrated energy, in dB relative to the total.
///
/// `tailEnergy` is added to the running sum before anything else: it stands for
/// the energy that arrived after the record ended, and leaving it out is the
/// single biggest source of error in this whole file.
///
/// Why it matters. A record of finite length has no energy after its last
/// sample, so the integral goes to zero there and the curve goes to minus
/// infinity -- not because the room stopped, but because the recording did.
/// That plunge steepens everything fitted through it. Measured on a synthetic
/// decay whose answer is known to be 1.000 s: a record holding 25 dB of it
/// returned T20 = 0.547 and T30 = 0.499, and even a 40 dB record returned
/// 0.928 and 0.846. Every one of those is wrong in the same direction, and
/// none of them looks obviously wrong on its own.
///
/// With the tail estimated and added, the curve asymptotes to that level
/// instead of diving through it, which is both the right shape and the thing
/// that makes "there is not enough decay here to measure T30" detectable
/// rather than silently answered.
[[nodiscard]] std::vector<float> integrate(const float* samples, SampleCount count,
                                           double tailEnergy = 0.0) {
    std::vector<float> curve(static_cast<std::size_t>(count), static_cast<float>(kDecibelFloor));
    if (count <= 0) {
        return curve;
    }
    // From the end, so each entry is the energy still to come.
    double running = std::max(0.0, tailEnergy);
    std::vector<double> remaining(static_cast<std::size_t>(count));
    for (SampleCount i = count - 1; i >= 0; --i) {
        running += static_cast<double>(samples[i]) * samples[i];
        remaining[static_cast<std::size_t>(i)] = running;
        if (i == 0) {
            break;
        }
    }
    const double total = remaining[0];
    if (!(total > 0.0)) {
        return curve;
    }
    for (SampleCount i = 0; i < count; ++i) {
        const double ratio = remaining[static_cast<std::size_t>(i)] / total;
        curve[static_cast<std::size_t>(i)] = ratio > 0.0
                                                 ? static_cast<float>(10.0 * std::log10(ratio))
                                                 : static_cast<float>(kDecibelFloor);
    }
    return curve;
}

/// Least-squares slope of the curve between two decibel levels, in dB per
/// sample, or nothing when that span is not in the curve.
///
/// A straight-line fit over the whole span rather than the two endpoints:
/// endpoints make the answer depend on two samples out of thousands, and ISO
/// 3382 asks for a regression for exactly that reason.
[[nodiscard]] bool slopeBetween(const std::vector<float>& curve, double fromDb, double toDb,
                                double& slopePerSample) {
    const auto count = static_cast<SampleCount>(curve.size());
    SampleIndex first = -1;
    SampleIndex last = -1;
    for (SampleCount i = 0; i < count; ++i) {
        if (first < 0 && curve[static_cast<std::size_t>(i)] <= fromDb) {
            first = i;
        }
        if (curve[static_cast<std::size_t>(i)] <= toDb) {
            last = i;
            break;
        }
    }
    if (first < 0 || last < 0 || last <= first + 2) {
        return false;
    }

    const auto n = static_cast<double>(last - first + 1);
    double sumX = 0.0;
    double sumY = 0.0;
    double sumXY = 0.0;
    double sumXX = 0.0;
    for (SampleIndex i = first; i <= last; ++i) {
        const auto x = static_cast<double>(i - first);
        const auto y = static_cast<double>(curve[static_cast<std::size_t>(i)]);
        sumX += x;
        sumY += y;
        sumXY += x * y;
        sumXX += x * x;
    }
    const double denominator = n * sumXX - sumX * sumX;
    if (!(std::abs(denominator) > 0.0)) {
        return false;
    }
    slopePerSample = (n * sumXY - sumX * sumY) / denominator;
    return slopePerSample < 0.0;
}

/// Energy the record did not capture, estimated by extending the decay it did.
///
/// The fit is taken from -5 to -20 dB of the *uncompensated* curve, which is
/// the region least affected by the very artefact being corrected: near the
/// start the truncation subtracts a small constant from a large number. Then
/// the remaining energy is the integral of that exponential from the end of
/// the record onwards, which for a decay of `slope` dB per sample is the
/// energy at the end times -10/(slope * ln 10) samples.
///
/// Returns zero when no slope can be fitted, which leaves the behaviour
/// exactly as it was before rather than compensating by a guess.
[[nodiscard]] double estimateTailEnergy(const float* samples, SampleCount count) {
    if (count < 64) {
        return 0.0;
    }
    const std::vector<float> plain = integrate(samples, count);
    double slope = 0.0;
    if (!slopeBetween(plain, -5.0, -20.0, slope) || !(slope < 0.0)) {
        return 0.0;
    }

    // Instantaneous energy over the last stretch of the record, which is what
    // the extrapolated decay continues from.
    const SampleCount window = std::max<SampleCount>(32, count / 100);
    double energy = 0.0;
    for (SampleCount i = count - window; i < count; ++i) {
        energy += static_cast<double>(samples[i]) * samples[i];
    }
    energy /= static_cast<double>(window);

    const double samplesRemaining = -10.0 / (slope * std::log(10.0));
    if (!(samplesRemaining > 0.0) || !std::isfinite(samplesRemaining)) {
        return 0.0;
    }
    return energy * samplesRemaining;
}

} // namespace

Result<std::vector<float>> schroederCurveDb(ConstAudioBufferView impulse, int channel) {
    if (channel < 0 || channel >= impulse.channelCount()) {
        return Error{ErrorCode::OutOfRange, "channel index outside the impulse response"};
    }
    if (impulse.frames() <= 0) {
        return std::vector<float>{};
    }
    const float* samples = impulse.channel(channel);
    const SampleIndex start = findDirectSound(samples, impulse.frames());
    const SampleCount usable = impulse.frames() - start;
    return integrate(samples + start, usable, estimateTailEnergy(samples + start, usable));
}

Result<RoomAcoustics> measureRoomAcoustics(ConstAudioBufferView impulse, SampleRate rate,
                                           int channel) {
    if (channel < 0 || channel >= impulse.channelCount()) {
        return Error{ErrorCode::OutOfRange, "channel index outside the impulse response"};
    }
    if (!(rate.hz() > 0.0)) {
        return Error{ErrorCode::InvalidArgument, "an impulse response needs a sample rate"};
    }

    RoomAcoustics out;
    if (impulse.frames() <= 0) {
        return out;
    }

    const float* samples = impulse.channel(channel);
    out.directSound = findDirectSound(samples, impulse.frames());
    const float* from = samples + out.directSound;
    const SampleCount available = impulse.frames() - out.directSound;
    if (available < 16) {
        return out;
    }

    const SampleCount usable = truncationPoint(from, available);
    if (usable < 16) {
        return out;
    }

    const std::vector<float> curve = integrate(from, usable, estimateTailEnergy(from, usable));
    if (curve.empty() || curve[0] <= static_cast<float>(kDecibelFloor)) {
        return out;
    }
    out.valid = true;
    // How far the curve actually fell before flattening onto the estimated
    // tail. With the tail in, this stops reporting the record's own ending as
    // though it were decay.
    out.usableRangeDb = -static_cast<double>(curve.back());

    const double perSample = 1.0 / rate.hz();
    double slope = 0.0;
    // EDT is the 0 to -10 dB slope; T20 and T30 start at -5 to skip the direct
    // sound and the first reflections, which are not part of the tail.
    if (slopeBetween(curve, 0.0, -10.0, slope)) {
        out.earlyDecaySeconds = -60.0 / slope * perSample;
        out.hasEarlyDecay = true;
    }
    if (slopeBetween(curve, -5.0, -25.0, slope)) {
        out.t20Seconds = -60.0 / slope * perSample;
        out.hasT20 = true;
    }
    if (slopeBetween(curve, -5.0, -35.0, slope)) {
        out.t30Seconds = -60.0 / slope * perSample;
        out.hasT30 = true;
    }

    // Clarity and definition, from the energy either side of a split. These
    // use the whole response rather than the truncated one: late energy is
    // exactly what the denominator is about, and cutting the tail off would
    // make every room read clearer than it is.
    const auto split = [&](double seconds) {
        const auto at =
            std::min<SampleCount>(available, static_cast<SampleCount>(seconds * rate.hz()));
        double early = 0.0;
        double late = 0.0;
        for (SampleCount i = 0; i < available; ++i) {
            const double energy = static_cast<double>(from[i]) * from[i];
            (i < at ? early : late) += energy;
        }
        return std::pair{early, late};
    };

    const auto [early50, late50] = split(0.050);
    const auto [early80, late80] = split(0.080);
    if (late50 > 0.0) {
        out.clarity50Db = 10.0 * std::log10(early50 / late50);
    }
    if (late80 > 0.0) {
        out.clarity80Db = 10.0 * std::log10(early80 / late80);
    }
    const double total = early50 + late50;
    if (total > 0.0) {
        out.definition50 = early50 / total;

        double weighted = 0.0;
        for (SampleCount i = 0; i < available; ++i) {
            const double energy = static_cast<double>(from[i]) * from[i];
            weighted += energy * static_cast<double>(i) * perSample;
        }
        out.centreTimeSeconds = weighted / total;
    }
    return out;
}

Result<std::vector<BandedRoomAcoustics>> measureRoomAcousticsByBand(ConstAudioBufferView impulse,
                                                                    SampleRate rate, int channel,
                                                                    BandWidth width) {
    if (channel < 0 || channel >= impulse.channelCount()) {
        return Error{ErrorCode::OutOfRange, "channel index outside the impulse response"};
    }
    if (!(rate.hz() > 0.0)) {
        return Error{ErrorCode::InvalidArgument, "an impulse response needs a sample rate"};
    }

    OctaveBandSettings settings;
    settings.width = width;
    const std::vector<Band> layout = bandLayout(rate, settings);

    std::vector<BandedRoomAcoustics> out;
    out.reserve(layout.size());
    if (impulse.frames() <= 0) {
        for (const Band& band : layout) {
            out.push_back(BandedRoomAcoustics{band.centreHz, RoomAcoustics{}});
        }
        return out;
    }

    const float* samples = impulse.channel(channel);
    AudioBuffer filtered{ChannelLayout::mono(), impulse.frames()};

    // A sixth-order Butterworth band-pass per band, from the shared filter
    // bank, rather than the two biquad sections this used to build for itself.
    //
    // The difference is not cosmetic. A band's reverberation time is read off
    // the decay of what the filter passes, so whatever leaks in from a
    // neighbour is measured as though it belonged here -- and a slow decay
    // leaking into a fast band overtakes the fast one within a second or so
    // and drags the answer towards its own rate. On a room that is lively low
    // and dead high, the two-section filter read 1.07 s for T30 in the 2 kHz
    // band of a room whose true figure there is 0.50.
    //
    // Six poles against four, and Butterworth rather than a cookbook
    // band-pass: 18.5 dB per octave against 12.3, measured.
    dsp::FilterBankSettings bankSettings;
    bankSettings.spacing =
        width == BandWidth::Octave ? dsp::BandSpacing::Octave : dsp::BandSpacing::ThirdOctave;
    bankSettings.lowestCentreHz = layout.front().centreHz;
    bankSettings.highestCentreHz = layout.back().centreHz;
    auto bank = dsp::FilterBank::create(rate, bankSettings);

    for (const Band& band : layout) {
        BandedRoomAcoustics entry;
        entry.centreHz = band.centreHz;

        // The layout here names bands by the preferred numbers (125, 16000)
        // and the bank centres them on the exact base-ten values (125.89,
        // 15848.9), so they are matched by proximity rather than equality.
        // A band the bank could not realise -- above Nyquist, or one it judged
        // unstable -- has no filter, and reports nothing rather than a figure
        // taken through something else.
        const dsp::BiquadCascade* cascade = nullptr;
        if (bank) {
            for (int i = 0; i < bank.value().bandCount(); ++i) {
                const double centre = bank.value().bands()[static_cast<std::size_t>(i)].centreHz;
                if (std::abs(centre - band.centreHz) <= band.centreHz * dsp::kBandNameTolerance) {
                    cascade = bank.value().filter(i);
                    break;
                }
            }
        }
        if (cascade == nullptr) {
            out.push_back(entry);
            continue;
        }

        // Forward only, as before. A zero-phase pass would be tidier but
        // filtering backwards over an impulse response smears energy earlier in
        // time, which is precisely the axis being measured.
        auto* working = const_cast<dsp::BiquadCascade*>(cascade);
        working->reset();
        for (SampleCount i = 0; i < impulse.frames(); ++i) {
            filtered.channel(0)[i] = working->processSample(samples[i]);
        }

        auto measured = measureRoomAcoustics(filtered.constView(), rate, 0);
        if (measured) {
            entry.measures = std::move(measured).value();
        }
        out.push_back(entry);
    }
    return out;
}

} // namespace sa::analysis
