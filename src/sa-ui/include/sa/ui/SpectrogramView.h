#pragma once

#include <sa/spectral/SpectrogramPyramid.h>
#include <sa/ui/Colourmap.h>
#include <sa/ui/ViewGeometry.h>

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
///
/// The frequency axis is logarithmic by default. A linear axis puts every
/// fundamental anyone cares about in the bottom tenth of the display and gives
/// the top half to the octave from 12 kHz to 24 kHz, which is where almost
/// nothing happens.
class SpectrogramView : public QWidget {
    Q_OBJECT

public:
    explicit SpectrogramView(QWidget* parent = nullptr);

    void setPyramid(std::shared_ptr<const spectral::SpectrogramPyramid> pyramid, SampleRate rate);
    void setViewRange(SampleIndex start, SampleCount length);
    void setColourmap(Colourmap map);

    [[nodiscard]] Colourmap colourmap() const noexcept { return colourmap_; }

    void setFrequencyScale(FrequencyScale scale);

    [[nodiscard]] FrequencyScale frequencyScale() const noexcept { return scale_; }

    /// Displayed dynamic range floor, in dB. Values at or below read as silence.
    void setFloorDecibels(float decibels);

    [[nodiscard]] float floorDecibels() const noexcept { return floorDb_; }

signals:
    void viewRangeChanged(SampleIndex start, SampleCount length);

    /// Time, frequency and level under the pointer, for a readout. `hz` is
    /// negative when the pointer is outside the plot.
    void cursorMoved(double seconds, double hz, double decibels);

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void rebuildImage();
    void paintGutter(QPainter& painter);
    [[nodiscard]] int plotWidth() const noexcept;

    std::shared_ptr<const spectral::SpectrogramPyramid> pyramid_;
    SampleRate rate_{48000.0};
    SampleIndex viewStart_ = 0;
    SampleCount viewLength_ = 0;
    Colourmap colourmap_ = Colourmap::Magma;
    FrequencyScale scale_ = FrequencyScale::Logarithmic;
    float floorDb_ = -96.0f;

    QImage image_;
    std::vector<std::uint8_t> tile_;
    std::vector<float> rowBinEdges_;
    bool imageDirty_ = true;
};

} // namespace sa::ui
