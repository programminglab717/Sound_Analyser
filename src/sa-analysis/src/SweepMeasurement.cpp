#include <sa/analysis/SweepMeasurement.h>
#include <sa/dsp/Fft.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

namespace sa::analysis {

namespace {

struct Plan {
    double startHz = 0.0;
    double endHz = 0.0;
    SampleCount frames = 0;
    SampleCount fadeFrames = 0;
    double k = 0.0; // ln(endHz / startHz)
};

[[nodiscard]] Result<Plan> planFor(SampleRate rate, const SweepSettings& settings) {
    if (!(rate.hz() > 0.0)) {
        return Error{ErrorCode::InvalidArgument, "a sweep needs a sample rate"};
    }
    if (!(settings.seconds > 0.0)) {
        return Error{ErrorCode::InvalidArgument, "a sweep needs a positive duration"};
    }
    Plan plan;
    const double nyquist = rate.hz() * 0.5;
    plan.startHz = std::clamp(settings.startHz, 1.0, nyquist * 0.99);
    plan.endHz = std::clamp(settings.endHz, plan.startHz * 1.001, nyquist * 0.99);
    if (!(plan.endHz > plan.startHz)) {
        return Error{ErrorCode::InvalidArgument, "the sweep must rise"};
    }
    plan.frames = static_cast<SampleCount>(settings.seconds * rate.hz());
    if (plan.frames < 16) {
        return Error{ErrorCode::InvalidArgument, "the sweep is too short to be one"};
    }
    plan.fadeFrames = std::clamp<SampleCount>(
        static_cast<SampleCount>(std::max(0.0, settings.fadeSeconds) * rate.hz()), 0,
        plan.frames / 2);
    plan.k = std::log(plan.endHz / plan.startHz);
    return plan;
}

/// Raised-cosine fades at both ends.
void applyFades(float* samples, SampleCount frames, SampleCount fade) noexcept {
    for (SampleCount i = 0; i < fade; ++i) {
        const auto shape =
            static_cast<float>(0.5 - 0.5 * std::cos(std::numbers::pi * static_cast<double>(i) /
                                                    static_cast<double>(fade)));
        samples[i] *= shape;
        samples[frames - 1 - i] *= shape;
    }
}

/// Convolve by transform. Both inputs are whole signals rather than streams, so
/// one pair of transforms does it.
[[nodiscard]] std::vector<float> convolve(const float* a, SampleCount aCount, const float* b,
                                          SampleCount bCount) {
    const SampleCount wanted = aCount + bCount - 1;
    int size = 4;
    while (static_cast<SampleCount>(size) < wanted) {
        size *= 2;
    }

    const dsp::RealFft fft{size};
    const auto bins = static_cast<std::size_t>(fft.binCount());
    std::vector<float> padded(static_cast<std::size_t>(size), 0.0f);
    std::vector<std::complex<float>> spectrumA(bins);
    std::vector<std::complex<float>> spectrumB(bins);

    std::copy_n(a, aCount, padded.begin());
    fft.forward(padded.data(), spectrumA.data());

    std::fill(padded.begin(), padded.end(), 0.0f);
    std::copy_n(b, bCount, padded.begin());
    fft.forward(padded.data(), spectrumB.data());

    for (std::size_t bin = 0; bin < bins; ++bin) {
        spectrumA[bin] *= spectrumB[bin];
    }

    std::vector<float> out(static_cast<std::size_t>(size));
    fft.inverse(spectrumA.data(), out.data());
    out.resize(static_cast<std::size_t>(wanted));
    return out;
}

} // namespace

Result<AudioBuffer> generateSweep(SampleRate rate, const SweepSettings& settings) {
    auto plan = planFor(rate, settings);
    if (!plan) {
        return plan.error();
    }
    const Plan& p = plan.value();

    AudioBuffer out{ChannelLayout::mono(), p.frames};
    const double seconds = static_cast<double>(p.frames) / rate.hz();
    // Phase of an exponential sweep: the integral of an instantaneous frequency
    // that rises geometrically, which is what makes every octave take the same
    // time and gives the technique its distortion-separating property.
    const double scale = 2.0 * std::numbers::pi * p.startHz * seconds / p.k;
    for (SampleCount i = 0; i < p.frames; ++i) {
        const double t = static_cast<double>(i) / rate.hz();
        const double phase = scale * (std::exp(t * p.k / seconds) - 1.0);
        out.channel(0)[i] = static_cast<float>(settings.amplitude * std::sin(phase));
    }
    applyFades(out.channel(0), p.frames, p.fadeFrames);
    return out;
}

Result<AudioBuffer> sweepInverseFilter(SampleRate rate, const SweepSettings& settings) {
    auto sweep = generateSweep(rate, settings);
    if (!sweep) {
        return sweep.error();
    }
    auto plan = planFor(rate, settings);
    if (!plan) {
        return plan.error();
    }
    const Plan& p = plan.value();
    const double seconds = static_cast<double>(p.frames) / rate.hz();

    AudioBuffer out{ChannelLayout::mono(), p.frames};
    const float* forward = sweep.value().channel(0);
    for (SampleCount i = 0; i < p.frames; ++i) {
        // Reversed, then tilted back up towards the high end. The sweep passes
        // through high frequencies quickly and low ones slowly, so it leaves
        // an energy density falling as 1/f and a magnitude spectrum falling as
        // 1/sqrt(f). Reversal alone gives the conjugate, so sweep times filter
        // would come out as |X(f)| squared -- still 1/f, a low-pass, not an
        // impulse. The envelope has to supply a gain proportional to f to
        // cancel that.
        //
        // Index i of the reversed sweep holds the moment (T - t) of the
        // forward one, whose instantaneous frequency is f1*exp(k*(T - i/rate)/T).
        // A gain proportional to that, normalised to one at the top, is
        // exp(-i*k/(rate*T)) -- decaying in i, so the high frequencies that sit
        // at the front of the reversed sweep keep full weight and the low ones
        // at the back are held down by f1/f2.
        const double t = static_cast<double>(i) / rate.hz();
        const double envelope = std::exp(-t * p.k / seconds);
        out.channel(0)[i] = static_cast<float>(forward[p.frames - 1 - i] * envelope);
    }

    // Normalised so that sweep convolved with filter peaks at one. Doing it by
    // measurement rather than by deriving a constant: the fades and the
    // clamping above both change the scale, and a formula that ignores them is
    // a formula that is quietly wrong.
    const std::vector<float> impulse = convolve(forward, p.frames, out.channel(0), p.frames);
    double peak = 0.0;
    for (const float value : impulse) {
        peak = std::max(peak, std::abs(static_cast<double>(value)));
    }
    if (peak > 0.0) {
        const auto gain = static_cast<float>(1.0 / peak);
        for (SampleCount i = 0; i < p.frames; ++i) {
            out.channel(0)[i] *= gain;
        }
    }
    return out;
}

Result<AudioBuffer> deconvolveSweep(ConstAudioBufferView recorded, SampleRate rate,
                                    const SweepSettings& settings, double keepSeconds,
                                    int channel) {
    if (channel < 0 || channel >= recorded.channelCount()) {
        return Error{ErrorCode::OutOfRange, "channel index outside the recording"};
    }
    if (recorded.frames() <= 0) {
        return Error{ErrorCode::InvalidArgument, "nothing was recorded"};
    }
    auto filter = sweepInverseFilter(rate, settings);
    if (!filter) {
        return filter.error();
    }
    auto plan = planFor(rate, settings);
    if (!plan) {
        return plan.error();
    }
    const SampleCount sweepFrames = plan.value().frames;

    // A recording shorter than the sweep cannot hold the whole sweep, so the
    // settings it is being deconvolved against are not the ones it was made
    // with. That is not a smaller measurement, it is a different one, and
    // returning a plausible-looking impulse response for it would be worse
    // than refusing.
    if (recorded.frames() < sweepFrames) {
        return Error{ErrorCode::InvalidArgument,
                     "the recording is shorter than the sweep it is being deconvolved against"};
    }

    const std::vector<float> full = convolve(recorded.channel(channel), recorded.frames(),
                                             filter.value().channel(0), sweepFrames);

    // The linear impulse response begins where the sweep's own length has been
    // consumed. Everything before that point deconvolved to negative time --
    // the harmonic distortion products the exponential sweep exists to
    // separate -- and is discarded rather than folded into the answer.
    const SampleIndex origin = sweepFrames - 1;
    // How much of what follows the origin is actually a measurement.
    //
    // A recording of length R is the sweep of length N through an impulse
    // response of length L, so R = N + L - 1 and the response can only be
    // L = R - N + 1 samples long. The convolution runs on for N - 1 samples
    // past that, but those samples carry no information about the thing being
    // measured: they are the residual of (sweep * filter - delta), the
    // deconvolution's own imperfection. Returning them would pad every answer
    // with a stretch of plausible-looking noise the room never made.
    const SampleCount availableAfter = recorded.frames() - sweepFrames + 1;
    const SampleCount keep =
        keepSeconds > 0.0
            ? std::min(availableAfter, static_cast<SampleCount>(keepSeconds * rate.hz()))
            : availableAfter;

    AudioBuffer out{ChannelLayout::mono(), keep};
    std::copy_n(full.begin() + origin, keep, out.channel(0));
    return out;
}

} // namespace sa::analysis
