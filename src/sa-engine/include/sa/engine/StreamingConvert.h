#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Cancellation.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/dsp/Resampler.h>
#include <sa/io/AudioSource.h>

/// Sample-rate conversion that never holds the file.
///
/// Converting through memory costs the source and the result at once, so it
/// runs out somewhere around two gigabytes of samples -- which a concert, an
/// archive transfer or a multi-hour session is past before it starts. This
/// reads a block, converts it, hands it to a sink and forgets it, so the cost
/// is the block and the filter state whatever the length of the file.
///
/// The output is the same audio, sample for sample, as converting the whole
/// buffer in one go. That is the property the design turns on rather than a
/// pleasant extra: the converter carries filter state across calls, and a
/// hand-off that drops or repeats any of it puts a click at every block
/// boundary. The tests assert exact equality against the in-memory path at
/// several block sizes, because a tolerance is precisely what would hide that.
///
/// What is not claimed: this converts rate, and nothing else. Sample format
/// belongs to the sink -- io::WavWriter already takes one -- and so does
/// dither, which belongs after the last thing that alters the samples rather
/// than before it. Channel count is not changed either; the output carries the
/// source's layout.
namespace sa::engine {

/// Somewhere to put converted audio.
///
/// An interface rather than a callback, for two reasons. It mirrors
/// io::AudioSource at the other end of the conversion, so both ends of a
/// streamed job are described the same way. And a sink is stateful and can
/// fail, which a std::function -- copyable, possibly empty, its state hidden
/// behind type erasure -- says nothing about. io::WavWriter is a four-line
/// adapter away.
///
/// Note what is absent: nothing here finishes a file. Finalisation stays with
/// the caller, and that is what makes a cancelled conversion safe. A caller
/// that never receives a success never calls io::WavWriter::finish(), and a
/// WAV whose header was never patched does not read as a complete file.
class AudioSink {
public:
    virtual ~AudioSink() = default;

    AudioSink() = default;
    AudioSink(const AudioSink&) = delete;
    AudioSink& operator=(const AudioSink&) = delete;
    AudioSink(AudioSink&&) = delete;
    AudioSink& operator=(AudioSink&&) = delete;

    /// Append `frames`, which are never empty and never wider than one block.
    [[nodiscard]] virtual Status write(ConstAudioBufferView frames) = 0;
};

/// Frames converted per round at the default block size: 64 Ki, about 1.4
/// seconds at 48 kHz. Large enough that the per-block overhead disappears,
/// small enough that a stereo block is half a megabyte.
inline constexpr SampleCount kDefaultConversionBlockFrames = 65536;

struct ConversionSpec {
    /// Equal to the source's rate means no resampling at all: the samples are
    /// copied straight through rather than run through a filter whose ratio is
    /// one. Compared exactly, not within a tolerance -- a caller who means
    /// "near enough" is the one who knows how near that is.
    SampleRate outputRate;

    dsp::ResamplerQuality quality = dsp::ResamplerQuality::Best;

    /// Frames read, converted and written per round. The only thing that sets
    /// what the conversion costs in memory, so it is the knob to turn on a
    /// machine that has none. The file's length does not enter; the ratio does,
    /// because the converted block is the read block times the ratio, and an
    /// eightfold upsample therefore holds eight blocks of output.
    SampleCount blockFrames = kDefaultConversionBlockFrames;
};

/// What a finished conversion did.
///
/// `framesWritten` is not `framesRead` times the ratio rounded some particular
/// way. It is everything the converter owed, including the tail that only
/// appears once the input has stopped -- which is the part of a streamed
/// conversion that goes missing, so it is worth being able to check.
struct ConversionProgress {
    SampleCount framesRead = 0;
    SampleCount framesWritten = 0;
};

/// Convert `source` into `sink`, a block at a time.
///
/// Returns the counts rather than a bare Status so a caller can report what it
/// moved without interrogating its own sink.
///
/// Refuses an output rate that is not a usable audio rate, a block size below
/// one frame, and a source with no channels. A rate pair outside the
/// converter's ratio limits is refused by dsp::Resampler, whose message says
/// so. Cancellation returns ErrorCode::Cancelled and leaves whatever reached
/// the sink where it is: the work is not undone, it is only not reported as
/// finished.
[[nodiscard]] Result<ConversionProgress> convertStreaming(const io::AudioSource& source,
                                                          AudioSink& sink,
                                                          const ConversionSpec& spec,
                                                          const JobMonitor& monitor = {});

} // namespace sa::engine
