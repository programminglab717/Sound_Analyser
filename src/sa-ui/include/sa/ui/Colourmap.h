#pragma once

#include <QColor>
#include <QRgb>
#include <array>
#include <cstdint>

namespace sa::ui {

/// Perceptually-ordered colour maps for spectrogram display.
///
/// The classic blue-green-red "rainbow" is excluded deliberately. It is not
/// perceptually uniform -- equal steps in value produce unequal steps in
/// apparent brightness -- so it invents edges where the data has none and hides
/// edges where it does. It is also unreadable to the ~8% of men with red-green
/// colour blindness, who are not a rounding error in an audio tool's audience.
///
/// Magma and viridis both increase monotonically in lightness, so they survive
/// being printed in greyscale and stay legible to every kind of colour vision.
enum class Colourmap {
    Magma,   ///< Black to purple to orange to white. The default.
    Viridis, ///< Dark blue to green to yellow. Better for fine detail.
    Grey,    ///< For print, and for anyone who wants no hue at all.
};

/// Look up a colour for a normalised value in [0, 1].
[[nodiscard]] QRgb sampleColourmap(Colourmap map, float value) noexcept;

/// Precomputed 256-entry table, so painting is an index rather than a
/// computation per pixel.
[[nodiscard]] const std::array<QRgb, 256>& colourmapTable(Colourmap map) noexcept;

} // namespace sa::ui
