#include <sa/dsp/Dehum.h>
#include <sa/dsp/Window.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

namespace sa::dsp {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

/// Samples in one measurement block, and how many consecutive blocks are
/// grouped to judge coherence.
///
/// The block is about a third of a second at 48 kHz, which resolves partials
/// roughly 7 Hz apart -- not enough on its own to separate a bass note from the
/// hum harmonic beside it, which is what coherence is for. Eight of them span
/// under three seconds: long enough that a note has started and stopped within
/// the group, short enough that the mains has not drifted measurably across it.
constexpr SampleCount kMeasureBlock = 16384;
constexpr int kBlocksPerGroup = 8;

/// What an incoherent signal scores is 1/sqrt(8) = 0.354, and what a steady
/// sinusoid scores is 1. This sits between them, nearer the sinusoid.
constexpr double kCoherenceFloor = 0.65;

/// How far the coherent part of a partial must stand above the coherent part of
/// the spectrum either side of it.
constexpr double kProminenceFloor = 3.0;

/// Average prominence across the searched harmonics before a recording is
/// called hummy at all.
constexpr double kFoundThreshold = 2.0;

/// The phasor is renormalised this often. A unit complex number multiplied by
/// itself sixteen thousand times in double precision drifts in magnitude; every
/// thousand steps is far more often than it needs and costs nothing.
constexpr SampleCount kRenormaliseEvery = 1024;

/// Complex amplitude of `frequency` over a windowed block.
///
/// The phase is referenced to absolute sample zero, not to the start of the
/// block. That is the whole basis of the method below: a steady sinusoid
/// measured in two different blocks comes back with the *same* complex
/// amplitude, while anything else comes back with a different one, so averaging
/// across blocks keeps the first and cancels the second.
///
/// The exponential is stepped by repeated multiplication rather than computed
/// per sample. A sine and a cosine per sample would make this function the cost
/// of the whole feature -- the search evaluates it tens of thousands of times.
[[nodiscard]] std::complex<double> amplitudeAt(const float* samples, SampleIndex start,
                                               SampleCount count, const Window& window,
                                               double frequency, double rate) {
    const double step = kTwoPi * frequency / rate;
    const double reference = step * static_cast<double>(start);
    std::complex<double> phasor{std::cos(reference), -std::sin(reference)};
    const std::complex<double> increment{std::cos(step), -std::sin(step)};

    std::complex<double> total{};
    double weight = 0.0;
    for (SampleCount i = 0; i < count; ++i) {
        const double w = static_cast<double>(window[static_cast<int>(i)]);
        total += phasor * (w * static_cast<double>(samples[start + i]));
        weight += w;
        phasor *= increment;
        if ((i % kRenormaliseEvery) == kRenormaliseEvery - 1) {
            phasor /= std::abs(phasor);
        }
    }
    return weight > 0.0 ? total * (2.0 / weight) : std::complex<double>{};
}

/// What one frequency looks like across a set of block groups.
struct Partial {
    /// Magnitude of the coherent part: the steady sinusoid that survives
    /// averaging across the blocks of a group. This is the hum's level.
    double coherentLevel = 0.0;

    /// How much of the energy at this frequency is that coherent part, from 0
    /// to 1. A steady sinusoid scores 1; noise and notes score about one over
    /// the square root of the group size.
    double coherence = 0.0;
};

/// Blocks are grouped rather than taken across the whole recording, because
/// mains frequency drifts. Over a few seconds the drift is nothing; over ten
/// minutes it is tens of cycles of phase, which would make genuine hum look
/// as incoherent as noise.
[[nodiscard]] Partial measurePartial(const float* samples, SampleCount frames,
                                     const std::vector<SampleIndex>& groups, SampleCount length,
                                     const Window& window, double frequency, double rate,
                                     int blocksPerGroup) {
    if (frequency <= 0.0 || frequency >= rate * 0.5) {
        return {};
    }

    std::vector<double> levels;
    double coherenceTotal = 0.0;
    int counted = 0;

    for (const SampleIndex groupStart : groups) {
        std::complex<double> sum{};
        double magnitudeSum = 0.0;
        int blocks = 0;
        for (int b = 0; b < blocksPerGroup; ++b) {
            const SampleIndex start = groupStart + static_cast<SampleIndex>(b) * length;
            if (start + length > frames) {
                break;
            }
            const std::complex<double> amplitude =
                amplitudeAt(samples, start, length, window, frequency, rate);
            sum += amplitude;
            magnitudeSum += std::abs(amplitude);
            ++blocks;
        }
        if (blocks < 2 || !(magnitudeSum > 0.0)) {
            continue;
        }
        levels.push_back(std::abs(sum) / static_cast<double>(blocks));
        coherenceTotal += std::abs(sum) / magnitudeSum;
        ++counted;
    }

    if (counted == 0) {
        return {};
    }
    std::sort(levels.begin(), levels.end());

    Partial result;
    // The median across groups, so one passage with an organ note in it does
    // not decide the answer for the whole recording.
    result.coherentLevel = levels[levels.size() / 2];
    result.coherence = coherenceTotal / static_cast<double>(counted);
    return result;
}

/// How far a partial's coherent level stands above the coherent level of the
/// spectrum either side of it. Compared like with like: the neighbours are
/// measured exactly the same way, so what is being asked is whether there is a
/// steady sinusoid *here* rather than everywhere.
[[nodiscard]] double prominenceOf(const float* samples, SampleCount frames,
                                  const std::vector<SampleIndex>& groups, SampleCount length,
                                  const Window& window, double frequency, double rate,
                                  int blocksPerGroup, const Partial& partial, bool quick = false) {
    std::vector<double> around;
    const std::vector<double> offsets =
        quick ? std::vector<double>{-14.0, 14.0} : std::vector<double>{-17.0, -11.0, 11.0, 17.0};
    for (const double offset : offsets) {
        const double at = frequency + offset;
        if (at > 0.0 && at < rate * 0.5) {
            around.push_back(
                measurePartial(samples, frames, groups, length, window, at, rate, blocksPerGroup)
                    .coherentLevel);
        }
    }
    if (around.empty()) {
        return 0.0;
    }
    std::sort(around.begin(), around.end());
    const double background = around[around.size() / 2];
    return background > 0.0 ? partial.coherentLevel / background : 0.0;
}

[[nodiscard]] std::vector<SampleIndex> groupStarts(SampleCount frames, SampleCount length,
                                                   int blocksPerGroup, int groups) {
    const SampleCount span = length * static_cast<SampleCount>(blocksPerGroup);
    std::vector<SampleIndex> starts;
    if (frames < span) {
        return starts;
    }
    for (int i = 0; i < groups; ++i) {
        const auto at = static_cast<SampleIndex>(
            static_cast<double>(frames - span) *
            (groups > 1 ? static_cast<double>(i) / static_cast<double>(groups - 1) : 0.0));
        starts.push_back(std::clamp<SampleIndex>(at, 0, frames - span));
    }
    return starts;
}

struct Candidate {
    double frequency = 0.0;
    double evidence = 0.0;
};

/// Evidence that a harmonic series sits at `fundamental`: the average, over the
/// partials looked at, of how far each one's steady component stands above its
/// surroundings. A partial that is present but not steady contributes nothing,
/// which is what stops a bass line harmonically related to the mains frequency
/// from being mistaken for it -- and sooner or later one will be.
[[nodiscard]] double harmonicEvidence(const float* samples, SampleCount frames,
                                      const std::vector<SampleIndex>& groups, SampleCount length,
                                      const Window& window, double fundamental, double rate,
                                      int blocksPerGroup, int harmonics, bool quick = false) {
    constexpr double kCap = 30.0;
    double total = 0.0;
    for (int k = 1; k <= harmonics; ++k) {
        const double frequency = fundamental * static_cast<double>(k);
        if (frequency >= rate * 0.5) {
            break;
        }
        const Partial partial = measurePartial(samples, frames, groups, length, window, frequency,
                                               rate, blocksPerGroup);
        if (partial.coherence < kCoherenceFloor) {
            continue;
        }
        total += std::min(kCap, prominenceOf(samples, frames, groups, length, window, frequency,
                                             rate, blocksPerGroup, partial, quick));
    }
    return total / static_cast<double>(harmonics);
}

/// Finds the mains frequency, or reports that nothing stood out.
///
/// Coarse then fine. A grid is held to a few hundredths of a Hertz in normal
/// operation and half a Hertz at worst, so the sweep is narrow; but the
/// precision has to be high, because by the fortieth harmonic a tenth of a
/// Hertz is four, and four Hertz is a different partial.
[[nodiscard]] Candidate findFundamental(const float* samples, SampleCount frames, double rate) {
    const Window window{WindowType::Hann, static_cast<int>(kMeasureBlock)};

    // One group and two neighbours in the coarse sweep. It has only to choose
    // between the two mains frequencies and land within a tenth of a Hertz;
    // the fine pass then measures the winner properly. Doing the full job at
    // every one of forty candidates made the search five times the cost of
    // everything else here put together.
    const std::vector<SampleIndex> coarseGroups =
        groupStarts(frames, kMeasureBlock, kBlocksPerGroup, 1);
    if (coarseGroups.empty()) {
        return {};
    }

    Candidate best;
    for (const double nominal : {50.0, 60.0}) {
        for (double offset = -kHumSearchSpread; offset <= kHumSearchSpread + 1e-9; offset += 0.1) {
            const double frequency = nominal + offset;
            const double evidence =
                harmonicEvidence(samples, frames, coarseGroups, kMeasureBlock, window, frequency,
                                 rate, kBlocksPerGroup, 4, true);
            if (evidence > best.evidence) {
                best = Candidate{frequency, evidence};
            }
        }
    }
    if (!(best.frequency > 0.0)) {
        return {};
    }

    const std::vector<SampleIndex> fineGroups =
        groupStarts(frames, kMeasureBlock, kBlocksPerGroup, 3);
    Candidate refined{best.frequency,
                      harmonicEvidence(samples, frames, fineGroups, kMeasureBlock, window,
                                       best.frequency, rate, kBlocksPerGroup, kHumSearchHarmonics)};
    for (double offset = -0.09; offset <= 0.09 + 1e-9; offset += 0.01) {
        const double frequency = best.frequency + offset;
        const double evidence =
            harmonicEvidence(samples, frames, fineGroups, kMeasureBlock, window, frequency, rate,
                             kBlocksPerGroup, kHumSearchHarmonics);
        if (evidence > refined.evidence) {
            refined = Candidate{frequency, evidence};
        }
    }
    return refined;
}

} // namespace

Result<DehumReport> dehum(AudioBufferView audio, SampleRate rate, const DehumSettings& settings) {
    if (audio.isEmpty()) {
        return Error{ErrorCode::InvalidArgument, "there is nothing to de-hum"};
    }
    if (!rate.isValid()) {
        return Error{ErrorCode::InvalidArgument, "the sample rate is not usable"};
    }
    if (settings.frequency < 0.0 || settings.frequency >= rate.hz() * 0.5) {
        return Error{ErrorCode::OutOfRange, "the hum frequency is negative or above Nyquist"};
    }
    if (settings.harmonics < 0 || settings.harmonics > 1000) {
        return Error{ErrorCode::OutOfRange, "the harmonic count is outside 0 to 1000"};
    }
    if (settings.blockSize < 1024) {
        return Error{ErrorCode::OutOfRange, "the measurement window is shorter than 1024 samples"};
    }
    if (!(settings.amount >= 0.0) || settings.amount > 1.0) {
        return Error{ErrorCode::OutOfRange, "the amount must be between 0 and 1"};
    }
    if (!(settings.highestHarmonicHz > 0.0)) {
        return Error{ErrorCode::OutOfRange, "the highest harmonic must be above zero"};
    }

    const SampleCount frames = audio.frames();
    const double hz = rate.hz();
    DehumReport report;

    // Detection runs on the first channel. Hum is a property of the wiring, not
    // of the mix, so a per-channel answer would be two estimates of one number
    // -- and if they disagreed there would be no principled way to choose.
    if (settings.frequency > 0.0) {
        report.frequency = settings.frequency;
        report.found = true;
    } else {
        const Candidate found = findFundamental(audio.channel(0), frames, hz);
        report.frequency = found.frequency;
        report.prominence = found.evidence;
        report.found = found.frequency > 0.0 && found.evidence >= kFoundThreshold;
    }
    if (!report.found) {
        return report;
    }

    const double highest = std::min(settings.highestHarmonicHz, hz * 0.5);
    int wanted = settings.harmonics;
    if (wanted <= 0) {
        wanted = static_cast<int>(highest / report.frequency);
    }
    wanted = std::max(1, std::min(wanted, static_cast<int>((hz * 0.5) / report.frequency)));

    // Which of those partials are actually there.
    //
    // Hum falls away with harmonic number and the series does not reach as far
    // as the arithmetic allows: a 50 Hz fundamental permits eighty partials
    // below 4 kHz, and on a real recording perhaps fifteen of them exist.
    // Subtracting the other sixty-five would be subtracting whatever the music
    // had at those frequencies, which is the failure this tool is most able to
    // commit and least likely to be forgiven for.
    const Window window{WindowType::Hann, static_cast<int>(kMeasureBlock)};
    const std::vector<SampleIndex> groups = groupStarts(frames, kMeasureBlock, kBlocksPerGroup, 3);

    std::vector<double> partials;
    for (int k = 1; k <= wanted; ++k) {
        const double frequency = report.frequency * static_cast<double>(k);
        if (frequency >= hz * 0.5) {
            break;
        }
        if (groups.empty()) {
            // Too short to measure coherence over. The series is taken as
            // asked for, which is the only thing left to do and is why the
            // frequency can be given explicitly.
            partials.push_back(frequency);
            continue;
        }
        const Partial partial = measurePartial(audio.channel(0), frames, groups, kMeasureBlock,
                                               window, frequency, hz, kBlocksPerGroup);
        if (partial.coherence >= kCoherenceFloor &&
            prominenceOf(audio.channel(0), frames, groups, kMeasureBlock, window, frequency, hz,
                         kBlocksPerGroup, partial) >= kProminenceFloor) {
            partials.push_back(frequency);
        }
    }

    if (partials.empty()) {
        report.found = false;
        return report;
    }
    report.harmonics = static_cast<int>(partials.size());

    // Subtraction. Each partial's complex amplitude is measured per block and
    // then smoothed across neighbouring blocks before being subtracted: the
    // smoothing is what removes the music from the estimate, because hum has
    // the same complex amplitude in every block and averages coherently while
    // a note does not and averages away.
    const auto blockSize = std::min<SampleCount>(settings.blockSize, frames);
    const SampleCount hop = std::max<SampleCount>(1, blockSize / 4);
    const Window synthesis{WindowType::Hann, static_cast<int>(blockSize)};

    std::vector<SampleIndex> starts;
    for (SampleIndex start = 0; start + blockSize <= frames; start += hop) {
        starts.push_back(start);
    }
    if (starts.empty() || starts.back() + blockSize < frames) {
        starts.push_back(frames - blockSize);
    }

    std::vector<double> estimate(static_cast<std::size_t>(frames));
    std::vector<double> weight(static_cast<std::size_t>(frames));
    std::vector<std::complex<double>> measured(starts.size());
    std::vector<std::complex<double>> smoothed(starts.size());

    double before = 0.0;
    double after = 0.0;

    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        float* samples = audio.channel(channel);
        std::fill(estimate.begin(), estimate.end(), 0.0);
        std::fill(weight.begin(), weight.end(), 0.0);

        for (const double frequency : partials) {
            for (std::size_t b = 0; b < starts.size(); ++b) {
                measured[b] = amplitudeAt(samples, starts[b], blockSize, synthesis, frequency, hz);
            }

            // Four blocks either side, which at the default settings averages
            // over about one and a half seconds.
            constexpr int kSmoothing = 4;
            for (std::size_t b = 0; b < starts.size(); ++b) {
                std::complex<double> sum{};
                int counted = 0;
                for (int d = -kSmoothing; d <= kSmoothing; ++d) {
                    const auto at = static_cast<std::ptrdiff_t>(b) + d;
                    if (at < 0 || at >= static_cast<std::ptrdiff_t>(starts.size())) {
                        continue;
                    }
                    sum += measured[static_cast<std::size_t>(at)];
                    ++counted;
                }
                smoothed[b] =
                    counted > 0 ? sum / static_cast<double>(counted) : std::complex<double>{};
            }

            const double step = kTwoPi * frequency / hz;
            for (std::size_t b = 0; b < starts.size(); ++b) {
                const SampleIndex start = starts[b];
                const double reference = step * static_cast<double>(start);
                std::complex<double> phasor{std::cos(reference), std::sin(reference)};
                const std::complex<double> increment{std::cos(step), std::sin(step)};
                const std::complex<double> amplitude = settings.amount * smoothed[b];

                for (SampleCount i = 0; i < blockSize; ++i) {
                    const double value =
                        amplitude.real() * phasor.real() - amplitude.imag() * phasor.imag();
                    estimate[static_cast<std::size_t>(start + i)] +=
                        static_cast<double>(synthesis[static_cast<int>(i)]) * value;
                    phasor *= increment;
                    if ((i % kRenormaliseEvery) == kRenormaliseEvery - 1) {
                        phasor /= std::abs(phasor);
                    }
                }
            }
        }

        for (const SampleIndex start : starts) {
            for (SampleCount i = 0; i < blockSize; ++i) {
                weight[static_cast<std::size_t>(start + i)] +=
                    static_cast<double>(synthesis[static_cast<int>(i)]);
            }
        }

        for (SampleCount i = 0; i < frames; ++i) {
            const double w = weight[static_cast<std::size_t>(i)];
            const double hum = w > 0.0 ? estimate[static_cast<std::size_t>(i)] / w : 0.0;
            const double was = static_cast<double>(samples[i]);
            const double now = was - hum;
            before += was * was;
            after += now * now;
            samples[i] = static_cast<float>(now);
        }
    }

    report.removedDb = before > 0.0 && after > 0.0 ? 10.0 * std::log10(after / before) : 0.0;
    return report;
}

} // namespace sa::dsp
