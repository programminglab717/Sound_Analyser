#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/dsp/BiquadCascade.h>
#include <sa/dsp/Dynamics.h>
#include <sa/dsp/OfflineDynamics.h>

#include <vector>

/// Compressing a few bands of the spectrum separately, then adding them back up.
///
/// One compressor across a whole programme is driven by whatever is loudest in
/// it, which on most music is the bass. So the kick drum pulls the cymbals down
/// with it, the mix breathes in time with the low end, and the only way to stop
/// it is to stop compressing. Splitting first breaks that coupling: the low
/// band's detector never hears the cymbals and the high band's never hears the
/// kick, so a boomy bottom can be held still with the top left where it was.
///
/// ## Why a crossover and not the filter bank
///
/// FilterBank.h is a bank of Butterworth band-passes on the octave and
/// third-octave centres, and it is the wrong splitter here for three separate
/// reasons, any one of which would be enough.
///
/// It does not sum. Two adjacent Butterworth band-passes cross at their -3 dB
/// edges, so where they meet their sum is 3 dB up; further out the bands gap
/// instead and the sum is down. The bank's own header says this out loud and
/// reports the error at 0.11 to 0.74 dB depending on the rate. That is a fine
/// number for a measurement read one band at a time and a disqualifying one for
/// a processor whose output *is* the sum -- a permanent colouring of everything
/// the tool touches, whether or not a single decibel of compression happened.
///
/// Its centres are fixed. They come from the base-ten octave series, and a
/// multiband compressor needs its crossovers where the material puts them: 120
/// Hz on one mix and 250 on another, chosen against the bass and the kick.
/// "The nearest octave centre" is not an answer to that question.
///
/// There are too many of them. A third-octave bank is thirty-one bands, and
/// thirty-one compressors is not a multiband compressor -- it is a spectral
/// processor with thirty-one controls, none of which means anything on its own.
///
/// A Linkwitz-Riley crossover is the complementary splitter that answers all
/// three: the frequency is whatever the caller asks for, the bands are as few
/// as the caller wants, and the two halves sum to unit magnitude by
/// construction. It is what Deess.cpp already splits with, and for the same
/// reason.
///
/// ## How it is built
///
/// A crossover of Linkwitz-Riley order 2N is a Butterworth of order N run twice
/// in series, so each half is 6 dB down where they cross rather than 3, and the
/// two are in phase there. Analytically, with s normalised to the crossover and
/// D(s) the Butterworth denominator of order N, the halves are 1/D(s)^2 and
/// s^(2N)/D(s)^2, and because D(s) D(-s) = 1 + s^(2N) their sum is D(-s)/D(s):
/// an all-pass of order N. Unit magnitude everywhere, exactly, and no polarity
/// flip on either half so long as N is even.
///
/// N even is also what BiquadCascade::butterworth will design -- an odd
/// Butterworth order needs a first-order section the cookbook set does not
/// produce. Both constraints say the same thing, so `order` is required to be a
/// multiple of four and nothing is silently rounded to reach one.
///
/// ## Three bands and up
///
/// Splitting twice does not work by splitting twice. Take the high half of the
/// first crossover and split that again, and the three bands sum to
/// LP1 + AP2 HP1, which is not all-pass: the second crossover's phase shift is
/// in two of the three bands and not in the third, and the mismatch is a
/// several-decibel scoop around the *first* crossover. It is the usual way to
/// build a multiband compressor that quietly spoils everything switched into
/// it.
///
/// The fix is to put the missing phase back: every band is also run through the
/// all-pass equivalent of each crossover *above* it, and then the sum
/// telescopes,
///
///     b0 + b1 + b2 = AP2 LP1 + AP2 HP1 = AP2 (LP1 + HP1) = AP2 AP1,
///
/// which is all-pass again. The all-pass equivalent is not designed separately;
/// it is that crossover's own two halves, run on the band and added, so the
/// identity holds by construction rather than by two designers agreeing.
///
/// ## What this does not claim
///
/// The output is not the input. With every band bypassed the output is the
/// input through one all-pass per crossover: flat in magnitude, measured flat
/// to within 0.0002 dB from 20 Hz to 20 kHz at the default settings, but not
/// flat in phase and not equal to the input sample for sample. A transient
/// comes out smeared by the group delay of those all-passes, which at a 200 Hz
/// crossover is a few milliseconds. That is the price of splitting at all, and
/// anything that needs its samples back unchanged should not be routed through
/// this at all.
///
/// Nothing here is real-time. It holds three copies of the selection and runs
/// the whole buffer band by band.
namespace sa::dsp {

/// Steepest crossover the cascade can realise: an order-2N Linkwitz-Riley half
/// is N biquads, and a cascade holds sixteen.
inline constexpr int kMaxCrossoverOrder = 2 * BiquadCascade::kMaxSections;

/// One band's compressor, plus the two switches that exist for setting it up.
///
/// A struct rather than the bare CompressorSettings the rest of the library
/// passes around, because bypass and solo are per band and belong next to the
/// band they apply to. Parallel vectors of flags would let a caller hand in
/// three settings and four bypasses.
struct MultibandBandSettings {
    CompressorSettings compressor;

    /// Take this band's compressor out of the circuit. The band still reaches
    /// the output; it is simply not processed, and its makeup gain is not
    /// applied either -- bypass means untouched, not "compressed by nothing".
    bool bypass = false;

    /// Listen to this band alone. When any band is soloed, only soloed bands
    /// reach the output and the rest are dropped; with none soloed every band
    /// is heard, which is the ordinary case.
    ///
    /// A dropped band is not compressed either. There is nothing to compress it
    /// for, so its reported gain reduction is zero rather than a figure
    /// describing audio nobody can hear.
    bool solo = false;
};

struct MultibandSettings {
    /// Crossover frequencies in hertz, low to high: n of them make n + 1 bands.
    ///
    /// Required to be strictly increasing and strictly inside (0, Nyquist). A
    /// list that is out of order or has a repeat is refused rather than sorted
    /// or deduplicated: either is far more likely to be a digit typed in the
    /// wrong place than a request, and quietly repairing one hands back bands
    /// the caller did not ask for under settings it believes it knows.
    std::vector<double> crossoverHz{200.0, 2000.0, 8000.0};

    /// One entry per band, so exactly crossoverHz.size() + 1 of them. The
    /// default is four bands of the default compressor, matching the three
    /// default crossovers.
    std::vector<MultibandBandSettings> bands = std::vector<MultibandBandSettings>(4);

    /// Linkwitz-Riley order, in poles: 4 is 24 dB per octave, 8 is 48. Must be
    /// a multiple of four, at least 4 and at most kMaxCrossoverOrder -- see
    /// above for why an order that is not is rejected rather than rounded.
    ///
    /// 4 by default because it is the mastering default: steep enough that the
    /// bands are separate, shallow enough that the crossover does not ring.
    int order = 4;

    /// Frames at the start of the buffer that exist only to settle the
    /// envelopes, exactly as in OfflineDynamics.h -- they are processed and are
    /// then the caller's to discard. dynamicsRunUp() sizes one.
    ///
    /// The crossover filters settle in this region too, which they would
    /// otherwise do over the first few cycles of the lowest crossover. It
    /// changes nothing about the recombination, which is exact from the first
    /// sample either way, but it does keep a band's compressor from looking at
    /// a filter's start-up transient.
    SampleCount runUp = 0;

    /// Frames at each end of the processed region to crossfade back to the
    /// unprocessed signal, so a compressed selection does not step where it
    /// meets the audio beside it.
    ///
    /// Applied per band with the same curve in every band, which is the same
    /// thing as applying it to the sum: the crossfade is linear, so the blend
    /// of the sums and the sum of the blends are one signal.
    ///
    /// It crossfades towards the band before its compressor, so the sum
    /// crossfades towards the sum of the uncompressed bands -- the input
    /// through the all-pass chain, not the input. It removes the gain step at
    /// the join and it cannot remove the phase step, which is inherent to
    /// splitting at all. A selection spliced back into untouched audio still
    /// meets that, which is the reason a multiband pass belongs on a whole
    /// programme rather than on a passage inside one.
    SampleCount blend = 0;

    /// Stereo linking for every band's compressor. Linked by default, for the
    /// usual reason -- an unlinked pair moves the image whenever the two
    /// channels differ, and a multiband one moves it differently per band.
    OfflineDynamicsSettings dynamics;
};

struct MultibandResult {
    /// Most each band was pulled down at any moment, in decibels and positive,
    /// low band first. One entry per band.
    ///
    /// Makeup gain is excluded, so this is what a gain-reduction meter shows
    /// and it means the same thing as Compressor::gainReductionDb(). A bypassed
    /// band, or one dropped by another band's solo, reports zero.
    ///
    /// Measured from the band signal on both sides of the compressor rather
    /// than read off the envelope, so it describes the gain that was really
    /// applied. The run-up is left out because the caller throws it away, and
    /// the two crossfades are left out because the gain there is partly the
    /// crossfade: with any makeup gain at all, a reading taken inside one says
    /// the band was reduced by the makeup at the very sample where nothing was
    /// done to it.
    std::vector<double> gainReductionDb;
};

/// Split `audio`, compress each band, and sum the bands back over it in place.
///
/// Fails, without touching the buffer, on a rate that is not an audio rate, a
/// crossover list that is empty, not strictly increasing, or not strictly
/// inside (0, Nyquist), a band count that is not one more than the crossover
/// count, an order that is not a multiple of four in range, a negative run-up
/// or blend, a run-up past the end of the buffer, or any band's compressor
/// settings being ones Compressor::create would reject. Every band is
/// validated, bypassed or not.
///
/// A blend longer than the region it has to fit in is clamped rather than
/// refused, which is what compressOffline does with the same number.
[[nodiscard]] Result<MultibandResult> compressMultiband(AudioBufferView audio, SampleRate rate,
                                                        const MultibandSettings& settings = {});

} // namespace sa::dsp
