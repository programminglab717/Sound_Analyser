#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/dsp/BiquadCascade.h>
#include <sa/dsp/Decibels.h>

#include <vector>

/// A band-pass filter bank on the octave and third-octave centres.
///
/// The other way of asking what is in a spectrum. An FFT answers by binning a
/// transform of a whole block; a filter bank answers by running the audio
/// through one band-pass per band and measuring what comes out of each. The
/// two disagree in ways that matter. A bin has no time in it, so a transform
/// describes a block and nothing finer, while a band-pass has a settling time
/// and an envelope and can be followed sample by sample -- which is what a
/// reverberation time per band has to be measured through, and what a level
/// meter is. A bin is also the same width everywhere, so a band assembled out
/// of bins is three bins wide at the bottom of the range and four hundred at
/// the top, and the bottom bands are as coarse as the transform rather than as
/// coarse as the band.
///
/// **What this does not claim.** IEC 61260 specifies a bank of roughly this
/// shape and then specifies tolerance masks the realised response has to fit
/// inside, together with the effective-bandwidth, anti-aliasing and
/// integration requirements that go with them. Those masks were not available
/// when this was written, so nothing here has been checked against them and
/// nothing here claims conformance -- to that standard or to any other. What
/// is claimed is what the tests measure and nothing beyond it: the centres
/// follow the base-ten definition below, the edges are the stated factors
/// around them, each band is a Butterworth band-pass of the stated order, and
/// its passband and skirts behave as that order implies. The phrase
/// "IEC 61260" does not belong next to this code until the masks are in the
/// repository and the response has been checked against them.
///
/// **Design.** Each band is a Butterworth band-pass of the configured order: a
/// low-pass prototype of half that order, put through the usual
/// low-pass-to-band-pass frequency transformation and then bilinear
/// transformed, one biquad per prototype pole. The band edges are pre-warped
/// before that transform, so the realised -3 dB points land on the band edges
/// themselves rather than near them, and each skirt falls at three decibels
/// per octave per pole -- 18 dB per octave at the default order of six.
///
/// What the pre-warping cannot fix is the shape of a band whose upper edge
/// approaches Nyquist, where tan() stretches the top of the band far more than
/// the bottom and the lower skirt comes out measurably shallower than the
/// order implies. It is worth knowing how much: at 48 kHz the octave band
/// centred at 15848.9 Hz, whose upper edge is 22413.8 Hz against a Nyquist of
/// 24000, rejects a tone an octave below its centre by 13.7 dB where the order
/// implies 19.6. The band below it, centred at 7943.3 Hz, manages 18.3, and
/// every octave band at 1995.3 Hz and under rejects by at least the implied
/// figure less a tenth of a decibel, on both sides.
namespace sa::dsp {

/// Octave or third-octave spacing.
enum class BandSpacing {
    /// Centres a factor of 10^(3/10) apart: ten bands named 31.5 Hz to 16 kHz.
    Octave,
    /// Centres a factor of 10^(1/10) apart: thirty-one bands named 20 Hz to
    /// 20 kHz, which is the display everyone recognises.
    ThirdOctave,
};

/// The reference frequency the whole series hangs off.
inline constexpr double kBandReferenceHz = 1000.0;

/// How far a centre may sit from a frequency that names it.
///
/// The centres people say out loud are rounded: the third-octave bands called
/// 160 Hz and 1.6 kHz are centred at 158.489 and 1584.89, which is 0.944%
/// below their names and the widest gap anywhere in the series. So a range
/// given as "20 Hz to 20 kHz" has to admit the band centred at 19.9526, or it
/// silently drops the band it was naming. Admitting that worst case from below
/// takes 0.954%; 2% is comfortably past that and still only a sixth of the way
/// to the halfway point between two third-octave centres, so this can include
/// or exclude the band a limit names and never its neighbour.
inline constexpr double kBandNameTolerance = 0.02;

/// Exact centre frequency of band `index`, in Hz.
///
/// The base-ten definition: 1000 Hz times 10^(index/10) for third-octaves and
/// 10^(3 index/10) for octaves, so index 0 is 1 kHz under both spacings.
///
/// Derived rather than looked up, and deliberately so. A table of thirty-one
/// four-figure numbers is a table that can be mistyped, and a mistyped centre
/// is a band sitting quietly in the wrong place for the life of the product,
/// wrong by an amount too small to notice and too large to ignore. Deriving it
/// also gives the exact centres instead of the rounded ones a display prints:
/// the band called 1250 Hz is centred at 1258.93, and a filter belongs where
/// the definition puts it rather than where the label rounds it to.
[[nodiscard]] double bandCentreHz(BandSpacing spacing, int index) noexcept;

/// One band of the series, independent of any sample rate.
struct FilterBand {
    /// Position in the series. 0 is the 1 kHz band under either spacing.
    int index = 0;
    /// Exact centre, not the rounded number a display shows.
    double centreHz = 0.0;
    /// Edges, a factor of 2^(-1/6) and 2^(1/6) around the centre for a
    /// third-octave and 2^(-1/2) and 2^(1/2) for an octave. These are the
    /// frequencies the realised filter is 3 dB down at.
    double lowHz = 0.0;
    double highHz = 0.0;
};

/// Why a band in the requested range has no filter.
enum class BandUnavailability {
    /// The upper edge is at or above Nyquist. There is no such band at this
    /// sample rate: the part of it above Nyquist does not exist in the signal
    /// and cannot be recovered, so a filter here would be measuring a narrower
    /// band than the one it is named after. Reported rather than clamped,
    /// because a band silently redefined is worse than a band missing.
    AboveNyquist,
    /// The design came out with a pole on or outside the unit circle, so the
    /// filter would ring rather than decay. Nothing at any rate this library
    /// accepts is currently known to land here -- see the stability test --
    /// but a filter bank that returns a resonator when the arithmetic runs out
    /// is worse than one that says it cannot.
    Unstable,
};

/// A band in the requested range that no filter was built for.
struct UnavailableBand {
    FilterBand band;
    BandUnavailability reason = BandUnavailability::AboveNyquist;
};

struct FilterBankSettings {
    BandSpacing spacing = BandSpacing::ThirdOctave;

    /// Order of each band-pass, counted in poles, so it must be even and each
    /// two poles cost one biquad. Six is three sections and 18 dB per octave
    /// of skirt, which is steep enough that a tone one octave outside an
    /// octave band is 19.6 dB down; the two identical cookbook sections the
    /// reverberation measurement filters with manage 12 dB per octave and
    /// 14.8 dB at the same place. Higher orders are steeper and ring longer,
    /// and the ringing is the reason not to reach for the maximum by default.
    int order = 6;

    /// The range of centres to cover, named the way people name them: the
    /// limits are matched against exact centres within kBandNameTolerance, so
    /// 20 to 20000 gets the thirty-one third-octave bands from the one called
    /// 20 Hz to the one called 20 kHz, and the ten octave bands from 31.5 Hz
    /// to 16 kHz -- the same two sets the FFT-integrated band display reports.
    double lowestCentreHz = 20.0;
    double highestCentreHz = 20000.0;
};

/// The bands `settings` names, low to high, before any sample rate is
/// considered.
///
/// Exposed because a display needs its axis before it has anything to draw on
/// it, and because band edges are worth being able to check on their own.
///
/// Fails on a range that is inverted, not finite, or starts below 1 Hz: no
/// audio band is centred below 1 Hz, and that bound is what keeps a mistyped
/// limit from producing a layout of thousands of bands. A range that is simply
/// narrower than one band comes back empty, which is an answer and not an
/// error.
[[nodiscard]] Result<std::vector<FilterBand>> bandLayout(const FilterBankSettings& settings = {});

/// One band's measured level.
struct BandLevel {
    FilterBand band;
    /// RMS of the band signal, in dBFS.
    ///
    /// An RMS, not a sine-referenced reading: a full-scale sine sitting in the
    /// band measures -3.01 dBFS here, because -3.01 dBFS is its RMS. An FFT
    /// display conventionally shows the sine at 0 instead, which is the same
    /// number plus 3.01 dB. Adding that is left to the caller, who is the one
    /// who knows which of the two conventions the reading is going into.
    double levelDb = kSilenceDecibels;
};

/// One band-pass per band, plus the bookkeeping of which bands exist.
///
/// Built on a worker thread by create(); the filters themselves are
/// BiquadCascades, so running audio through one allocates nothing and is safe
/// from the audio thread. measure() is the offline path and returns a vector.
class FilterBank {
public:
    /// The most poles a band can have, set by the cascade's capacity: one
    /// biquad per two poles.
    static constexpr int kMaxOrder = 2 * BiquadCascade::kMaxSections;

    [[nodiscard]] static Result<FilterBank> create(SampleRate rate,
                                                   const FilterBankSettings& settings = {});

    [[nodiscard]] SampleRate sampleRate() const noexcept { return rate_; }

    [[nodiscard]] const FilterBankSettings& settings() const noexcept { return settings_; }

    /// The bands a filter was built for, low to high.
    [[nodiscard]] const std::vector<FilterBand>& bands() const noexcept { return bands_; }

    [[nodiscard]] int bandCount() const noexcept { return static_cast<int>(bands_.size()); }

    /// The bands in the requested range that no filter was built for, and why.
    /// Empty when every band asked for was realised.
    [[nodiscard]] const std::vector<UnavailableBand>& unavailable() const noexcept {
        return unavailable_;
    }

    /// The cascade realising band `index`, or nullptr if there is no such
    /// band. The non-const overload is the audio-thread entry point: filtering
    /// changes the cascade's state, and it allocates nothing.
    [[nodiscard]] BiquadCascade* filter(int index) noexcept;
    [[nodiscard]] const BiquadCascade* filter(int index) const noexcept;

    /// Clears every band's state, keeping the coefficients.
    void reset() noexcept;

    /// Level per band for one channel of `audio`.
    ///
    /// Resets first, so the answer depends only on the buffer handed in.
    /// That makes the settling of each band part of the measurement, which is
    /// not an accident to be corrected away: a band-pass cannot report a level
    /// it has not yet heard. The lowest third-octave band takes about three
    /// cycles of its centre frequency to reach steady state and some twenty to
    /// settle, so measuring a 20 Hz band from a buffer shorter than about a
    /// second reads low, and reads low by more the shorter the buffer is.
    ///
    /// Reported per band and never summed, because the sum is not the signal.
    /// Each band's noise bandwidth is a few percent wider than the distance
    /// between its edges, which counts some energy twice, and the bank covers
    /// only as far as its top band's upper edge, which counts none of the rest
    /// at all. The two errors have opposite signs and no fixed size: at 48 kHz
    /// with the default third-octave layout they nearly cancel and the sum of
    /// the bands comes back 0.11 dB under the signal, while at 44.1 kHz, where
    /// the 20 kHz band does not fit below Nyquist, it comes back 0.74 dB under.
    [[nodiscard]] Result<std::vector<BandLevel>> measure(ConstAudioBufferView audio,
                                                         int channel = 0);

private:
    SampleRate rate_;
    FilterBankSettings settings_;
    std::vector<FilterBand> bands_;
    std::vector<UnavailableBand> unavailable_;
    std::vector<BiquadCascade> filters_;
};

} // namespace sa::dsp
