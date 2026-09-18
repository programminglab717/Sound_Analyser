#include <sa/dsp/Stft.h>
#include <sa/spectral/SpectralEdit.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

namespace sa::spectral {

namespace {

/// How far either side of the region the analysis has to reach.
///
/// A frame whose window overlaps the region at all carries some of it, so the
/// processed span has to include every such frame in full or the edit's own
/// spread is clipped and the seam clicks.
[[nodiscard]] SampleCount contextFor(const SpectralEditSettings& settings) noexcept {
    return settings.fftSize;
}

/// Raised-cosine taper: 0 at `distance` 0, 1 at `width`. Used at every edge of
/// the mask, in both axes, for the same reason.
[[nodiscard]] float taper(double distance, double width) noexcept {
    if (width <= 0.0) {
        return 1.0f;
    }
    const double t = std::clamp(distance / width, 0.0, 1.0);
    return static_cast<float>(0.5 - 0.5 * std::cos(std::numbers::pi * t));
}

struct Plan {
    dsp::Stft stft;
    SampleIndex spanStart = 0;
    SampleCount spanLength = 0;
    SampleCount frames = 0;
    int bins = 0;
    double binHz = 0.0;
    double frequencyFeatherHz = 0.0;
    SampleCount timeFeather = 0;
};

[[nodiscard]] Result<Plan> makePlan(const AudioBufferView& audio, SampleRate rate,
                                    const SpectralRegion& region,
                                    const SpectralEditSettings& settings) {
    if (region.isEmpty()) {
        return Error{ErrorCode::InvalidArgument, "region is empty"};
    }
    if (audio.isEmpty()) {
        return Error{ErrorCode::InvalidArgument, "no audio"};
    }
    if (rate.hz() <= 0.0) {
        return Error{ErrorCode::InvalidArgument, "sample rate must be positive"};
    }
    if (region.startSample < 0 || region.endSample > audio.frames()) {
        return Error{ErrorCode::OutOfRange, "region falls outside the audio"};
    }

    auto stft = dsp::Stft::create(settings.fftSize, settings.hopSize, settings.window);
    if (!stft) {
        return stft.error();
    }

    const SampleCount context = contextFor(settings);
    Plan plan{std::move(stft).value()};
    plan.spanStart = std::max<SampleIndex>(0, region.startSample - context);
    const SampleIndex spanEnd = std::min<SampleIndex>(audio.frames(), region.endSample + context);
    plan.spanLength = spanEnd - plan.spanStart;
    plan.frames = plan.stft.frameCount(plan.spanLength);
    plan.bins = plan.stft.binCount();
    plan.binHz = rate.hz() / settings.fftSize;

    plan.frequencyFeatherHz =
        settings.frequencyFeatherHz > 0.0 ? settings.frequencyFeatherHz : 3.0 * plan.binHz;
    plan.timeFeather =
        settings.timeFeather > 0 ? settings.timeFeather : SampleCount{settings.hopSize};
    return plan;
}

/// Centre of an analysis frame, in samples from the start of the analysed span.
///
/// analyse() pads the front by fftSize - hopSize, so frame f starts at
/// f * hop - padding and its centre is half a window later.
[[nodiscard]] double frameCentre(const dsp::Stft& stft, SampleCount frame) noexcept {
    const auto padding = static_cast<double>(stft.fftSize() - stft.hopSize());
    return static_cast<double>(frame) * stft.hopSize() - padding + stft.fftSize() * 0.5;
}

/// Principal value of an angle, in (-pi, pi].
[[nodiscard]] double wrapPhase(double radians) noexcept {
    const auto twoPi = 2.0 * std::numbers::pi;
    radians = std::fmod(radians + std::numbers::pi, twoPi);
    if (radians < 0.0) {
        radians += twoPi;
    }
    return radians - std::numbers::pi;
}

/// How strongly the mask applies to one cell, 0 outside to 1 fully inside.
[[nodiscard]] float maskWeight(const Plan& plan, const SpectralRegion& region, SampleCount frame,
                               int bin) noexcept {
    const double centre = frameCentre(plan.stft, frame) + static_cast<double>(plan.spanStart);
    const auto start = static_cast<double>(region.startSample);
    const auto end = static_cast<double>(region.endSample);
    const auto feather = static_cast<double>(plan.timeFeather);

    if (centre <= start - feather || centre >= end + feather) {
        return 0.0f;
    }
    const float timeWeight = std::min(taper(centre - (start - feather), feather),
                                      taper((end + feather) - centre, feather));

    const double hz = static_cast<double>(bin) * plan.binHz;
    if (hz <= region.lowHz - plan.frequencyFeatherHz ||
        hz >= region.highHz + plan.frequencyFeatherHz) {
        return 0.0f;
    }
    const float frequencyWeight =
        std::min(taper(hz - (region.lowHz - plan.frequencyFeatherHz), plan.frequencyFeatherHz),
                 taper((region.highHz + plan.frequencyFeatherHz) - hz, plan.frequencyFeatherHz));

    return timeWeight * frequencyWeight;
}

/// Run `edit` over one channel's spectrum and write the result back in place.
template <typename Edit>
[[nodiscard]] Status processChannel(AudioBufferView audio, int channel, const Plan& plan,
                                    Edit&& edit) {
    float* samples = audio.channel(channel) + plan.spanStart;

    const auto cells = static_cast<std::size_t>(plan.frames) * static_cast<std::size_t>(plan.bins);
    std::vector<std::complex<float>> spectrum(cells);
    plan.stft.analyse(samples, plan.spanLength, spectrum.data());

    edit(spectrum);

    plan.stft.synthesise(spectrum.data(), plan.frames, samples, plan.spanLength);
    return Status{};
}

} // namespace

Status attenuateRegion(AudioBufferView audio, SampleRate rate, const SpectralRegion& region,
                       double decibels, const SpectralEditSettings& settings) {
    auto planned = makePlan(audio, rate, region, settings);
    if (!planned) {
        return planned.error();
    }
    const Plan& plan = planned.value();

    // A finite gain, even a tiny one, is not the same as zero: interpolating
    // toward it leaves the feathered edges continuous, which is the whole point
    // of having them.
    const auto gain = static_cast<float>(std::pow(10.0, decibels / 20.0));

    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        const auto status =
            processChannel(audio, channel, plan, [&](std::vector<std::complex<float>>& spectrum) {
                for (SampleCount frame = 0; frame < plan.frames; ++frame) {
                    const auto offset =
                        static_cast<std::size_t>(frame) * static_cast<std::size_t>(plan.bins);
                    for (int bin = 0; bin < plan.bins; ++bin) {
                        const float weight = maskWeight(plan, region, frame, bin);
                        if (weight <= 0.0f) {
                            continue;
                        }
                        const float applied = 1.0f + weight * (gain - 1.0f);
                        spectrum[offset + static_cast<std::size_t>(bin)] *= applied;
                    }
                }
            });
        if (!status) {
            return status;
        }
    }
    return Status{};
}

Status healRegion(AudioBufferView audio, SampleRate rate, const SpectralRegion& region,
                  const SpectralEditSettings& settings) {
    auto planned = makePlan(audio, rate, region, settings);
    if (!planned) {
        return planned.error();
    }
    const Plan& plan = planned.value();

    // Frames whose centre lands strictly inside the region are replaced; the
    // frames either side of that run are the donors. Feathering is deliberately
    // not applied to the time axis here -- the replacement is already
    // continuous with its neighbours by construction, and tapering toward the
    // damaged original would reintroduce the very thing being removed.
    SampleCount firstInside = plan.frames;
    SampleCount lastInside = -1;
    for (SampleCount frame = 0; frame < plan.frames; ++frame) {
        const double centre = frameCentre(plan.stft, frame) + static_cast<double>(plan.spanStart);
        if (centre >= static_cast<double>(region.startSample) &&
            centre < static_cast<double>(region.endSample)) {
            firstInside = std::min(firstInside, frame);
            lastInside = std::max(lastInside, frame);
        }
    }
    if (lastInside < firstInside) {
        return Error{ErrorCode::InvalidArgument, "the region is shorter than one analysis frame"};
    }

    const SampleCount before = firstInside - 1;
    const SampleCount after = lastInside + 1;
    const bool haveBefore = before >= 0;
    const bool haveAfter = after < plan.frames;

    const auto twoPi = 2.0 * std::numbers::pi;
    const auto hop = static_cast<double>(plan.stft.hopSize());
    const auto size = static_cast<double>(plan.stft.fftSize());

    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        const auto status =
            processChannel(audio, channel, plan, [&](std::vector<std::complex<float>>& spectrum) {
                const auto cell = [&](SampleCount frame, int bin) -> std::complex<float>& {
                    return spectrum[static_cast<std::size_t>(frame) *
                                        static_cast<std::size_t>(plan.bins) +
                                    static_cast<std::size_t>(bin)];
                };

                for (int bin = 0; bin < plan.bins; ++bin) {
                    // Only bins the user selected are touched, with the same
                    // frequency feather as attenuation so the repair does not
                    // announce its edges.
                    const float weight = maskWeight(plan, region, firstInside, bin);
                    if (weight <= 0.0f) {
                        continue;
                    }

                    const float leftMagnitude = haveBefore ? std::abs(cell(before, bin)) : 0.0f;
                    const float rightMagnitude = haveAfter ? std::abs(cell(after, bin)) : 0.0f;
                    const double startPhase = haveBefore ? std::arg(cell(before, bin)) : 0.0;

                    // Advance phase at the tone's *actual* frequency, not the
                    // bin's centre.
                    //
                    // A partial almost never sits on a bin centre -- 500 Hz at
                    // 48 kHz with a 4096-point FFT lands on bin 42.67 -- and
                    // advancing at the centre instead accumulates error across
                    // the gap until the fill is audibly out of step with what
                    // follows it. The phase vocoder's answer is to measure the
                    // advance the signal actually had just before the damage,
                    // by unwrapping the observed step against the expected one.
                    const double expected = twoPi * static_cast<double>(bin) * hop / size;
                    double advance = expected;
                    if (haveBefore && before >= 1) {
                        const double step =
                            std::arg(cell(before, bin)) - std::arg(cell(before - 1, bin));
                        advance = expected + wrapPhase(step - expected);
                    }

                    const auto span = static_cast<double>(lastInside - firstInside + 1);
                    for (SampleCount frame = firstInside; frame <= lastInside; ++frame) {
                        const double t =
                            span > 1.0 ? static_cast<double>(frame - firstInside) / (span - 1.0)
                                       : 0.5;
                        const auto magnitude =
                            static_cast<float>(leftMagnitude * (1.0 - t) + rightMagnitude * t);
                        const double phase =
                            startPhase + advance * static_cast<double>(frame - before);

                        const std::complex<float> replacement{
                            magnitude * static_cast<float>(std::cos(phase)),
                            magnitude * static_cast<float>(std::sin(phase))};

                        std::complex<float>& target = cell(frame, bin);
                        target = target * (1.0f - weight) + replacement * weight;
                    }
                }
            });
        if (!status) {
            return status;
        }
    }
    return Status{};
}

} // namespace sa::spectral
