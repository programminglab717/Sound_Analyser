#include <sa/ui/Colourmap.h>

#include <algorithm>
#include <cmath>

namespace sa::ui {

namespace {

/// Control points for each map, sampled from the published perceptually-uniform
/// definitions. Linear interpolation between them is close enough that the
/// lightness stays monotonic, which is the property that matters.
struct Stop {
    float position;
    float r;
    float g;
    float b;
};

constexpr std::array<Stop, 9> kMagma{{{0.000f, 0.001f, 0.000f, 0.014f},
                                      {0.125f, 0.078f, 0.054f, 0.211f},
                                      {0.250f, 0.232f, 0.060f, 0.438f},
                                      {0.375f, 0.390f, 0.100f, 0.502f},
                                      {0.500f, 0.550f, 0.161f, 0.506f},
                                      {0.625f, 0.716f, 0.215f, 0.475f},
                                      {0.750f, 0.868f, 0.288f, 0.409f},
                                      {0.875f, 0.967f, 0.468f, 0.360f},
                                      {1.000f, 0.987f, 0.991f, 0.749f}}};

constexpr std::array<Stop, 9> kViridis{{{0.000f, 0.267f, 0.005f, 0.329f},
                                        {0.125f, 0.283f, 0.141f, 0.458f},
                                        {0.250f, 0.254f, 0.265f, 0.530f},
                                        {0.375f, 0.208f, 0.372f, 0.553f},
                                        {0.500f, 0.164f, 0.471f, 0.558f},
                                        {0.625f, 0.128f, 0.567f, 0.551f},
                                        {0.750f, 0.135f, 0.659f, 0.518f},
                                        {0.875f, 0.267f, 0.749f, 0.441f},
                                        {1.000f, 0.993f, 0.906f, 0.144f}}};

QRgb interpolate(const std::array<Stop, 9>& stops, float value) noexcept {
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    for (std::size_t i = 1; i < stops.size(); ++i) {
        if (clamped <= stops[i].position) {
            const Stop& a = stops[i - 1];
            const Stop& b = stops[i];
            const float span = b.position - a.position;
            const float t = span > 0.0f ? (clamped - a.position) / span : 0.0f;
            const auto channel = [t](float from, float to) {
                return static_cast<int>(std::lround((from + (to - from) * t) * 255.0f));
            };
            return qRgb(channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b));
        }
    }
    const Stop& last = stops.back();
    return qRgb(static_cast<int>(last.r * 255.0f), static_cast<int>(last.g * 255.0f),
                static_cast<int>(last.b * 255.0f));
}

std::array<QRgb, 256> buildTable(Colourmap map) noexcept {
    std::array<QRgb, 256> table{};
    for (int i = 0; i < 256; ++i) {
        const float value = static_cast<float>(i) / 255.0f;
        switch (map) {
        case Colourmap::Magma:
            table[static_cast<std::size_t>(i)] = interpolate(kMagma, value);
            break;
        case Colourmap::Viridis:
            table[static_cast<std::size_t>(i)] = interpolate(kViridis, value);
            break;
        case Colourmap::Grey: {
            const int level = i;
            table[static_cast<std::size_t>(i)] = qRgb(level, level, level);
            break;
        }
        }
    }
    return table;
}

} // namespace

const std::array<QRgb, 256>& colourmapTable(Colourmap map) noexcept {
    static const std::array<QRgb, 256> magma = buildTable(Colourmap::Magma);
    static const std::array<QRgb, 256> viridis = buildTable(Colourmap::Viridis);
    static const std::array<QRgb, 256> grey = buildTable(Colourmap::Grey);

    switch (map) {
    case Colourmap::Viridis:
        return viridis;
    case Colourmap::Grey:
        return grey;
    case Colourmap::Magma:
        break;
    }
    return magma;
}

QRgb sampleColourmap(Colourmap map, float value) noexcept {
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    const auto index = static_cast<std::size_t>(std::lround(clamped * 255.0f));
    return colourmapTable(map)[index];
}

} // namespace sa::ui
