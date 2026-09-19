#include <sa/analysis/NullTest.h>
#include <sa/analysis/OctaveBands.h>
#include <sa/dsp/Fft.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace sa::analysis {

namespace {

/// Largest transform this will ask dsp::RealFft to build.
///
/// RealFft sizes itself with an int, so 2^30 is the ceiling the type imposes
/// rather than one chosen here. At 48 kHz it is six hours of audio, and the
/// refusal that quotes it points at maxDelaySamples, which is the way out.
constexpr SampleCount kMaxTransformSize = 1 << 30;

[[nodiscard]] SampleCount nextPowerOfTwo(SampleCount atLeast) noexcept {
    SampleCount size = 4; // RealFft's smallest supported transform.
    while (size < atLeast && size < kMaxTransformSize) {
        size *= 2;
    }
    return size;
}

/// The offset at which `other` best matches `reference`, in samples.
///
/// Cross-correlation, computed as IFFT(conj(A) * B), which gives
/// sum_n a[n] * b[n + lag] at index `lag`. Done as a transform rather than
/// directly because the direct form is a multiply-add per sample per lag, and
/// on two three-minute files with no bound on the search that is 10^14 of
/// them.
///
/// Blocked rather than one transform over everything. The reference is cut
/// into blocks and each is correlated against the stretch of `other` it can
/// reach, with the partial results summed; the sum is the same correlation, and
/// the transform is sized by the *lag range* instead of by the recordings. A
/// bounded search over an hour of audio then costs an hour of small transforms
/// rather than one transform nothing has the memory for.
///
/// The peak is taken by magnitude, not by value, so that a polarity-inverted
/// copy aligns at its true offset instead of being pushed to whichever lag
/// happens to correlate least negatively. The gain fit that follows is signed
/// and will report the inversion.
///
/// This is the matched-filter estimate, which is the best one can do when the
/// two differ by added noise, and it is not robust to the case where they
/// differ by a filter: a strong enough phase rotation moves the peak. A delay
/// that looks wrong and a residual that will not fall are the same symptom.
[[nodiscard]] Result<SampleIndex> bestLag(const float* a, SampleCount aFrames, const float* b,
                                          SampleCount bFrames, SampleCount maxDelay) {
    // Lags outside this have no samples in common at all, so there is nothing
    // to correlate there however wide a search was asked for.
    const SampleIndex lowest =
        maxDelay > 0 ? std::max<SampleIndex>(-(aFrames - 1), -maxDelay) : -(aFrames - 1);
    const SampleIndex highest =
        maxDelay > 0 ? std::min<SampleIndex>(bFrames - 1, maxDelay) : bFrames - 1;
    const SampleCount lagCount = highest - lowest + 1;
    const SampleCount spread = lagCount - 1;

    // A block of `taken` reference samples reaches `spread` samples further
    // into `other` than it is long, so a transform of `taken + spread` holds
    // the whole correlation without wrapping one lag onto another. Starting
    // from a block about as long as the lag range balances the two terms;
    // whatever the rounding up to a power of two then buys is given back to
    // the block, which costs nothing and means fewer blocks.
    const SampleCount wanted = std::min(aFrames, lagCount);
    const SampleCount size = nextPowerOfTwo(wanted + spread);
    if (size < wanted + spread) {
        return Error{ErrorCode::InvalidArgument,
                     "these recordings are too long to cross-correlate; bound the search with "
                     "maxDelaySamples"};
    }
    const SampleCount block = std::min(aFrames, size - spread);

    const dsp::RealFft fft{static_cast<int>(size)};
    const int bins = fft.binCount();
    std::vector<float> reference(static_cast<std::size_t>(size));
    std::vector<float> compared(static_cast<std::size_t>(size));
    std::vector<std::complex<float>> referenceBins(static_cast<std::size_t>(bins));
    std::vector<std::complex<float>> comparedBins(static_cast<std::size_t>(bins));
    // Accumulated in double: the partial sums are over the whole recording and
    // a float total would stop growing long before the last block went in.
    std::vector<double> correlation(static_cast<std::size_t>(lagCount), 0.0);

    float* referenceBlock = reference.data();
    float* comparedBlock = compared.data();
    std::complex<float>* referenceSpectrum = referenceBins.data();
    std::complex<float>* comparedSpectrum = comparedBins.data();
    double* total = correlation.data();

    for (SampleCount start = 0; start < aFrames; start += block) {
        std::fill(reference.begin(), reference.end(), 0.0f);
        std::fill(compared.begin(), compared.end(), 0.0f);

        const SampleCount taken = std::min(block, aFrames - start);
        for (SampleCount i = 0; i < taken; ++i) {
            referenceBlock[i] = a[start + i];
        }

        // The stretch of `other` this block can reach, clipped to what exists.
        const SampleIndex from = start + lowest;
        const SampleCount first = std::max<SampleCount>(0, -from);
        const SampleCount last = std::min<SampleCount>(size, bFrames - from);
        for (SampleCount i = first; i < last; ++i) {
            comparedBlock[i] = b[from + i];
        }

        fft.forward(referenceBlock, referenceSpectrum);
        fft.forward(comparedBlock, comparedSpectrum);
        for (int k = 0; k < bins; ++k) {
            comparedSpectrum[k] = std::conj(referenceSpectrum[k]) * comparedSpectrum[k];
        }
        fft.inverse(comparedSpectrum, comparedBlock);

        for (SampleCount k = 0; k < lagCount; ++k) {
            total[k] += static_cast<double>(comparedBlock[k]);
        }
    }

    SampleIndex found = 0;
    double loudest = -1.0;
    for (SampleCount k = 0; k < lagCount; ++k) {
        const double magnitude = std::abs(total[k]);
        const SampleIndex lag = lowest + k;
        // Ties go to the smallest shift. On silence every lag correlates at
        // exactly zero, and answering "no delay" is better than answering with
        // whichever end of the search range came first.
        if (magnitude > loudest || (magnitude == loudest && std::abs(lag) < std::abs(found))) {
            loudest = magnitude;
            found = lag;
        }
    }
    return found;
}

} // namespace

Result<NullResult> nullTest(ConstAudioBufferView reference, ConstAudioBufferView other,
                            SampleRate rate, const NullSettings& settings, int channel) {
    if (channel < 0 || channel >= reference.channelCount() || channel >= other.channelCount()) {
        return Error{ErrorCode::OutOfRange, "channel index outside one of the recordings"};
    }
    if (!rate.isValid()) {
        return Error{ErrorCode::InvalidArgument, "a null test needs a sample rate"};
    }
    if (reference.frames() <= 0 || other.frames() <= 0) {
        return Error{ErrorCode::InvalidArgument, "a null test needs samples on both sides"};
    }

    const float* first = reference.channel(channel);
    const float* second = other.channel(channel);
    const SampleCount firstFrames = reference.frames();
    const SampleCount secondFrames = other.frames();

    NullResult out;
    // Compared as bytes rather than with ==, which would call +0.0 and -0.0
    // the same sample and call a NaN different from itself. Neither is what
    // "bit-identical" claims.
    out.bitIdentical =
        firstFrames == secondFrames &&
        std::memcmp(first, second, static_cast<std::size_t>(firstFrames) * sizeof(float)) == 0;

    if (settings.alignDelay) {
        auto found = bestLag(first, firstFrames, second, secondFrames,
                             std::max<SampleCount>(0, settings.maxDelaySamples));
        if (!found) {
            return found.error();
        }
        out.delaySamples = found.value();
    }

    // The region both recordings cover once `other` has been slid back by the
    // delay, in indices on the reference's timeline.
    const SampleIndex from = std::max<SampleIndex>(0, -out.delaySamples);
    const SampleIndex until = std::min<SampleIndex>(firstFrames, secondFrames - out.delaySamples);
    if (until <= from) {
        // Not reachable through the search above, which never returns a lag
        // that leaves the two disjoint. Kept so that a future caller supplying
        // its own delay cannot walk off the end quietly.
        return Error{ErrorCode::InvalidArgument, "the two recordings do not overlap once aligned"};
    }
    out.comparedFrames = until - from;

    double cross = 0.0;
    double otherEnergy = 0.0;
    double referenceEnergy = 0.0;
    for (SampleIndex i = from; i < until; ++i) {
        const auto x = static_cast<double>(first[i]);
        const auto y = static_cast<double>(second[i + out.delaySamples]);
        cross += x * y;
        otherEnergy += y * y;
        referenceEnergy += x * x;
    }

    // The least-squares scale: d/dg of sum (x - g*y)^2 is zero at <x,y>/<y,y>
    // and nowhere else, so this is *the* gain and not merely a gain. Unity
    // when `other` is silent, where every scale leaves the same residual and
    // there is nothing to choose between them.
    const double fit = otherEnergy > 0.0 ? cross / otherEnergy : 1.0;
    const double applied = settings.matchGain ? fit : 1.0;

    // Both fits carry the sign of the same inner product, so either answers
    // the polarity question.
    out.polarityInverted = cross < 0.0;

    // The same fit the other way round, which is the level rather than the
    // correction. Divided by the reference's energy so that it falls to the
    // decibel floor when the two have nothing in common, rather than growing
    // without bound the way a reciprocal of `fit` would.
    out.gainDb = amplitudeToDecibels(referenceEnergy > 0.0 ? cross / referenceEnergy : 0.0);

    // Two copies of the overlap: the residual, and the stretch of the
    // reference it is to be compared against. The second is a copy rather than
    // a sub-range view because measureBands reads channel zero, and `channel`
    // here need not be zero.
    AudioBuffer residual{ChannelLayout::mono(), out.comparedFrames};
    AudioBuffer aligned{ChannelLayout::mono(), out.comparedFrames};
    float* residualSamples = residual.channel(0);
    float* alignedSamples = aligned.channel(0);

    double residualEnergy = 0.0;
    double peak = 0.0;
    SampleIndex peakAt = from;
    for (SampleIndex i = from; i < until; ++i) {
        const auto x = static_cast<double>(first[i]);
        const double left = x - applied * static_cast<double>(second[i + out.delaySamples]);
        residualSamples[i - from] = static_cast<float>(left);
        alignedSamples[i - from] = first[i];
        residualEnergy += left * left;
        const double magnitude = std::abs(left);
        if (magnitude > peak) {
            peak = magnitude;
            peakAt = i;
        }
    }

    const auto frames = static_cast<double>(out.comparedFrames);
    const double referenceMeanSquare = referenceEnergy / frames;
    const double residualMeanSquare = residualEnergy / frames;
    if (referenceMeanSquare > 0.0) {
        out.residualDb = powerToDecibels(residualMeanSquare / referenceMeanSquare);
        out.valid = true;
    } else {
        // A silent reference. Two silences null perfectly and that is a real
        // answer; a silent reference against anything else is a ratio with
        // nothing underneath it.
        //
        // Judged on the other recording rather than on the residual, because
        // the residual is misleading here: the least-squares fit against a
        // silent reference is zero, so the subtraction leaves silence and
        // would report a perfect null when what happened is that there was
        // never anything to cancel.
        out.valid = !(otherEnergy > 0.0);
    }
    out.peakResidualDb = amplitudeToDecibels(peak);
    out.worstTimeSeconds = samplesToSeconds(peakAt, rate);

    if (out.bitIdentical) {
        out.verdict = NullVerdict::BitIdentical;
    } else if (out.valid && out.residualDb <= kNullFloorDb) {
        out.verdict = NullVerdict::WithinFloatFloor;
    } else {
        out.verdict = NullVerdict::Different;
    }

    OctaveBandSettings bands;
    bands.width = BandWidth::Octave;
    auto referenceBands = measureBands(aligned.constView(), rate, bands);
    if (!referenceBands) {
        return referenceBands.error();
    }
    auto residualBands = measureBands(residual.constView(), rate, bands);
    if (!residualBands) {
        return residualBands.error();
    }

    // Both layouts come from the same rate and the same settings, so they are
    // the same bands in the same order; the shorter length is taken only so
    // that the loop cannot depend on that staying true.
    const std::size_t counted =
        std::min(referenceBands.value().size(), residualBands.value().size());
    out.bands.reserve(counted);
    for (std::size_t i = 0; i < counted; ++i) {
        NullBand band;
        band.centreHz = referenceBands.value()[i].centreHz;
        band.referenceDb = referenceBands.value()[i].levelDb;
        band.residualDb = residualBands.value()[i].levelDb;
        band.relativeDb = band.residualDb - band.referenceDb;
        out.bands.push_back(band);
    }
    return out;
}

} // namespace sa::analysis
