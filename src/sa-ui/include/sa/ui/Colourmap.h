#pragma once

#include <sa/ui/ViewGeometry.h>

#include <QColor>
#include <QRgb>
#include <array>
#include <cstdint>

namespace sa::ui {

/// Look up a colour for a normalised value in [0, 1].
[[nodiscard]] QRgb sampleColourmap(Colourmap map, float value) noexcept;

/// Precomputed 256-entry table, so painting is an index rather than a
/// computation per pixel.
[[nodiscard]] const std::array<QRgb, 256>& colourmapTable(Colourmap map) noexcept;

} // namespace sa::ui
