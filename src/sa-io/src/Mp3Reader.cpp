#include "DecoderCursor.h"

#include <sa/io/Mp3Reader.h>

#include <algorithm>
#include <array>
#include <dr_mp3.h>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace sa::io {

namespace {

/// dr_mp3's callbacks, bridged onto a ByteSource. As with dr_flac the user data
/// is the cursor and never the reader, so the decoder's view of the stream
/// survives the reader being moved.
std::size_t onRead(void* userData, void* bufferOut, std::size_t bytesToRead) {
    return static_cast<detail::ByteSourceCursor*>(userData)->readInto(bufferOut, bytesToRead);
}

drmp3_bool32 onSeek(void* userData, int offset, drmp3_seek_origin origin) {
    auto* cursor = static_cast<detail::ByteSourceCursor*>(userData);

    std::int64_t base = 0;
    if (origin == DRMP3_SEEK_CUR) {
        base = static_cast<std::int64_t>(cursor->position);
    } else if (origin == DRMP3_SEEK_END) {
        // dr_mp3 probes for trailing ID3v1 and APE tags with a negative offset
        // from the end, so this arm really is reached.
        base = cursor->size();
    } else if (origin != DRMP3_SEEK_SET) {
        return DRMP3_FALSE;
    }

    return cursor->seekTo(base + static_cast<std::int64_t>(offset)) ? DRMP3_TRUE : DRMP3_FALSE;
}

drmp3_bool32 onTell(void* userData, drmp3_int64* position) {
    const auto* cursor = static_cast<const detail::ByteSourceCursor*>(userData);
    *position = static_cast<drmp3_int64>(cursor->position);
    return DRMP3_TRUE;
}

/// The most PCM frames an MPEG stream of a given size could possibly hold.
///
/// An MP3's length comes from the Xing/Info header when it has one, and that
/// header is part of the file and therefore attacker-controlled. Believing a
/// fabricated one would have readAll() ask for an allocation the size of the
/// claim. The densest legal encoding is layer III on MPEG 2 at 8 kbit/s and
/// 24 kHz, which is 576 samples in 72 bytes, or 24 frames per byte; this is
/// that bound rounded up, and a claim past it gets measured instead of
/// believed.
constexpr std::uint64_t kMaxFramesPerByte = 32;

} // namespace

/// Owns the dr_mp3 decoder and the cursor its callbacks read through.
///
/// The decoder is held by value rather than by pointer because that is the
/// shape dr_mp3 offers, and it is a big object -- it carries a frame of decoded
/// samples and the synthesis filter bank's history -- which is another reason
/// this lives on the heap and not in the reader.
struct Mp3Reader::Decoder {
    detail::ByteSourceCursor cursor;
    drmp3 handle{};
    bool initialised = false;
    /// The frame the decoder will hand back next. A backwards seek in an MP3
    /// costs a rescan from the start of the stream, so tracking this to skip
    /// the seek on a sequential read is not a micro-optimisation.
    SampleIndex nextFrame = 0;

    Decoder() = default;

    ~Decoder() {
        if (initialised) {
            drmp3_uninit(&handle);
        }
    }

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    Decoder(Decoder&&) = delete;
    Decoder& operator=(Decoder&&) = delete;
};

Result<Mp3Reader> Mp3Reader::open(const std::filesystem::path& path) {
    auto source = FileByteSource::open(path);
    if (!source) {
        return source.error();
    }
    return decode(std::shared_ptr<const ByteSource>{std::move(source).value()});
}

Result<Mp3Reader> Mp3Reader::fromMemory(std::span<const std::byte> bytes) {
    return decode(std::make_shared<const MemoryByteSource>(bytes));
}

Result<Mp3Reader> Mp3Reader::decode(std::shared_ptr<const ByteSource> source) {
    if (source == nullptr) {
        return Error{ErrorCode::InvalidArgument, "no byte source"};
    }

    auto decoder = std::make_shared<Decoder>();
    decoder->cursor.source = std::move(source);

    // There is no header at a fixed offset to check first, the way FLAC has its
    // magic: an MP3 is only ever recognised by finding a frame. Initialising
    // the decoder is that search, and it failing is the one signal available.
    if (drmp3_init(&decoder->handle, onRead, onSeek, onTell, nullptr, &decoder->cursor, nullptr) ==
        DRMP3_FALSE) {
        return Error{ErrorCode::CorruptData, "no decodable MPEG audio frame in the stream"};
    }
    decoder->initialised = true;

    const drmp3& mp3 = decoder->handle;
    if (mp3.channels == 0 || mp3.channels > kMaxChannels) {
        return Error{ErrorCode::UnsupportedFormat,
                     "unsupported channel count (" + std::to_string(mp3.channels) + ")"};
    }
    const SampleRate sampleRate{static_cast<double>(mp3.sampleRate)};
    if (!sampleRate.isValid()) {
        return Error{ErrorCode::UnsupportedFormat, "unsupported sample rate"};
    }

    // Free when the file carries a Xing/Info header, a full decode otherwise.
    // Either way the decoder is left back at frame zero.
    drmp3_uint64 totalFrames = drmp3_get_pcm_frame_count(&decoder->handle);

    // A header claiming more audio than the file could hold is not a length,
    // and this is the number that becomes an allocation, so measure instead.
    const std::uint64_t sourceBytes = decoder->cursor.source->size();
    const std::uint64_t plausibleFrames =
        sourceBytes > std::numeric_limits<std::uint64_t>::max() / kMaxFramesPerByte
            ? std::numeric_limits<std::uint64_t>::max()
            : sourceBytes * kMaxFramesPerByte;
    if (totalFrames > plausibleFrames &&
        drmp3_get_mp3_and_pcm_frame_count(&decoder->handle, nullptr, &totalFrames) == DRMP3_FALSE) {
        return Error{ErrorCode::CorruptData, "MPEG stream length cannot be established"};
    }

    if (totalFrames == 0) {
        return Error{ErrorCode::CorruptData, "MPEG stream decodes to no audio"};
    }

    const auto channels = static_cast<int>(mp3.channels);

    Mp3Reader reader;
    reader.info_.sampleRate = sampleRate;
    reader.info_.layout = channels == 1   ? ChannelLayout::mono()
                          : channels == 2 ? ChannelLayout::stereo()
                                          : ChannelLayout::discrete(channels);
    // There is no PCM width in an MP3 to round-trip to. Float32 is what the
    // decoder produces and what an export should preserve.
    reader.info_.format = SampleFormat::Float32;
    constexpr auto kFrameCeiling =
        static_cast<drmp3_uint64>(std::numeric_limits<SampleCount>::max());
    reader.info_.frameCount = static_cast<SampleCount>(std::min(totalFrames, kFrameCeiling));

    reader.decoder_ = std::move(decoder);
    return reader;
}

Result<SampleCount> Mp3Reader::read(SampleIndex startFrame, AudioBufferView destination) const {
    if (decoder_ == nullptr || !decoder_->initialised) {
        return Error{ErrorCode::InvalidArgument, "reader is not open"};
    }
    if (startFrame < 0) {
        return Error{ErrorCode::OutOfRange, "startFrame is negative"};
    }
    if (destination.isEmpty() || startFrame >= info_.frameCount) {
        return SampleCount{0};
    }

    Decoder& decoder = *decoder_;
    const SampleCount wanted = std::min(destination.frames(), info_.frameCount - startFrame);
    const int fileChannels = info_.channelCount();
    const int copyChannels = std::min(fileChannels, destination.channelCount());

    if (decoder.nextFrame != startFrame) {
        if (drmp3_seek_to_pcm_frame(&decoder.handle, static_cast<drmp3_uint64>(startFrame)) ==
            DRMP3_FALSE) {
            return Error{ErrorCode::CorruptData,
                         "cannot seek to frame " + std::to_string(startFrame)};
        }
        decoder.nextFrame = startFrame;
    }

    // Fixed stack block and a de-interleave into the planar destination, as in
    // FlacReader. Streaming reads must not allocate per call.
    constexpr std::size_t kBlockFloats = 4 * 1024; // 16 KiB
    std::array<float, kBlockFloats> block{};
    const auto framesPerBlock =
        static_cast<SampleCount>(kBlockFloats / static_cast<std::size_t>(fileChannels));
    if (framesPerBlock == 0) {
        return Error{ErrorCode::UnsupportedFormat, "frame larger than the read block"};
    }

    SampleCount done = 0;
    while (done < wanted) {
        const SampleCount batch = std::min(framesPerBlock, wanted - done);
        const auto got = static_cast<SampleCount>(drmp3_read_pcm_frames_f32(
            &decoder.handle, static_cast<drmp3_uint64>(batch), block.data()));
        if (got <= 0) {
            break; // truncated or corrupt from here on: report what we managed
        }
        decoder.nextFrame += got;

        for (int channel = 0; channel < copyChannels; ++channel) {
            float* out = destination.channel(channel) + done;
            const float* base = block.data() + channel;
            for (SampleCount frame = 0; frame < got; ++frame) {
                out[frame] =
                    base[static_cast<std::size_t>(frame) * static_cast<std::size_t>(fileChannels)];
            }
        }

        done += got;
        if (got < batch) {
            break;
        }
    }

    return done;
}

Result<AudioBuffer> Mp3Reader::readAll() const {
    AudioBuffer buffer;
    try {
        buffer.resize(info_.layout, info_.frameCount);
    } catch (const std::bad_alloc&) {
        // As in FlacReader: the only allocation here sized by a number out of
        // a file, and failure is reported rather than thrown.
        return Error{ErrorCode::OutOfMemory,
                     "cannot hold " + std::to_string(info_.frameCount) + " frames in memory"};
    }
    if (info_.frameCount == 0) {
        return buffer;
    }
    auto framesRead = read(0, buffer.view());
    if (!framesRead) {
        return framesRead.error();
    }
    return buffer;
}

} // namespace sa::io
