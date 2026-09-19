#include <sa/dsp/Dither.h>

#include <algorithm>
#include <cmath>

namespace sa::dsp {

Result<Ditherer> Ditherer::create(const DitherSettings& settings, int channelCount) {
    if (channelCount <= 0) {
        return Error{ErrorCode::InvalidArgument, "dither needs at least one channel"};
    }
    if (settings.bits < 2 || settings.bits > 32) {
        return Error{ErrorCode::InvalidArgument, "dither depth must be between 2 and 32 bits"};
    }
    return Ditherer{settings, channelCount};
}

Ditherer::Ditherer(const DitherSettings& settings, int channelCount)
    : settings_(settings), channelCount_(channelCount) {
    // One code, as a fraction of full scale. The writer scales by 2^(bits-1),
    // so this has to match that exactly or the dither is the wrong size.
    step_ = 1.0 / std::pow(2.0, settings_.bits - 1);
    reset();
}

void Ditherer::reset() noexcept {
    // Seeded rather than random: an export written twice has to give the same
    // file. A zero seed would leave the generator stuck, so it is nudged.
    state_ = settings_.seed == 0 ? 0x9E3779B97F4A7C15ULL : settings_.seed;
    error1_.assign(static_cast<std::size_t>(channelCount_), 0.0);
    error2_.assign(static_cast<std::size_t>(channelCount_), 0.0);
}

double Ditherer::uniform() noexcept {
    // xorshift64*: a few instructions, no allocation, and a period long enough
    // that a programme cannot reach the end of it. The quality bar here is that
    // the values are uncorrelated with the audio, which any decent generator
    // clears; this is not cryptography.
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    const std::uint64_t scrambled = state_ * 0x2545F4914F6CDD1DULL;
    // Top 53 bits into [0, 1), then centred.
    return static_cast<double>(scrambled >> 11) * (1.0 / 9007199254740992.0) - 0.5;
}

void Ditherer::process(AudioBufferView audio) noexcept {
    if (settings_.type == DitherType::None || audio.channelCount() != channelCount_ ||
        audio.frames() <= 0) {
        return;
    }
    const bool shaping = settings_.type == DitherType::TpdfNoiseShaped;

    for (SampleCount i = 0; i < audio.frames(); ++i) {
        // Frame by frame rather than channel by channel, so every channel draws
        // from the generator in a fixed order and the result does not depend on
        // the block size.
        for (int channel = 0; channel < channelCount_; ++channel) {
            const auto c = static_cast<std::size_t>(channel);
            float* sample = audio.channel(channel) + i;

            double value = *sample;
            if (shaping) {
                // Error feedback through (1 - z^-1)^2. Subtracting the past
                // errors shaped this way makes the noise that remains rise
                // with frequency.
                value -= 2.0 * error1_[c] - error2_[c];
            }

            // Two uniforms summed: triangular, two codes peak to peak. One
            // uniform would leave the noise floor modulated by the signal,
            // which is the fault dither exists to remove rather than a smaller
            // version of it.
            const double dithered = value + (uniform() + uniform()) * step_;

            if (shaping) {
                // What the writer's rounding will do to this value, so the
                // feedback describes the error that is actually made.
                const double quantised = std::round(dithered / step_) * step_;
                error2_[c] = error1_[c];
                error1_[c] = quantised - dithered;
            }

            // Clamped here as well as in the writer: dither can push a sample
            // that was exactly at full scale past it, and a wrap at that point
            // is a click.
            *sample = static_cast<float>(std::clamp(dithered, -1.0, 1.0));
        }
    }
}

} // namespace sa::dsp
