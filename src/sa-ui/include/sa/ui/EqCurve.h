#pragma once

#include <sa/core/Types.h>
#include <sa/dsp/ParametricEq.h>
#include <sa/ui/ViewGeometry.h>

#include <string>
#include <vector>

namespace sa::ui {

/// The EQ curve over the analyser: the bands, where they land on the plot, and
/// what a gesture over them means in hertz and decibels.
///
/// Windowless deliberately. Everything here is arithmetic -- a summed response,
/// a pixel to a frequency, a drag to a gain, a click to the handle it grabbed
/// -- and arithmetic that can only be exercised by opening a window and
/// pushing a mouse around is arithmetic that is checked by eye. The widget
/// above this owns the painting and the events; it owns no maths.
///
/// It is not itself an equaliser. The filters are sa::dsp::ParametricEq and the
/// response drawn is read back from the coefficients those filters are actually
/// running, so the curve cannot drift away from what the audio will get. What
/// this adds is the screen: the band list a person edits, and the mapping
/// between that list and the pixels under their pointer.
class EqCurve {
public:
    /// How near a handle a click has to land to count as grabbing it.
    ///
    /// Generous relative to the drawn handle, because the panel is small and
    /// the bands can sit close together; the nearest handle wins when two are
    /// both in reach, so being generous costs nothing but a miss becoming a
    /// hit.
    static constexpr int kHandleRadius = 10;

    /// What a band may be set to. The frequency ceiling keeps bands away from
    /// Nyquist, where the cookbook peaking filter degenerates into a wire and a
    /// handle would sit on the edge of the plot doing nothing visible.
    static constexpr double kMinimumQ = 0.1;
    static constexpr double kMaximumQ = 24.0;
    static constexpr double kGainLimitDb = 24.0;
    static constexpr double kFrequencyCeilingFraction = 0.95;

    /// Q a new band starts at. One is a bell about 1.4 octaves wide at its
    /// half-gain points -- broad enough to be a tonal move rather than a
    /// surgical one, which is what someone reaching for a new band usually
    /// wants first.
    static constexpr double kDefaultQ = 1.0;

    /// What one notch of the wheel multiplies Q by. Multiplicative rather than
    /// additive because Q is read logarithmically: a step of 0.2 is a huge
    /// change at Q 0.5 and imperceptible at Q 10.
    static constexpr double kQPerWheelNotch = 1.15;

    /// Pixels of a modifier-held drag that double Q, for pointing devices with
    /// no usable wheel.
    static constexpr double kQPixelsPerDoubling = 60.0;

    explicit EqCurve(SampleRate rate = kSampleRate48000);

    [[nodiscard]] SampleRate sampleRate() const noexcept { return eq_.sampleRate(); }

    /// Re-design every band for a new rate.
    ///
    /// The bands are in hertz and survive the change. One that no longer fits
    /// under the new Nyquist is brought down to the ceiling rather than
    /// dropped, so opening a 32 kHz file does not silently lose the top band
    /// of a curve. A rate the filters will not accept at all is refused and
    /// changes nothing.
    bool setSampleRate(SampleRate rate);

    [[nodiscard]] int bandCount() const noexcept { return eq_.bandCount(); }

    [[nodiscard]] const dsp::EqBand* band(int index) const noexcept { return eq_.band(index); }

    /// The bands as the DSP layer wants them, for handing to a processor.
    [[nodiscard]] std::vector<dsp::EqBand> bands() const;

    /// Appends a peaking band and returns its index, or -1 when the parameters
    /// cannot be realised at this rate or the EQ is full. Frequency, gain and Q
    /// are clamped to the ranges above first, so a drag off the edge of the
    /// plot parks a band at the edge rather than failing.
    int addBand(double frequencyHz, double gainDb, double q);

    bool setBand(int index, double frequencyHz, double gainDb, double q);

    bool removeBand(int index);

    void clear() noexcept { eq_.clearBands(); }

    /// True when no band would change the audio. A curve of bands all sitting
    /// at 0 dB is flat, which is why this asks about the gains rather than the
    /// count.
    [[nodiscard]] bool isFlat() const noexcept;

    /// Summed response of every band at `frequency`, in decibels.
    ///
    /// The bands run in series, so their magnitudes multiply and their decibels
    /// add. This is the curve worth drawing: two 6 dB bells that overlap give
    /// 12 dB where they meet, and an EQ that draws only the individual bells
    /// hides exactly the mistake a person most needs to see.
    [[nodiscard]] double responseDbAt(double frequency) const noexcept;

    /// One response value per column of `plot`, left edge first.
    [[nodiscard]] std::vector<double> responseAcross(const SpectrumPlot& plot) const;

    /// Response of one band alone, for drawing the bells under the sum.
    [[nodiscard]] double bandResponseDbAt(int index, double frequency) const noexcept;

    [[nodiscard]] int handleX(const SpectrumPlot& plot, int index) const noexcept;
    [[nodiscard]] int handleY(const SpectrumPlot& plot, int index) const noexcept;

    /// The band whose handle is within kHandleRadius of (x, y), or -1.
    ///
    /// Nearest wins, and later bands win a tie, so a band dropped on top of
    /// another is the one that comes back up -- the alternative strands the
    /// band just placed under one the user cannot see past.
    [[nodiscard]] int handleAt(const SpectrumPlot& plot, int x, int y) const noexcept;

    /// Put a band's handle at (x, y): x is its centre frequency, y its gain.
    /// Q is untouched, which is what makes a drag a drag rather than a reset.
    bool moveHandleTo(const SpectrumPlot& plot, int index, int x, int y);

    /// Q after turning the wheel `notches` forward over a band. Forward
    /// narrows, which is the direction every EQ with a wheel uses.
    bool adjustQByNotches(int index, double notches);

    /// Q after a modifier-held drag that started at `startQ` and has travelled
    /// `dy` pixels, positive downwards. Up narrows, to agree with the wheel.
    ///
    /// Anchored on where the drag began rather than applied step by step, so
    /// that dragging past a limit and back comes home to the Q it started
    /// from instead of stopping wherever the clamp left it.
    bool setQFromDrag(int index, double startQ, int dy);

    /// "1.00 kHz   +6.0 dB   Q 1.41" -- what a band is, as a line of text.
    ///
    /// A mastering decision is a number before it is a shape: "about 4 dB
    /// somewhere near 3 k" is not a setting anyone can repeat tomorrow, or
    /// describe to the person who asked for it.
    [[nodiscard]] std::string describe(int index) const;

    /// What the EQ is doing, in one phrase, for an undo entry and a status
    /// line. One band names itself -- "+6.0 dB at 1.00 kHz" -- and several are
    /// counted, because an undo entry reading out four bands is a label nobody
    /// can scan.
    [[nodiscard]] std::string summarise() const;

private:
    [[nodiscard]] double frequencyCeiling() const noexcept;

    dsp::ParametricEq eq_;
};

} // namespace sa::ui
