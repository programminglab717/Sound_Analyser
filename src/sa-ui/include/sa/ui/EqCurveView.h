#pragma once

#include <sa/core/Types.h>
#include <sa/dsp/ParametricEq.h>
#include <sa/ui/EqCurve.h>
#include <sa/ui/SpectrumView.h>

#include <vector>

namespace sa::ui {

/// The spectrum panel with an editable EQ curve drawn over it.
///
/// A subclass rather than a transparent widget laid on top, because the whole
/// value of the thing is that the curve and the spectrum are in one coordinate
/// space: a boost has to sit over the peak it is correcting, to the pixel, at
/// every window size and after every splitter drag. Two widgets would have to
/// be kept in step to achieve what one widget gets for nothing, and the first
/// layout change nobody thought about would put the curve an octave out.
///
/// What it does not claim: this is not a mixing EQ and there is no live
/// monitoring of it. Dragging a band changes a drawing and nothing else --
/// the audio changes when the curve is applied to the document, through the
/// same undo machinery as every other processor. An EQ you can hear while you
/// drag it needs the curve hung on the playback graph, which is a different
/// piece of work and would be a poor reason to make this one wait.
///
/// The gestures, in one place because they are the part a person has to learn:
///
///  - drag a handle: sideways for centre frequency, up and down for gain
///  - wheel over a handle: Q, forward to narrow
///  - Shift and drag: Q as well, up to narrow, for pointers with no wheel
///  - double-click on the curve: a new band there; on a handle: back to 0 dB
///  - right-click a handle, or Delete with it selected: the band goes
class EqCurveView : public SpectrumView {
    Q_OBJECT

public:
    explicit EqCurveView(QWidget* parent = nullptr);

    /// Show the curve and take the gestures above. Hidden, the panel behaves
    /// exactly as the spectrum always did, which is what an analyser should do
    /// for someone who has not asked for an EQ.
    void setEqVisible(bool visible);

    [[nodiscard]] bool isEqVisible() const noexcept { return visible_; }

    /// The rate the bands are designed at -- the document's, always. The axis
    /// the curve is drawn on comes from SpectrumView::setSampleRate, and both
    /// have to be told, because a curve designed at one rate and drawn on
    /// another is wrong in a way that looks right.
    void setEqSampleRate(SampleRate rate);

    [[nodiscard]] const EqCurve& curve() const noexcept { return curve_; }

    [[nodiscard]] std::vector<dsp::EqBand> bands() const { return curve_.bands(); }

    void clearBands();

    /// Drive the gestures without a mouse.
    ///
    /// These synthesise the events a pointer would deliver and post them at
    /// the widget, rather than reaching past the handlers to the curve. The
    /// hit-testing and the pixel mapping are precisely what is worth checking
    /// here, and a seam that went round them would pass with both broken.
    bool addBandAt(double frequencyHz, double gainDb);
    bool dragBandTo(int index, double frequencyHz, double gainDb);
    bool shiftDragBandBy(int index, int dy);
    bool turnWheelOverBand(int index, double notches);
    bool clickAwayBand(int index);

signals:
    /// A band was added, moved, reshaped or removed. The window listens so
    /// that "apply" can be greyed out when there is nothing to apply.
    void bandsChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    /// Redraw and tell whoever is listening. Called from every gesture, so
    /// that none of them can change a band and leave the picture stale.
    void bandsEdited();

    EqCurve curve_;
    bool visible_ = false;

    /// The band the numbers are shown for, and the one Delete removes. -1 for
    /// none.
    int selected_ = -1;

    /// The band under a held button, or -1. Separate from the selection
    /// because a drag that leaves the panel must not stop following the
    /// pointer, and because the selection outlives the button.
    int dragging_ = -1;

    /// Where a Shift-drag started, so its Q is anchored on that rather than
    /// accumulated a move at a time.
    bool draggingQ_ = false;
    int dragStartY_ = 0;
    double dragStartQ_ = EqCurve::kDefaultQ;
};

} // namespace sa::ui
