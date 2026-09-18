#include <sa/engine/Clip.h>

#include <algorithm>
#include <cmath>

namespace sa::engine {

float fadeGain(FadeShape shape, SampleIndex position, SampleCount length, bool fadingIn) noexcept {
    if (length <= 0) {
        return 1.0f;
    }
    if (position <= 0) {
        return fadingIn ? 0.0f : 1.0f;
    }
    if (position >= length) {
        return fadingIn ? 1.0f : 0.0f;
    }

    // Normalised progress through the fade, always measured as "how far in",
    // then mirrored for a fade-out. Deriving both directions from one curve
    // keeps a fade-out the exact mirror of a fade-in, so a crossfade built from
    // the pair sums correctly.
    const double t = static_cast<double>(position) / static_cast<double>(length);
    const double progress = fadingIn ? t : 1.0 - t;

    switch (shape) {
    case FadeShape::Linear:
        return static_cast<float>(progress);
    case FadeShape::EqualPower:
        // sin/cos pair: two of these sum to constant power, so a crossfade does
        // not dip in the middle the way two linear fades do.
        return static_cast<float>(std::sin(progress * 1.5707963267948966));
    case FadeShape::Logarithmic:
        return static_cast<float>(std::pow(progress, 0.5));
    case FadeShape::Exponential:
        return static_cast<float>(progress * progress);
    case FadeShape::SCurve:
        return static_cast<float>(progress * progress * (3.0 - 2.0 * progress));
    }
    return static_cast<float>(progress);
}

float Clip::gainAt(SampleIndex offset) const noexcept {
    if (offset < 0 || offset >= length) {
        return 0.0f;
    }

    float value = gain;
    if (fadeIn.length > 0 && offset < fadeIn.length) {
        value *= fadeGain(fadeIn.shape, offset, fadeIn.length, true);
    }
    if (fadeOut.length > 0) {
        const SampleIndex fromEnd = length - offset;
        if (fromEnd <= fadeOut.length) {
            // Distance from the start of the fade-out, so position 0 is full
            // gain and position `length` is silence.
            value *= fadeGain(fadeOut.shape, fadeOut.length - fromEnd, fadeOut.length, false);
        }
    }
    return value;
}

} // namespace sa::engine
