#pragma once

#include <sa/spectral/SpectrogramPyramid.h>
#include <sa/ui/Colourmap.h>

#include <QImage>
#include <QWidget>

#include <memory>

namespace sa::ui {

/// Spectrogram display, and the surface the product is built around.
///
/// Renders on the CPU. The Phase 0 spike measured that at 11.59 ms for a
/// 1920x1080 view against a 16.67 ms frame budget, so it holds 60 fps at
/// ordinary screen sizes on ordinary hardware. A shader path buys headroom at
/// 4K and above but is not required to run the product -- see
/// docs/03-architecture.md.
class SpectrogramView : public QWidget {
    Q_OBJECT

public:
    explicit SpectrogramView(QWidget* parent = nullptr);

    void setPyramid(std::shared_ptr<const spectral::SpectrogramPyramid> pyramid, SampleRate rate);
    void setViewRange(SampleIndex start, SampleCount length);
    void setColourmap(Colourmap map);
    [[nodiscard]] Colourmap colourmap() const noexcept { return colourmap_; }

    /// Displayed dynamic range floor, in dB. Values at or below read as silence.
    void setFloorDecibels(float decibels);
    [[nodiscard]] float floorDecibels() const noexcept { return floorDb_; }

signals:
    void viewRangeChanged(SampleIndex start, SampleCount length);

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void rebuildImage();

    std::shared_ptr<const spectral::SpectrogramPyramid> pyramid_;
    SampleRate rate_{48000.0};
    SampleIndex viewStart_ = 0;
    SampleCount viewLength_ = 0;
    Colourmap colourmap_ = Colourmap::Magma;
    float floorDb_ = -96.0f;

    QImage image_;
    std::vector<std::uint8_t> tile_;
    bool imageDirty_ = true;
};

} // namespace sa::ui
