#pragma once

#include <sa/spectral/SpectrogramPyramid.h>
#include <sa/ui/Colourmap.h>
#include <sa/ui/TimeAxisView.h>

#include <QImage>
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
class SpectrogramView : public TimeAxisView {
    Q_OBJECT

public:
    explicit SpectrogramView(QWidget* parent = nullptr);

    void setPyramid(std::shared_ptr<const spectral::SpectrogramPyramid> pyramid, SampleRate rate,
                    SampleCount totalFrames);
    void setColourmap(Colourmap map);

    [[nodiscard]] Colourmap colourmap() const noexcept { return colourmap_; }

    void setFrequencyScale(FrequencyScale scale);

    [[nodiscard]] FrequencyScale frequencyScale() const noexcept { return scale_; }

    /// Displayed dynamic range floor, in dB. Values at or below read as silence.
    void setFloorDecibels(float decibels);

    [[nodiscard]] float floorDecibels() const noexcept { return floorDb_; }

signals:
    /// Time, frequency and level under the pointer, for a readout. `hz` is
    /// negative when the pointer is outside the plot.
    void cursorMoved(double seconds, double hz, double decibels);

protected:
    void paintPlot(QPainter& painter, const QRect& plot) override;
    void paintGutter(QPainter& painter) override;
    void viewInvalidated() override;
    void hover(const QPoint& position) override;
    void leaveEvent(QEvent* event) override;

private:
    void rebuildImage();

    std::shared_ptr<const spectral::SpectrogramPyramid> pyramid_;
    Colourmap colourmap_ = Colourmap::Magma;
    FrequencyScale scale_ = FrequencyScale::Logarithmic;
    float floorDb_ = -96.0f;

    QImage image_;
    std::vector<std::uint8_t> tile_;
    std::vector<float> rowBinEdges_;
    bool imageDirty_ = true;
};

} // namespace sa::ui
