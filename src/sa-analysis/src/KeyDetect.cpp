#include <sa/analysis/KeyDetect.h>
#include <sa/dsp/Fft.h>
#include <sa/dsp/Window.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

namespace sa::analysis {

namespace {

constexpr std::array<std::string_view, 12> kNames{"C",  "C#", "D",  "D#", "E",  "F",
                                                  "F#", "G",  "G#", "A",  "A#", "B"};

/// What a key is expected to sound like, as a weight per scale degree.
///
/// Degree 0 is the tonic. These are written from the theory rather than taken
/// from a listening experiment, and the reasoning is:
///
///   - The tonic triad -- degrees 0, 3 or 4, and 7 -- is what a key is built
///     on and what a piece in that key keeps returning to, so it carries the
///     most weight. The tonic leads, the fifth follows it, the third is third
///     because it is the degree most often coloured or borrowed.
///   - The rest of the scale is present but unremarkable, so it gets a middle
///     weight; what distinguishes a key from its neighbours is mostly which
///     notes are *in* the scale at all.
///   - Notes outside the scale are not absent from real music -- passing
///     tones, secondary dominants and modal borrowing all put them there -- so
///     they get a small weight rather than zero. Zero would make any
///     chromaticism at all count as evidence against the right answer.
///
/// Minor is the messier of the two because it has three forms. Degree 10 is the
/// natural minor's flat seventh and degree 11 the harmonic minor's leading
/// note; both are common, so both are in, sharing roughly what one scale degree
/// would have had.
constexpr std::array<double, 12> kMajorProfile{
    6.0, // tonic
    1.9, // flat second, outside
    2.6, // second
    1.6, // minor third, outside, though a borrowed one is not rare
    3.8, // major third, in the triad
    2.6, // fourth
    1.6, // tritone, outside
    4.6, // fifth, in the triad
    1.6, // flat sixth, outside
    2.6, // sixth
    1.9, // flat seventh, outside but common in mixolydian-flavoured writing
    2.4, // leading note
};

constexpr std::array<double, 12> kMinorProfile{
    6.0, // tonic
    1.7, // flat second, outside
    2.5, // second
    3.8, // minor third, in the triad
    1.7, // major third, outside, though the picardy third exists
    2.5, // fourth
    1.6, // tritone, outside
    4.6, // fifth, in the triad
    2.5, // flat sixth
    1.8, // sixth, from the melodic minor
    2.4, // flat seventh, from the natural minor
    2.2, // leading note, from the harmonic minor
};

/// Pearson correlation. Both sides are centred, so a profile that is uniformly
/// louder than another does not score differently for that reason alone --
/// which matters, because the chromagram's absolute scale depends on the
/// recording level and says nothing about the key.
[[nodiscard]] double correlate(const std::array<double, 12>& a,
                               const std::array<double, 12>& b) noexcept {
    const double meanA = std::accumulate(a.begin(), a.end(), 0.0) / 12.0;
    const double meanB = std::accumulate(b.begin(), b.end(), 0.0) / 12.0;
    double covariance = 0.0;
    double varianceA = 0.0;
    double varianceB = 0.0;
    for (std::size_t i = 0; i < 12; ++i) {
        const double da = a[i] - meanA;
        const double db = b[i] - meanB;
        covariance += da * db;
        varianceA += da * da;
        varianceB += db * db;
    }
    if (!(varianceA > 0.0) || !(varianceB > 0.0)) {
        return 0.0;
    }
    return covariance / std::sqrt(varianceA * varianceB);
}

struct Analysis {
    std::array<double, 12> chroma{};
    double tuningCents = 0.0;
    double totalWeight = 0.0;
};

[[nodiscard]] Result<Analysis> analyse(ConstAudioBufferView audio, SampleRate rate,
                                       const KeySettings& settings, int channel) {
    if (channel < 0 || channel >= audio.channelCount()) {
        return Error{ErrorCode::OutOfRange, "channel index outside the audio"};
    }
    if (!(rate.hz() > 0.0)) {
        return Error{ErrorCode::InvalidArgument, "a chromagram needs a sample rate"};
    }
    if (!dsp::RealFft::isSupportedSize(settings.fftSize)) {
        return Error{ErrorCode::InvalidArgument, "the transform size must be a power of two"};
    }
    if (settings.hop <= 0) {
        return Error{ErrorCode::InvalidArgument, "the hop must be positive"};
    }
    if (!(settings.highHz > settings.lowHz) || !(settings.lowHz > 0.0)) {
        return Error{ErrorCode::InvalidArgument, "the range must rise"};
    }
    const auto size = static_cast<SampleCount>(settings.fftSize);
    if (audio.frames() < size) {
        return Error{ErrorCode::InvalidArgument, "shorter than one analysis window"};
    }

    const dsp::RealFft fft{settings.fftSize};
    const dsp::Window window{dsp::WindowType::Hann, settings.fftSize};
    const auto bins = static_cast<std::size_t>(fft.binCount());
    std::vector<float> frame(static_cast<std::size_t>(settings.fftSize));
    std::vector<std::complex<float>> spectrum(bins);

    // The bins worth looking at, worked out once. Below lowHz a note's own
    // partials outweigh its fundamental and the fold puts energy on the wrong
    // pitch class; above highHz almost everything is a harmonic of something
    // already counted.
    const double binHz = rate.hz() / static_cast<double>(settings.fftSize);
    const auto firstBin =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(settings.lowHz / binHz)));
    const auto lastBin =
        std::min<std::size_t>(bins - 1, static_cast<std::size_t>(settings.highHz / binHz));
    if (firstBin >= lastBin) {
        return Error{ErrorCode::InvalidArgument, "the range holds no bins at this size"};
    }

    Analysis result;
    // Tuning is accumulated as a vector rather than a mean of cent deviations,
    // because deviation is circular: -49 cents and +49 cents are two cents
    // apart, and averaging them as numbers gives zero, the one answer that is
    // certainly wrong.
    double tuningX = 0.0;
    double tuningY = 0.0;

    const float* samples = audio.channel(channel);
    for (SampleCount start = 0; start + size <= audio.frames(); start += settings.hop) {
        for (SampleCount i = 0; i < size; ++i) {
            frame[static_cast<std::size_t>(i)] = samples[start + i] * window[static_cast<int>(i)];
        }
        fft.forward(frame.data(), spectrum.data());

        for (std::size_t bin = firstBin; bin <= lastBin; ++bin) {
            const double magnitude = std::abs(spectrum[bin]);
            if (!(magnitude > 0.0)) {
                continue;
            }
            const double hz = static_cast<double>(bin) * binHz;
            // Semitones above C0 at A = 440. The 69 and the 12 are the MIDI
            // convention; subtracting nothing and taking the remainder gives
            // the pitch class directly, since MIDI note 0 is a C.
            const double semitone = 69.0 + 12.0 * std::log2(hz / 440.0);
            const double nearest = std::round(semitone);
            const double cents = (semitone - nearest) * 100.0;

            auto pitchClass = static_cast<int>(std::fmod(nearest, 12.0));
            if (pitchClass < 0) {
                pitchClass += 12;
            }
            result.chroma[static_cast<std::size_t>(pitchClass)] += magnitude;
            result.totalWeight += magnitude;

            // Weighted by magnitude, so a loud note in tune counts for more
            // than a quiet bin of noise between two semitones.
            const double angle = 2.0 * std::numbers::pi * cents / 100.0;
            tuningX += magnitude * std::cos(angle);
            tuningY += magnitude * std::sin(angle);
        }
    }

    if (!(result.totalWeight > 0.0)) {
        return Error{ErrorCode::InvalidArgument, "the audio is silent"};
    }
    for (double& value : result.chroma) {
        value /= result.totalWeight;
    }
    result.tuningCents = std::atan2(tuningY, tuningX) * 100.0 / (2.0 * std::numbers::pi);
    return result;
}

} // namespace

std::string_view pitchClassName(int pitchClass) noexcept {
    if (pitchClass < 0 || pitchClass >= 12) {
        return {};
    }
    return kNames[static_cast<std::size_t>(pitchClass)];
}

std::string keyName(const KeyEstimate& estimate) {
    if (!estimate.valid) {
        return {};
    }
    return std::string{pitchClassName(estimate.tonic)} +
           (estimate.mode == Mode::Major ? " major" : " minor");
}

Result<std::array<double, 12>> chromagram(ConstAudioBufferView audio, SampleRate rate,
                                          const KeySettings& settings, int channel) {
    auto analysed = analyse(audio, rate, settings, channel);
    if (!analysed) {
        return analysed.error();
    }
    return analysed.value().chroma;
}

Result<KeyEstimate> detectKey(ConstAudioBufferView audio, SampleRate rate,
                              const KeySettings& settings, int channel) {
    auto analysed = analyse(audio, rate, settings, channel);
    if (!analysed) {
        return analysed.error();
    }

    KeyEstimate best;
    best.chroma = analysed.value().chroma;
    best.tuningOffsetCents = analysed.value().tuningCents;

    // Twenty-four candidates: each profile rotated to each tonic. Rotating the
    // profile rather than the chroma keeps the returned chroma in absolute
    // pitch classes, which is what a caller wants to look at.
    double bestScore = -2.0;
    double secondScore = -2.0;
    for (int tonic = 0; tonic < 12; ++tonic) {
        for (const Mode mode : {Mode::Major, Mode::Minor}) {
            const auto& profile = mode == Mode::Major ? kMajorProfile : kMinorProfile;
            std::array<double, 12> rotated{};
            for (int degree = 0; degree < 12; ++degree) {
                rotated[static_cast<std::size_t>((tonic + degree) % 12)] =
                    profile[static_cast<std::size_t>(degree)];
            }
            const double score = correlate(analysed.value().chroma, rotated);
            if (score > bestScore) {
                secondScore = bestScore;
                best.runnerUpTonic = best.tonic;
                best.runnerUpMode = best.mode;
                bestScore = score;
                best.tonic = tonic;
                best.mode = mode;
            } else if (score > secondScore) {
                secondScore = score;
                best.runnerUpTonic = tonic;
                best.runnerUpMode = mode;
            }
        }
    }

    best.fit = bestScore;
    best.margin = bestScore - secondScore;

    // How shaped the chroma is at all, which the correlation above cannot
    // see: centring both sides is what makes a correlation a correlation, and
    // it is also what throws this away.
    const double mean = std::accumulate(best.chroma.begin(), best.chroma.end(), 0.0) / 12.0;
    double variance = 0.0;
    for (const double value : best.chroma) {
        variance += (value - mean) * (value - mean);
    }
    best.contrast = mean > 0.0 ? std::sqrt(variance / 12.0) / mean : 0.0;
    best.strength = bestScore * std::min(1.0, best.contrast);

    best.valid = true;
    return best;
}

} // namespace sa::analysis
