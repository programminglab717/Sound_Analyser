#include <sa/analysis/TempoTrack.h>
#include <sa/dsp/Fft.h>
#include <sa/dsp/Window.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace sa::analysis {

namespace {

/// Knee of the magnitude compression, as a reciprocal of magnitude. With 1000
/// the compression is close to linear below about -60 dBFS per bin and close
/// to logarithmic above it, so the quiet end keeps its proportions and the
/// loud end stops a chorus swamping a verse.
constexpr double kCompressionGamma = 1000.0;

/// How far below a frame's loudest bin a bin is still believed, as an
/// amplitude ratio. 1e-3 is 60 dB. Below that a bin is mostly the window's own
/// leakage from something louder elsewhere in the spectrum, and on a
/// compressed scale the relative wobble of something inaudible would count for
/// as much as a real change in something audible.
constexpr double kFrameFloor = 1e-3;

/// What share of the envelope's total must fall in its loudest tenth of frames
/// before the record is treated as having any events in it.
///
/// A flat envelope puts exactly a tenth of itself in its loudest tenth, so 0.1
/// is the floor of this measure and 1.0 means every onset is inside that
/// tenth. Measured on this code: a click train reads 1.00, eighth notes 0.997,
/// sixteenths 0.98 and thirty-second notes 0.77, while white noise reads 0.12
/// and held tones from 60 Hz to 900 Hz read between 0.15 and 0.38. The
/// threshold sits in that gap.
///
/// This is the test that rejects a held tone, and what it is testing is not
/// fussy: it is the difference between a record made of events and a record
/// made of a continuous wobble. The wobble is real rather than rounding error.
/// A 21 ms window cannot resolve a low tone, so the tone's positive- and
/// negative-frequency images overlap and the magnitude spectrum genuinely
/// pulses at the tone's own rate; that pulse is periodic, correlates
/// beautifully with itself, and the autocorrelation on its own reports a
/// confident tempo for a recording of nothing happening. Concentration is what
/// separates them, because a wobble is everywhere and a beat is somewhere.
constexpr double kMinimumConcentration = 0.55;

/// The fraction of frames that concentration is measured over.
constexpr std::size_t kConcentrationDivisor = 10;

/// Width of the tempo preference, in octaves. Wide enough that a genuine 70 or
/// 180 BPM piece is still found on the evidence, narrow enough to settle the
/// octave when the evidence is equal either way.
constexpr double kOctaveWidth = 0.9;

/// The weakest correlation still called a tempo.
///
/// This is the second of the two tests, and it catches the record that has
/// events in it but no rhythm -- the concentration above has nothing to say
/// about a drummer with no time. Measured on this code: click trains read from
/// 0.81 to 1.00 and tone bursts with a quiet off-beat 1.00, while two dozen
/// clicks scattered at random over the same twelve seconds read 0.09. The
/// threshold sits in that gap rather than on a slope.
constexpr double kMinimumConfidence = 0.15;

/// How many periods of a candidate tempo the record must hold for that
/// candidate to be considered. Four is not a lot of evidence, but at three the
/// correlation at the longest lags is computed from so few terms that noise
/// alone produces peaks.
constexpr SampleCount kMinimumPeriods = 4;

/// Phase search resolution, in steps per frame, for the coarse grid that the
/// least-squares fit then starts from.
constexpr int kPhaseStepsPerFrame = 8;

/// How far either side of a coarse beat the fit looks for the onset that beat
/// is about, as a fraction of the period. A quarter of a beat absorbs the
/// drift a coarse period leaves and cannot reach the neighbouring beat.
constexpr double kFitRadius = 0.25;

/// How far the fitted period may move from the correlation's. Past a tenth the
/// fit has not refined that answer, it has found a different one, and the
/// correlation -- which looked at the whole record at once -- is the better
/// judge of which octave we are in.
constexpr double kFitTolerance = 0.1;

[[nodiscard]] bool isPowerOfTwo(int value) noexcept {
    return value > 0 && (value & (value - 1)) == 0;
}

/// Everything both entry points refuse. The tempo range is not checked here:
/// it cannot affect an onset envelope, and refusing for a reason that cannot
/// change the answer is worse than not looking.
[[nodiscard]] Status validateFraming(ConstAudioBufferView audio, SampleRate rate,
                                     const TempoSettings& settings, int channel) {
    if (channel < 0 || channel >= audio.channelCount()) {
        return Error{ErrorCode::OutOfRange, "channel index outside the audio"};
    }
    if (!rate.isValid()) {
        return Error{ErrorCode::InvalidArgument, "tempo analysis needs a sample rate"};
    }
    if (!isPowerOfTwo(settings.fftSize) || settings.fftSize < 64) {
        return Error{ErrorCode::InvalidArgument, "the transform size must be a power of two >= 64"};
    }
    if (settings.hop <= 0 || settings.hop > settings.fftSize) {
        return Error{ErrorCode::InvalidArgument,
                     "the hop must be between one sample and the transform size"};
    }
    if (audio.frames() <= 0) {
        return Error{ErrorCode::InvalidArgument, "there is no audio to analyse"};
    }
    if (audio.frames() < settings.fftSize) {
        return Error{ErrorCode::InvalidArgument, "the audio is shorter than one analysis window"};
    }
    return {};
}

/// Half-wave rectified spectral flux, one value per frame.
[[nodiscard]] std::vector<float> spectralFlux(const float* samples, SampleCount count,
                                              const TempoSettings& settings) {
    const int size = settings.fftSize;
    const SampleCount hop = settings.hop;
    const SampleCount frameCount = (count - size) / hop + 1;

    const dsp::RealFft fft{size};
    const dsp::Window window{dsp::WindowType::Hann, size};
    const auto bins = static_cast<std::size_t>(fft.binCount());

    std::vector<float> block(static_cast<std::size_t>(size), 0.0f);
    std::vector<std::complex<float>> spectrum(bins);
    std::vector<double> magnitude(bins, 0.0);
    std::vector<double> compressed(bins, 0.0);
    std::vector<double> previous(bins, 0.0);

    std::vector<float> envelope(static_cast<std::size_t>(frameCount), 0.0f);
    for (SampleCount frame = 0; frame < frameCount; ++frame) {
        const SampleCount start = frame * hop;
        for (int i = 0; i < size; ++i) {
            block[static_cast<std::size_t>(i)] = samples[start + i] * window[i];
        }
        fft.forward(block.data(), spectrum.data());

        double loudest = 0.0;
        for (std::size_t bin = 0; bin < bins; ++bin) {
            magnitude[bin] = static_cast<double>(std::abs(spectrum[bin]));
            loudest = std::max(loudest, magnitude[bin]);
        }
        const double quietest = loudest * kFrameFloor;

        double flux = 0.0;
        for (std::size_t bin = 0; bin < bins; ++bin) {
            compressed[bin] = std::log1p(kCompressionGamma * std::max(magnitude[bin], quietest));
            flux += std::max(0.0, compressed[bin] - previous[bin]);
        }
        // Frame 0 has nothing behind it, so its "increase" is the whole of its
        // own magnitude and means nothing. The compressed values are still
        // kept, because frame 1 needs them.
        envelope[static_cast<std::size_t>(frame)] = frame > 0 ? static_cast<float>(flux) : 0.0f;
        previous.swap(compressed);
    }
    return envelope;
}

/// The share of the envelope that falls in its loudest tenth of frames. Zero
/// for an envelope with nothing in it.
[[nodiscard]] double onsetConcentration(const std::vector<float>& envelope) {
    const std::size_t count = envelope.size();
    if (count < kConcentrationDivisor) {
        return 0.0;
    }
    double total = 0.0;
    for (const float value : envelope) {
        total += static_cast<double>(value);
    }
    if (!(total > 0.0)) {
        return 0.0;
    }

    std::vector<float> ranked(envelope);
    const std::size_t loudest = count / kConcentrationDivisor;
    std::nth_element(ranked.begin(), ranked.begin() + static_cast<std::ptrdiff_t>(loudest),
                     ranked.end(), [](float a, float b) { return a > b; });
    double top = 0.0;
    for (std::size_t i = 0; i < loudest; ++i) {
        top += static_cast<double>(ranked[i]);
    }
    return top / total;
}

/// The envelope at a fractional frame, linearly interpolated. Out of range
/// reads as the nearer end rather than as a failure: a beat grid is allowed to
/// run past the last onset.
[[nodiscard]] double sampleEnvelope(const std::vector<float>& envelope, double at) noexcept {
    if (envelope.empty() || at < 0.0) {
        return 0.0;
    }
    const auto lastIndex = static_cast<double>(envelope.size() - 1);
    if (at >= lastIndex) {
        return static_cast<double>(envelope.back());
    }
    const double whole = std::floor(at);
    const auto index = static_cast<std::size_t>(whole);
    return static_cast<double>(envelope[index]) +
           (static_cast<double>(envelope[index + 1]) - static_cast<double>(envelope[index])) *
               (at - whole);
}

/// A beat grid in envelope-frame coordinates: beat k sits at phase + k*period.
struct FrameGrid {
    double period = 0.0;
    double phase = 0.0;
};

/// Move a coarse grid onto the onsets it is near, by least squares.
///
/// The autocorrelation settles the period to a fraction of a frame, which
/// sounds like plenty and is not. Across a record holding twenty-odd beats, a
/// tenth of a frame of period error moves the outer beats two frames off the
/// music, so the grid is visibly wrong at both ends while being right in the
/// middle. Measured before this existed: a 120 BPM click train came back as
/// 119.85 BPM and its beats missed the clicks by up to 473 samples, which is
/// nearly two hops.
///
/// So each coarse beat is used to find the strongest onset near it, and a
/// straight line is fitted through those onsets against beat number. The
/// weights are the onset strengths, which costs nothing and means a beat that
/// landed on silence contributes nothing instead of dragging the line towards
/// whatever noise happened to be nearest.
[[nodiscard]] FrameGrid fitToOnsets(const std::vector<float>& envelope, FrameGrid coarse) {
    if (envelope.empty() || !(coarse.period > 0.0)) {
        return coarse;
    }
    const auto count = static_cast<SampleCount>(envelope.size());
    const auto lastFrame = static_cast<double>(count - 1);
    const auto radius = static_cast<SampleCount>(coarse.period * kFitRadius);
    if (radius < 1) {
        return coarse;
    }

    double sumWeight = 0.0;
    double sumBeat = 0.0;
    double sumFrame = 0.0;
    double sumBeatBeat = 0.0;
    double sumBeatFrame = 0.0;
    SampleCount used = 0;
    for (SampleCount beat = 0;; ++beat) {
        const double at = coarse.phase + coarse.period * static_cast<double>(beat);
        if (at > lastFrame) {
            break;
        }
        // A second pass can start from a phase the first pass fitted a shade
        // below zero. That beat is outside the envelope, so it has no onset to
        // be fitted to.
        if (at < 0.0) {
            continue;
        }
        const auto centre = static_cast<SampleCount>(std::lround(at));
        const SampleCount from = std::max<SampleCount>(0, centre - radius);
        const SampleCount to = std::min<SampleCount>(count - 1, centre + radius);
        SampleCount peak = from;
        for (SampleCount i = from; i <= to; ++i) {
            if (envelope[static_cast<std::size_t>(i)] > envelope[static_cast<std::size_t>(peak)]) {
                peak = i;
            }
        }
        const auto weight = static_cast<double>(envelope[static_cast<std::size_t>(peak)]);
        if (!(weight > 0.0)) {
            continue;
        }
        const auto index = static_cast<double>(beat);
        const auto frame = static_cast<double>(peak);
        sumWeight += weight;
        sumBeat += weight * index;
        sumFrame += weight * frame;
        sumBeatBeat += weight * index * index;
        sumBeatFrame += weight * index * frame;
        ++used;
    }
    if (used < 3) {
        return coarse;
    }

    const double denominator = sumWeight * sumBeatBeat - sumBeat * sumBeat;
    if (!(denominator > 0.0)) {
        return coarse;
    }
    FrameGrid fitted;
    fitted.period = (sumWeight * sumBeatFrame - sumBeat * sumFrame) / denominator;
    fitted.phase = (sumFrame - fitted.period * sumBeat) / sumWeight;
    if (!(std::abs(fitted.period - coarse.period) < coarse.period * kFitTolerance)) {
        return coarse;
    }
    return fitted;
}

} // namespace

double onsetFrameSeconds(double frameIndex, SampleRate rate, const TempoSettings& settings) {
    if (!rate.isValid()) {
        return 0.0;
    }
    const auto hop = static_cast<double>(settings.hop);
    return (frameIndex * hop + static_cast<double>(settings.fftSize) - hop * 0.5) / rate.hz();
}

Result<std::vector<float>> onsetEnvelope(ConstAudioBufferView audio, SampleRate rate,
                                         const TempoSettings& settings, int channel) {
    if (const Status status = validateFraming(audio, rate, settings, channel); !status) {
        return status.error();
    }
    return spectralFlux(audio.channel(channel), audio.frames(), settings);
}

Result<BeatGrid> trackTempo(ConstAudioBufferView audio, SampleRate rate,
                            const TempoSettings& settings, int channel) {
    if (const Status status = validateFraming(audio, rate, settings, channel); !status) {
        return status.error();
    }
    if (!(settings.minBpm > 0.0) || !(settings.minBpm < settings.maxBpm)) {
        return Error{ErrorCode::InvalidArgument,
                     "the tempo range must run from a positive minimum to a larger maximum"};
    }

    const std::vector<float> envelope =
        spectralFlux(audio.channel(channel), audio.frames(), settings);
    const auto frameCount = static_cast<SampleCount>(envelope.size());

    BeatGrid grid;
    if (frameCount < 2) {
        return grid;
    }
    // Asked before the correlation rather than after it, because a record with
    // no events in it has nothing for a confidence to be about. What comes
    // back is a zero, not a low measurement.
    if (onsetConcentration(envelope) < kMinimumConcentration) {
        return grid;
    }

    // The envelope is non-negative, so its mean is large and would dominate
    // every correlation equally -- every lag would read close to one and the
    // peak would be lost in the offset. Removing it is what turns the
    // autocorrelation into a measure of periodicity rather than of level.
    double mean = 0.0;
    for (const float value : envelope) {
        mean += static_cast<double>(value);
    }
    mean /= static_cast<double>(frameCount);

    std::vector<double> centred(envelope.size(), 0.0);
    double zeroLag = 0.0;
    for (std::size_t i = 0; i < envelope.size(); ++i) {
        centred[i] = static_cast<double>(envelope[i]) - mean;
        zeroLag += centred[i] * centred[i];
    }
    zeroLag /= static_cast<double>(frameCount);
    if (!(zeroLag > 0.0)) {
        return grid;
    }

    // A lag is a number of frames, so bpm = 60 * framesPerSecond / lag: the
    // fast end of the tempo range is the short end of the lag range.
    const double framesPerSecond = rate.hz() / static_cast<double>(settings.hop);
    const double shortestPeriod = 60.0 * framesPerSecond / settings.maxBpm;
    const double longestPeriod = 60.0 * framesPerSecond / settings.minBpm;

    const auto shortest =
        std::max<SampleCount>(1, static_cast<SampleCount>(std::ceil(shortestPeriod)));
    const SampleCount longest =
        std::min(static_cast<SampleCount>(std::floor(longestPeriod)), frameCount / kMinimumPeriods);
    if (shortest > longest) {
        return grid; // Too short to hold four beats at any tempo in the range.
    }

    std::vector<double> correlation(static_cast<std::size_t>(longest - shortest + 1), 0.0);
    const double preferredBpm = std::sqrt(settings.minBpm * settings.maxBpm);

    SampleCount bestLag = shortest;
    double bestScore = 0.0;
    bool found = false;
    for (SampleCount lag = shortest; lag <= longest; ++lag) {
        const SampleCount overlap = frameCount - lag;
        double sum = 0.0;
        for (SampleCount i = 0; i < overlap; ++i) {
            sum +=
                centred[static_cast<std::size_t>(i)] * centred[static_cast<std::size_t>(i + lag)];
        }
        // Divided by the overlap rather than by the frame count. The usual
        // unnormalised form tapers towards zero as the lag grows, which is a
        // preference for fast tempi that comes from the arithmetic rather than
        // from the music, and it would fight the weighting below.
        const double value = sum / static_cast<double>(overlap) / zeroLag;
        correlation[static_cast<std::size_t>(lag - shortest)] = value;

        const double bpm = 60.0 * framesPerSecond / static_cast<double>(lag);
        const double octaves = std::log2(bpm / preferredBpm) / kOctaveWidth;
        const double score = value * std::exp(-0.5 * octaves * octaves);
        if (!found || score > bestScore) {
            bestScore = score;
            bestLag = lag;
            found = true;
        }
    }

    grid.confidence =
        std::clamp(correlation[static_cast<std::size_t>(bestLag - shortest)], 0.0, 1.0);
    if (grid.confidence < kMinimumConfidence) {
        return grid;
    }

    // Sub-frame refinement, fitted to the unweighted correlation: the
    // weighting chooses between octaves and has no business moving the period
    // within one.
    double period = static_cast<double>(bestLag);
    if (bestLag > shortest && bestLag < longest) {
        const auto at = static_cast<std::size_t>(bestLag - shortest);
        const double before = correlation[at - 1];
        const double here = correlation[at];
        const double after = correlation[at + 1];
        const double curvature = before - 2.0 * here + after;
        if (curvature < 0.0) {
            const double shift = 0.5 * (before - after) / curvature;
            if (std::abs(shift) <= 1.0) {
                period += shift;
            }
        }
    }
    period = std::clamp(period, shortestPeriod, longestPeriod);

    // Phase: slide a pulse train at that period across the envelope and keep
    // the alignment that collects the most onset strength. Every phase is
    // scored over the same number of pulses, so that a phase which happens to
    // fit one more beat into the record cannot win on count alone.
    const auto lastFrame = static_cast<double>(frameCount - 1);
    const auto pulses = std::max<SampleCount>(1, static_cast<SampleCount>(lastFrame / period));
    const auto steps = std::max<SampleCount>(
        1, static_cast<SampleCount>(std::lround(period * kPhaseStepsPerFrame)));

    FrameGrid coarse;
    coarse.period = period;
    double bestStrength = -1.0;
    for (SampleCount step = 0; step < steps; ++step) {
        const double phase = period * static_cast<double>(step) / static_cast<double>(steps);
        double strength = 0.0;
        for (SampleCount pulse = 0; pulse < pulses; ++pulse) {
            strength += sampleEnvelope(envelope, phase + period * static_cast<double>(pulse));
        }
        if (strength > bestStrength) {
            bestStrength = strength;
            coarse.phase = phase;
        }
    }

    // Twice, because the first fit moves the beats onto the onsets and the
    // second then sees onsets the first was looking past.
    FrameGrid fitted = fitToOnsets(envelope, coarse);
    fitted = fitToOnsets(envelope, fitted);
    // The fit can only have moved the period by a tenth, so this bites solely
    // when the tempo sits on the edge of the requested range.
    fitted.period = std::clamp(fitted.period, shortestPeriod, longestPeriod);

    // The grid runs across the analysed span and no further: the earliest beat
    // is the first one at or after frame zero, even when the fitted phase puts
    // an earlier beat a fraction of a frame before the record starts.
    const auto firstBeat = static_cast<SampleCount>(std::ceil(-fitted.phase / fitted.period));
    for (SampleCount beat = firstBeat;; ++beat) {
        const double at = fitted.phase + fitted.period * static_cast<double>(beat);
        if (at > lastFrame) {
            break;
        }
        grid.beatSeconds.push_back(onsetFrameSeconds(at, rate, settings));
    }
    if (grid.beatSeconds.size() < 2) {
        grid.beatSeconds.clear();
        return grid;
    }

    grid.bpm = 60.0 * framesPerSecond / fitted.period;
    grid.firstBeatSeconds = grid.beatSeconds.front();
    grid.valid = true;
    return grid;
}

} // namespace sa::analysis
