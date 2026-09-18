#include <sa/dsp/Resampler.h>
#include <sa/dsp/Stft.h>
#include <sa/spectral/TimeStretch.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

namespace sa::spectral {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

/// Any rate would do. The converter is built round a pair of rates, but only
/// ever uses their quotient, so a pitch shift can hand it any pair in the right
/// proportion. 48 kHz is chosen because scaled by the widest shift allowed it
/// still lands inside the rates the type accepts.
constexpr double kReferenceRate = 48000.0;

/// Principal value of an angle, in (-pi, pi].
[[nodiscard]] double wrapPhase(double radians) noexcept {
    radians = std::fmod(radians + std::numbers::pi, kTwoPi);
    if (radians < 0.0) {
        radians += kTwoPi;
    }
    return radians - std::numbers::pi;
}

/// Which peak each bin belongs to.
///
/// A sinusoid does not occupy one bin. A Hann window spreads it over three or
/// four, and those bins are not independent observations -- they are one thing
/// seen through a window, and their phases are locked to each other in the
/// source. Grouping them is what makes it possible to keep them locked in the
/// output too.
///
/// The boundary between two peaks is the quietest bin between them, where the
/// two partials' skirts cross, rather than the midpoint: an intense partial
/// beside a weak one owns more of the gap, and it should.
void assignPeaks(const std::vector<double>& magnitude, std::vector<int>& owner,
                 std::vector<int>& peaks) {
    const auto bins = static_cast<int>(magnitude.size());
    peaks.clear();
    for (int bin = 1; bin + 1 < bins; ++bin) {
        if (magnitude[static_cast<std::size_t>(bin)] >
                magnitude[static_cast<std::size_t>(bin - 1)] &&
            magnitude[static_cast<std::size_t>(bin)] >
                magnitude[static_cast<std::size_t>(bin + 1)]) {
            peaks.push_back(bin);
        }
    }
    if (peaks.empty()) {
        // Silence, or a spectrum so flat there is nothing to lock to. Locking
        // everything to one arbitrary bin would be worse than not locking.
        std::fill(owner.begin(), owner.end(), -1);
        return;
    }

    std::fill(owner.begin(), owner.begin() + peaks.front() + 1, peaks.front());
    for (std::size_t i = 0; i + 1 < peaks.size(); ++i) {
        const int left = peaks[i];
        const int right = peaks[i + 1];
        int valley = left + 1;
        for (int bin = left + 2; bin < right; ++bin) {
            if (magnitude[static_cast<std::size_t>(bin)] <
                magnitude[static_cast<std::size_t>(valley)]) {
                valley = bin;
            }
        }
        std::fill(owner.begin() + left + 1, owner.begin() + valley + 1, left);
        std::fill(owner.begin() + valley + 1, owner.begin() + right + 1, right);
    }
    std::fill(owner.begin() + peaks.back() + 1, owner.end(), peaks.back());
}

} // namespace

double pitchRatio(double semitones) noexcept {
    return std::pow(2.0, semitones / 12.0);
}

Result<AudioBuffer> timeStretch(const AudioBuffer& audio, const StretchSettings& settings) {
    if (audio.isEmpty()) {
        return Error{ErrorCode::InvalidArgument, "there is nothing to stretch"};
    }
    if (!std::isfinite(settings.factor) || settings.factor < stretch::kMinimumFactor ||
        settings.factor > stretch::kMaximumFactor) {
        return Error{ErrorCode::OutOfRange, "stretch factor is outside 0.1 to 10"};
    }

    auto made = dsp::Stft::create(settings.fftSize, settings.hopSize, settings.window);
    if (!made) {
        return made.error();
    }
    const dsp::Stft& stft = made.value();

    const auto bins = static_cast<std::size_t>(stft.binCount());
    const SampleCount inputFrames = stft.frameCount(audio.frames());
    const auto outCount =
        std::max<SampleCount>(1, static_cast<SampleCount>(std::llround(
                                     static_cast<double>(audio.frames()) * settings.factor)));
    const SampleCount outputFrames = stft.frameCount(outCount);
    if (inputFrames <= 0 || outputFrames <= 0) {
        return Error{ErrorCode::InvalidArgument, "there is nothing to stretch"};
    }

    AudioBuffer result{audio.layout(), outCount};

    std::vector<std::complex<float>> analysis(static_cast<std::size_t>(inputFrames) * bins);
    std::vector<std::complex<float>> synthesis(static_cast<std::size_t>(outputFrames) * bins);
    std::vector<double> magnitude(bins);
    std::vector<double> phase(bins);
    std::vector<int> owner(bins);
    std::vector<int> peaks;

    // The phase advance one analysis hop earns at each bin's own centre
    // frequency. What the signal actually does is measured against this.
    std::vector<double> expected(bins);
    for (std::size_t bin = 0; bin < bins; ++bin) {
        expected[bin] = kTwoPi * static_cast<double>(bin) * static_cast<double>(settings.hopSize) /
                        static_cast<double>(settings.fftSize);
    }

    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        stft.analyse(audio.channel(channel), audio.frames(), analysis.data());

        for (SampleCount frame = 0; frame < outputFrames; ++frame) {
            // Where in the analysis this output frame reads from. Mapping the
            // two frame ranges onto each other end to end, rather than stepping
            // by 1/factor, puts the last output frame on the last analysis
            // frame exactly however the length rounding went -- so the end of
            // the material is the end of the material, not a frame short of it.
            const double position = outputFrames > 1 ? static_cast<double>(frame) *
                                                           static_cast<double>(inputFrames - 1) /
                                                           static_cast<double>(outputFrames - 1)
                                                     : 0.0;
            const auto left = static_cast<SampleCount>(position);
            const SampleCount right = std::min(left + 1, inputFrames - 1);
            const double t = position - static_cast<double>(left);

            const std::complex<float>* a = analysis.data() + static_cast<std::size_t>(left) * bins;
            const std::complex<float>* b = analysis.data() + static_cast<std::size_t>(right) * bins;
            // Magnitude interpolates between frames; phase does not, because a
            // phase halfway between two angles is not a halfway phase. Where a
            // phase is needed as a reference rather than as a rate, the nearer
            // frame's is the one to use.
            const std::complex<float>* nearest = t < 0.5 ? a : b;

            for (std::size_t bin = 0; bin < bins; ++bin) {
                magnitude[bin] = (1.0 - t) * static_cast<double>(std::abs(a[bin])) +
                                 t * static_cast<double>(std::abs(b[bin]));
            }

            if (frame == 0) {
                // Start from the source's own phase, so an onset at sample zero
                // comes out as an onset rather than as whatever the vocoder
                // would have invented.
                for (std::size_t bin = 0; bin < bins; ++bin) {
                    phase[bin] = std::arg(a[bin]);
                }
            } else {
                for (std::size_t bin = 0; bin < bins; ++bin) {
                    // The step the signal actually took between these two
                    // analysis frames, unwrapped against the step this bin's
                    // centre frequency would have taken. The difference is
                    // where the partial really is, to far better than bin
                    // spacing, and advancing by it is what keeps a held note
                    // held instead of beating.
                    const double step = static_cast<double>(std::arg(b[bin])) -
                                        static_cast<double>(std::arg(a[bin]));
                    phase[bin] += expected[bin] + wrapPhase(step - expected[bin]);
                }

                if (settings.lockPhases) {
                    assignPeaks(magnitude, owner, peaks);
                    for (std::size_t bin = 0; bin < bins; ++bin) {
                        const int lead = owner[bin];
                        if (lead < 0 || static_cast<std::size_t>(lead) == bin) {
                            continue;
                        }
                        // The bin keeps the offset from its peak that it has in
                        // the source. Every bin describing one partial then
                        // moves as one thing, which is what it is.
                        phase[bin] = phase[static_cast<std::size_t>(lead)] +
                                     (static_cast<double>(std::arg(nearest[bin])) -
                                      static_cast<double>(
                                          std::arg(nearest[static_cast<std::size_t>(lead)])));
                    }
                }
            }

            std::complex<float>* out = synthesis.data() + static_cast<std::size_t>(frame) * bins;
            for (std::size_t bin = 0; bin < bins; ++bin) {
                // Wrapped as it is stored: nothing downstream cares which turn
                // of the circle it is on, and a phase left to grow for an hour
                // is a phase computed by sin() of a large number.
                phase[bin] = wrapPhase(phase[bin]);
                out[bin] =
                    std::polar(static_cast<float>(magnitude[bin]), static_cast<float>(phase[bin]));
            }
        }

        stft.synthesise(synthesis.data(), outputFrames, result.channel(channel), outCount);
    }

    return result;
}

Result<AudioBuffer> pitchShift(const AudioBuffer& audio, const PitchSettings& settings) {
    if (audio.isEmpty()) {
        return Error{ErrorCode::InvalidArgument, "there is nothing to shift"};
    }
    if (!std::isfinite(settings.semitones) || settings.semitones < stretch::kMinimumSemitones ||
        settings.semitones > stretch::kMaximumSemitones) {
        return Error{ErrorCode::OutOfRange, "pitch shift is outside three octaves either way"};
    }

    const double ratio = pitchRatio(settings.semitones);

    StretchSettings stretching = settings.stretch;
    stretching.factor = ratio;
    auto stretched = timeStretch(audio, stretching);
    if (!stretched) {
        return stretched.error();
    }
    const AudioBuffer& source = stretched.value();

    // Undo the length change by resampling, which takes the pitch with it: the
    // same waveform read faster is the same waveform higher up.
    dsp::ResamplerSpec spec;
    spec.inputRate = SampleRate{kReferenceRate * ratio};
    spec.outputRate = SampleRate{kReferenceRate};
    spec.quality = settings.quality;

    AudioBuffer result{audio.layout(), audio.frames()};
    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        auto converter = dsp::Resampler::create(spec);
        if (!converter) {
            return converter.error();
        }
        dsp::Resampler& resampler = converter.value();

        const float* input = source.channel(channel);
        float* output = result.channel(channel);
        SampleCount consumed = 0;
        SampleCount produced = 0;
        while (consumed < source.frames() && produced < result.frames()) {
            const dsp::ResamplerProgress progress =
                resampler.process(input + consumed, source.frames() - consumed, output + produced,
                                  result.frames() - produced);
            if (progress.inputConsumed == 0 && progress.outputProduced == 0) {
                break;
            }
            consumed += progress.inputConsumed;
            produced += progress.outputProduced;
        }
        while (produced < result.frames()) {
            const SampleCount drained =
                resampler.flush(output + produced, result.frames() - produced);
            if (drained == 0) {
                break;
            }
            produced += drained;
        }

        // The conversion can land a sample or two short of the requested length
        // through rounding. What is missing is silence, not whatever the buffer
        // happened to hold.
        std::fill(output + produced, output + result.frames(), 0.0f);
    }

    return result;
}

} // namespace sa::spectral
