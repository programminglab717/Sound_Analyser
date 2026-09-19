#pragma once

#include <sa/spectral/SpectrogramTiles.h>
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

    /// The spectrogram to draw.
    ///
    /// A tiled cache rather than a whole pyramid, because a whole pyramid of a
    /// three-hour recording does not fit in memory. The view does not know or
    /// care which parts of it are resident: render() draws detail where there
    /// is detail and the overview where there is not, so a part-built picture
    /// is coarse in places rather than blank.
    void setTiles(std::shared_ptr<const spectral::SpectrogramTiles> tiles, SampleRate rate,
                  SampleCount totalFrames);
    void setColourmap(Colourmap map);

    [[nodiscard]] Colourmap colourmap() const noexcept { return colourmap_; }

    void setFrequencyScale(FrequencyScale scale);

    [[nodiscard]] FrequencyScale frequencyScale() const noexcept { return scale_; }

    /// Displayed dynamic range floor, in dB. Values at or below read as silence.
    void setFloorDecibels(float decibels);

    [[nodiscard]] float floorDecibels() const noexcept { return floorDb_; }

    /// The selected frequency band. Defaults to the whole spectrum, which is
    /// what a time-only selection means.
    [[nodiscard]] double selectionLowHz() const noexcept { return lowHz_; }

    [[nodiscard]] double selectionHighHz() const noexcept { return highHz_; }

    void setFrequencySelection(double lowHz, double highHz);

signals:
    /// Time, frequency and level under the pointer, for a readout. `hz` is
    /// negative when the pointer is outside the plot.
    void cursorMoved(double seconds, double hz, double decibels);

    void frequencySelectionChanged(double lowHz, double highHz);

protected:
    void paintPlot(QPainter& painter, const QRect& plot) override;
    void paintGutter(QPainter& painter) override;
    void viewInvalidated() override;

    [[nodiscard]] bool selectsVertically() const noexcept override { return true; }

    void verticalSelectionChanged(double lowFraction, double highFraction) override;
    [[nodiscard]] QRect selectionRect(const QRect& plot, int left, int right) const override;
    void hover(const QPoint& position) override;
    void leaveEvent(QEvent* event) override;

private:
    void rebuildImage();

    std::shared_ptr<const spectral::SpectrogramTiles> tiles_;
    Colourmap colourmap_ = Colourmap::Magma;
    FrequencyScale scale_ = FrequencyScale::Logarithmic;
    float floorDb_ = -96.0f;
    double lowHz_ = 0.0;
    double highHz_ = 0.0;

    QImage image_;
    std::vector<std::uint8_t> tile_;
    std::vector<float> rowBinEdges_;
    bool imageDirty_ = true;
};

} // namespace sa::ui
