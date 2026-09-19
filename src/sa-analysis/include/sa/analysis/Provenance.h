#pragma once

#include <sa/analysis/Decibels.h>
#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>

#include <vector>

/// What a file has been through, read off the audio rather than off its header.
///
/// A header says what a file is now. It says nothing about what it was, and the
/// difference is the whole question in archival work, in delivery checking and
/// in anything where provenance matters: a 24-bit WAV that was an MP3 an hour
/// ago is still a 24-bit WAV, and the only place the MP3 is still visible is in
/// the samples.
///
/// Two things are visible there, and both are measured exactly rather than
/// guessed at.
namespace sa::analysis {

/// Everything the audio says about where it came from.
struct Provenance {
    /// Frequency above which there is essentially nothing, in Hz, or zero when
    /// the spectrum simply runs to the top.
    ///
    /// A lossy encoder discards everything above a cutoff chosen by its
    /// bitrate, and the edge it leaves is unnaturally sharp -- nothing acoustic
    /// stops like that. The frequency also names the encoder's settings fairly
    /// closely: around 16 kHz for 128 kbit MP3, 19 to 20 for 320, 15 for older
    /// or lower settings.
    double cutoffHz = 0.0;

    /// How far the spectrum falls across the cutoff, in dB. Large and sudden is
    /// an encoder; gradual is a microphone, a room, or a tape.
    double cutoffDropDb = 0.0;

    /// True when the drop is both deep and sharp enough that nothing acoustic
    /// accounts for it.
    ///
    /// Deliberately not called "is an MP3". A brick wall at 16 kHz is also what
    /// a deliberate low-pass looks like, and what a 32 kHz source upsampled to
    /// 48 looks like. What can be said from the samples is that the top of the
    /// band was removed by something with a very steep filter, and that is what
    /// this says.
    bool hasSteepCutoff = false;

    /// Bits that actually carry information, out of the file's declared depth.
    ///
    /// A 24-bit file made by padding a 16-bit master has eight bits that are
    /// always zero, and this reports 16. Material that has been through any
    /// processing at all fills every bit, so a result equal to the declared
    /// depth means only that nothing is provably unused.
    int effectiveBits = 0;

    /// What the file says it is, for comparison with the above.
    int declaredBits = 0;

    /// True when the file declares more bits than it uses -- a padded file
    /// rather than a deeper one.
    bool isPadded = false;

    /// False when there was not enough audio to say anything.
    bool valid = false;

    SampleCount frames = 0;
};

/// How much audio to look at, and how hard to look.
struct ProvenanceSettings {
    /// Declared depth of the file, from its header. Zero means a float file,
    /// where the bit-depth question does not apply and is not answered.
    int declaredBits = 0;

    /// How deep a fall counts as a cliff rather than a slope. Encoders leave
    /// 40 dB and more across a fraction of an octave.
    double cutoffDropDb = 35.0;

    /// How narrow the fall has to be, as a fraction of an octave. A natural
    /// high-frequency rolloff takes octaves; an encoder takes a few hundred
    /// hertz.
    double cutoffWidthOctaves = 0.25;

    /// Ignore everything above this, because it is above what any encoder
    /// keeps and a file's own antialiasing lives there.
    double highestInterestingHz = 22000.0;
};

/// Read `audio` and report what it says about its own history.
///
/// The audio should be the whole file, or a fair sample of it. A single quiet
/// passage can look band-limited when the programme is not.
[[nodiscard]] Result<Provenance> examineProvenance(ConstAudioBufferView audio, SampleRate rate,
                                                   const ProvenanceSettings& settings = {});

/// Bits that actually vary, out of `declaredBits`.
///
/// Exposed separately because it is exact, cheap and answers a question of its
/// own: whether a file is as deep as it claims. Returns `declaredBits` when
/// every bit is in use, and 0 for silence, which uses none of them.
[[nodiscard]] int effectiveBitDepth(ConstAudioBufferView audio, int declaredBits) noexcept;

} // namespace sa::analysis
