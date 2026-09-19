#include <sa/analysis/PitchTrack.h>
#include <sa/dsp/Fft.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace sa::analysis {

namespace {

/// Longest frame we are willing to set a transform up for, in samples.
///
/// Only a guard against settings that are not pitch analysis: at 48 kHz this
/// is a minute and a half in one window, by which point minHz is somewhere
/// below a hundredth of a hertz. Without it a nonsensical minHz asks for an
/// allocation large enough to take the process down.
constexpr SampleCount kLongestFrame = 1 << 22;

/// The lags the settings allow an answer at, in samples.
struct LagRange {
    SampleCount shortest = 0; // From maxHz, the highest pitch accepted.
    SampleCount longest = 0;  // From minHz, the lowest.
};

/// Rounded outward on both ends, so that a pitch exactly on a bound is inside
/// the range rather than a rounding away from it.
[[nodiscard]] LagRange lagRange(SampleRate rate, const PitchSettings& settings) noexcept {
    LagRange range;
    range.shortest = static_cast<SampleCount>(std::floor(rate.hz() / settings.maxHz));
    range.longest = static_cast<SampleCount>(std::ceil(rate.hz() / settings.minHz));
    return range;
}

[[nodiscard]] Status checkSettings(SampleRate rate, const PitchSettings& settings) {
    if (!rate.isValid()) {
        return Error{ErrorCode::InvalidArgument, "pitch detection needs a sample rate"};
    }
    if (!(settings.minHz > 0.0) || !(settings.maxHz > settings.minHz)) {
        return Error{ErrorCode::InvalidArgument,
                     "the pitch range must run from a positive minimum up to a higher maximum"};
    }
    // A period of two samples is the shortest a sampled signal has, so a
    // maximum at or above Nyquist is asking for lags that do not exist.
    if (!(settings.maxHz < rate.hz() * 0.5)) {
        return Error{ErrorCode::InvalidArgument,
                     "the highest pitch searched must be below Nyquist"};
    }
    if (!(settings.threshold > 0.0) || !(settings.threshold < 1.0)) {
        return Error{ErrorCode::InvalidArgument,
                     "the threshold is a fraction between zero and one"};
    }
    if (settings.window < 2 || settings.hop < 1) {
        return Error{ErrorCode::InvalidArgument, "the window and hop must both be positive"};
    }
    return {};
}

/// Smallest transform that holds a frame without the correlation wrapping.
///
/// The circular correlation below only equals the linear one when the padding
/// is long enough that no lag reaches round the end, which needs the transform
/// to be at least as long as window + longest lag.
[[nodiscard]] int transformSize(SampleCount span) noexcept {
    int size = 4;
    while (static_cast<SampleCount>(size) < span) {
        size *= 2;
    }
    return size;
}

/// The lag YIN settled on in one frame, and how well it matched there.
struct Match {
    double lag = 0.0;          // Refined to a fraction of a sample.
    double aperiodicity = 1.0; // The normalised difference at that lag.
    bool inRange = false;      // Whether it is a lag the settings allow.
};

/// One frame of YIN, with its working buffers kept between frames.
///
/// A class rather than a function because a contour is thousands of frames and
/// every one of them needs the same four buffers; rebuilding the transform
/// tables per frame would dominate the cost.
class Yin {
public:
    Yin(SampleCount window, SampleCount longestLag)
        : window_(window), longestLag_(longestLag), transform_(transformSize(window + longestLag)),
          fft_(transform_), padded_(static_cast<std::size_t>(transform_), 0.0f),
          windowSpectrum_(static_cast<std::size_t>(fft_.binCount())),
          frameSpectrum_(static_cast<std::size_t>(fft_.binCount())),
          correlation_(static_cast<std::size_t>(transform_), 0.0f),
          normalised_(static_cast<std::size_t>(longestLag + 1), 1.0) {}

    /// Analyse a frame, which must hold window + longestLag samples.
    [[nodiscard]] Match analyse(const float* frame, double threshold, SampleCount shortestLag) {
        difference(frame);
        return choose(threshold, shortestLag);
    }

private:
    /// Fill normalised_ with the cumulative mean normalised difference.
    void difference(const float* frame) {
        const auto span = window_ + longestLag_;

        // r(lag) = sum over the window of x[j] * x[j + lag], which is the
        // correlation of the window against the whole frame. Taken through the
        // transform because the direct sum costs window * longestLag
        // multiply-adds per frame -- two million at the defaults, for every one
        // of the thousands of hops in a contour.
        std::fill(padded_.begin(), padded_.end(), 0.0f);
        std::copy(frame, frame + window_, padded_.begin());
        fft_.forward(padded_.data(), windowSpectrum_.data());

        std::fill(padded_.begin(), padded_.end(), 0.0f);
        std::copy(frame, frame + span, padded_.begin());
        fft_.forward(padded_.data(), frameSpectrum_.data());

        // Conjugating the first spectrum turns the convolution the product
        // stands for into a correlation. The result lands back in the window's
        // spectrum, which is finished with. Both operands came from real
        // signals, so the product is conjugate-symmetric and the inverse is
        // real, as the real transform requires.
        for (std::size_t bin = 0; bin < windowSpectrum_.size(); ++bin) {
            windowSpectrum_[bin] = std::conj(windowSpectrum_[bin]) * frameSpectrum_[bin];
        }
        fft_.inverse(windowSpectrum_.data(), correlation_.data());

        // The difference function from the paper's identity: the mismatch at a
        // lag is the energy of the window, plus the energy of the window moved
        // along by that lag, less twice their correlation. Both energy terms
        // slide, so each costs one add and one subtract rather than a fresh
        // sum over the window. They are accumulated in double while the
        // correlation is not, which is deliberate: the correlation is the only
        // term the transform touches, and keeping the other two exact holds the
        // cancellation at the period down to the transform's own rounding.
        double windowEnergy = 0.0;
        for (SampleCount j = 0; j < window_; ++j) {
            windowEnergy += static_cast<double>(frame[j]) * static_cast<double>(frame[j]);
        }
        double laggedEnergy = windowEnergy;

        normalised_[0] = 1.0;
        double runningSum = 0.0;
        for (SampleCount lag = 1; lag <= longestLag_; ++lag) {
            const double leaving = static_cast<double>(frame[lag - 1]);
            const double entering = static_cast<double>(frame[lag + window_ - 1]);
            laggedEnergy += entering * entering - leaving * leaving;

            const auto correlated =
                static_cast<double>(correlation_[static_cast<std::size_t>(lag)]);
            // Mathematically a sum of squares and so never negative; the
            // transform's rounding can still put it a hair below zero at the
            // period, where the three terms very nearly cancel.
            const double mismatch = std::max(0.0, windowEnergy + laggedEnergy - 2.0 * correlated);

            runningSum += mismatch;
            // Divided by the mean of every lag up to and including this one.
            // Silence makes that mean zero, and one -- perfectly aperiodic --
            // is the right reading for a frame with nothing in it, rather than
            // the zero that dividing a zero mismatch by itself would suggest.
            normalised_[static_cast<std::size_t>(lag)] =
                runningSum > 0.0 ? mismatch * static_cast<double>(lag) / runningSum : 1.0;
        }
    }

    [[nodiscard]] Match choose(double threshold, SampleCount shortestLag) const {
        Match match;

        // The first dip below the threshold, searched from the shortest lag
        // there is. Starting at one rather than at shortestLag is what the
        // normalisation buys: the curve is held near one where a signal is
        // still trivially correlated with itself, so there is nothing to guard
        // against down there, and a signal pitched above maxHz can be seen for
        // what it is instead of being answered with the first multiple of its
        // period that happens to fall in range.
        SampleCount chosen = 0;
        for (SampleCount lag = 1; lag <= longestLag_; ++lag) {
            if (normalised_[static_cast<std::size_t>(lag)] >= threshold) {
                continue;
            }
            // Down to the bottom of this dip, because the lag that first
            // crosses the threshold is on its shoulder and a shoulder is a
            // fraction of a period out -- at 80 Hz and 48 kHz, fifty samples
            // of it.
            //
            // The dip is taken as the whole run below the threshold, and the
            // answer as its deepest point. Walking downhill from the shoulder
            // and stopping at the first rise is the obvious reading of the
            // rule, and it is wrong in noise: the walk is a dozen lags long,
            // any bump on it ends the walk early, and an early stop is always
            // short of the period rather than past it -- so the error has a
            // direction and does not average out. On a 220 Hz tone at 11 dB
            // signal to noise, that rule put 39 frames out of 83 on a lag of
            // 215 rather than the true 218.2, reading 223.3 Hz: 1.5% sharp,
            // in one direction, from a detector inside 0.5% on the same tone
            // clean. Taking the deepest point of the run instead centres the
            // same frames on 218.
            //
            // It costs nothing in octave safety. The run can only reach the
            // next period down if the curve never climbs back over the
            // threshold in between, and it always does: halfway between a
            // period and twice it a pure tone reaches 2.0, which is more than
            // ten times a threshold of 0.15.
            SampleCount bottom = lag;
            for (SampleCount at = lag;
                 at <= longestLag_ && normalised_[static_cast<std::size_t>(at)] < threshold; ++at) {
                if (normalised_[static_cast<std::size_t>(at)] <
                    normalised_[static_cast<std::size_t>(bottom)]) {
                    bottom = at;
                }
            }
            chosen = bottom;
            break;
        }

        if (chosen > 0) {
            match.lag = refine(chosen);
            match.aperiodicity = normalised_[static_cast<std::size_t>(chosen)];
            match.inRange = chosen >= shortestLag && chosen <= longestLag_;
            return match;
        }

        // Nothing matched well enough anywhere. The best lag inside the range
        // still says how close the frame came, which is the difference between
        // noise and a tone the threshold only just turned away.
        auto best = static_cast<std::size_t>(shortestLag);
        for (auto lag = static_cast<std::size_t>(shortestLag);
             lag <= static_cast<std::size_t>(longestLag_); ++lag) {
            if (normalised_[lag] < normalised_[best]) {
                best = lag;
            }
        }
        match.aperiodicity = normalised_[best];
        return match;
    }

    /// The minimum's position to a fraction of a sample, from a parabola
    /// through it and its two neighbours.
    ///
    /// For a parabola through (-1, a), (0, b), (1, c) the vertex sits at
    /// (a - c) / (2 * (a - 2b + c)). Writing u = a - b and v = c - b, both of
    /// which are non-negative at a minimum, that is (u - v) / (2 * (u + v)),
    /// so the shift can never exceed half a sample and the guard below is for
    /// a lag that is not a minimum at all.
    [[nodiscard]] double refine(SampleCount lag) const {
        const auto at = static_cast<std::size_t>(lag);
        if (lag < 1 || at + 1 >= normalised_.size()) {
            return static_cast<double>(lag);
        }
        const double before = normalised_[at - 1];
        const double here = normalised_[at];
        const double after = normalised_[at + 1];
        const double curvature = before - 2.0 * here + after;
        if (!(curvature > 0.0)) {
            return static_cast<double>(lag);
        }
        const double shift = (before - after) / (2.0 * curvature);
        if (!(std::abs(shift) <= 0.5)) {
            return static_cast<double>(lag);
        }
        return static_cast<double>(lag) + shift;
    }

    SampleCount window_ = 0;
    SampleCount longestLag_ = 0;
    int transform_ = 0;
    dsp::RealFft fft_;
    std::vector<float> padded_;
    std::vector<std::complex<float>> windowSpectrum_;
    std::vector<std::complex<float>> frameSpectrum_;
    std::vector<float> correlation_;
    std::vector<double> normalised_;
};

/// The contour, from a bare pointer. Both public entry points land here.
[[nodiscard]] Result<std::vector<PitchPoint>>
track(const float* samples, SampleCount count, SampleRate rate, const PitchSettings& settings) {
    if (const Status settingsOk = checkSettings(rate, settings); !settingsOk) {
        return settingsOk.error();
    }
    if (samples == nullptr || count <= 0) {
        return Error{ErrorCode::InvalidArgument, "pitch detection needs some audio"};
    }

    const LagRange range = lagRange(rate, settings);
    if (range.shortest < 1 || range.longest < range.shortest) {
        return Error{ErrorCode::InvalidArgument, "the pitch range is narrower than one lag"};
    }
    const SampleCount span = settings.window + range.longest;
    if (span > kLongestFrame) {
        return Error{ErrorCode::InvalidArgument, "the window and lowest pitch ask for a frame "
                                                 "longer than pitch analysis has any use for"};
    }
    if (count < span) {
        return Error{ErrorCode::OutOfRange,
                     "the audio is shorter than one analysis frame, which is the window plus the "
                     "longest lag the lowest pitch implies"};
    }

    Yin yin{settings.window, range.longest};
    std::vector<PitchPoint> contour;
    contour.reserve(static_cast<std::size_t>((count - span) / settings.hop + 1));

    const double halfWindow = static_cast<double>(settings.window) * 0.5;
    for (SampleCount start = 0; start + span <= count; start += settings.hop) {
        const Match match = yin.analyse(samples + start, settings.threshold, range.shortest);

        PitchPoint point;
        point.timeSeconds = (static_cast<double>(start) + halfWindow) / rate.hz();
        point.voiced = match.inRange;
        point.confidence = std::clamp(1.0 - match.aperiodicity, 0.0, 1.0);
        if (match.inRange && match.lag > 0.0) {
            point.hz = rate.hz() / match.lag;
        }
        contour.push_back(point);
    }
    return contour;
}

} // namespace

Result<double> estimatePitch(const float* samples, SampleCount count, SampleRate rate,
                             const PitchSettings& settings) {
    Result<std::vector<PitchPoint>> contour = track(samples, count, rate, settings);
    if (!contour) {
        return contour.error();
    }

    std::vector<double> voiced;
    voiced.reserve(contour.value().size());
    for (const PitchPoint& point : contour.value()) {
        if (point.voiced) {
            voiced.push_back(point.hz);
        }
    }
    if (voiced.empty()) {
        return 0.0;
    }

    const auto middle = static_cast<std::ptrdiff_t>(voiced.size() / 2);
    std::nth_element(voiced.begin(), voiced.begin() + middle, voiced.end());
    return voiced[static_cast<std::size_t>(middle)];
}

Result<std::vector<PitchPoint>> trackPitch(ConstAudioBufferView audio, SampleRate rate,
                                           const PitchSettings& settings, int channel) {
    if (channel < 0 || channel >= audio.channelCount()) {
        return Error{ErrorCode::OutOfRange, "channel index outside the audio"};
    }
    return track(audio.channel(channel), audio.frames(), rate, settings);
}

} // namespace sa::analysis
