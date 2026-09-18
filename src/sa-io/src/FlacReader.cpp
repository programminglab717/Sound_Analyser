#include "DecoderCursor.h"

#include <sa/io/FlacReader.h>

#include <algorithm>
#include <array>
#include <dr_flac.h>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace sa::io {

namespace {

/// dr_flac's callbacks, bridged onto a ByteSource. The user data is always a
/// detail::ByteSourceCursor, never the reader, so that the decoder's view of
/// the stream survives the reader being moved.
std::size_t onRead(void* userData, void* bufferOut, std::size_t bytesToRead) {
    return static_cast<detail::ByteSourceCursor*>(userData)->readInto(bufferOut, bytesToRead);
}

drflac_bool32 onSeek(void* userData, int offset, drflac_seek_origin origin) {
    auto* cursor = static_cast<detail::ByteSourceCursor*>(userData);

    std::int64_t base = 0;
    if (origin == DRFLAC_SEEK_CUR) {
        base = static_cast<std::int64_t>(cursor->position);
    } else if (origin == DRFLAC_SEEK_END) {
        base = cursor->size();
    } else if (origin != DRFLAC_SEEK_SET) {
        return DRFLAC_FALSE;
    }

    return cursor->seekTo(base + static_cast<std::int64_t>(offset)) ? DRFLAC_TRUE : DRFLAC_FALSE;
}

drflac_bool32 onTell(void* userData, drflac_int64* position) {
    const auto* cursor = static_cast<const detail::ByteSourceCursor*>(userData);
    *position = static_cast<drflac_int64>(cursor->position);
    return DRFLAC_TRUE;
}

/// FLAC records a bit depth, not a sample layout, and AudioFileInfo::format
/// exists so an export can round-trip what was there. Report the narrowest PCM
/// width that holds the file's samples without losing any: FLAC permits depths
/// like 12 and 20, and rounding those down would throw away bits on export.
///
/// 8-bit FLAC is reported as 16-bit PCM rather than as PcmUInt8, which is
/// specifically the unsigned encoding WAV uses at that depth; FLAC's is signed,
/// so the 8-bit entry would be a lie where 16-bit is merely wider.
SampleFormat resolveFormat(std::uint8_t bitsPerSample) noexcept {
    if (bitsPerSample == 0) {
        return SampleFormat::Unknown;
    }
    if (bitsPerSample <= 16) {
        return SampleFormat::PcmInt16;
    }
    if (bitsPerSample <= 24) {
        return SampleFormat::PcmInt24;
    }
    if (bitsPerSample <= 32) {
        return SampleFormat::PcmInt32;
    }
    return SampleFormat::Unknown;
}

/// The most PCM frames a FLAC stream of a given size could possibly hold.
///
/// STREAMINFO simply states a frame count, and unlike a WAV data chunk nothing
/// about the file constrains it: a corrupt or hostile one states 2^36, and
/// readAll() would then be asked for a terabyte. The smallest legal FLAC frame
/// is twelve bytes -- an eight-byte header, one constant subframe at the
/// minimum four-bit depth, and a CRC -- and one frame carries at most 65535
/// samples, so no stream can exceed about 5461 frames per byte. This is that
/// bound rounded up to a power of two: anything above it is not a length, it is
/// a corruption, and the stream gets measured instead.
constexpr std::uint64_t kMaxFramesPerByte = 8192;

/// Decode through the stream once, counting frames and keeping none.
///
/// Two things lead here. A FLAC header may declare a total sample count of
/// zero, which means unknown rather than empty -- it is what an encoder writes
/// when it could not seek back to patch STREAMINFO, as happens when FLAC is
/// produced on a pipe -- and taking that at face value would show the user an
/// empty file. Or it may declare a count the file could not possibly hold, in
/// which case the declaration is worthless. Either way, counting is linear in
/// the length of the file but constant in memory, which is the trade this
/// module makes everywhere.
SampleCount countFramesByDecoding(drflac* handle) {
    constexpr drflac_uint64 kSkipBatch = 4096;
    SampleCount total = 0;
    for (;;) {
        const auto got =
            static_cast<SampleCount>(drflac_read_pcm_frames_f32(handle, kSkipBatch, nullptr));
        if (got <= 0) {
            break;
        }
        total += got;
        // A stream long enough to overflow this is not one we could index
        // anyway; stop counting rather than wrap.
        if (total >
            std::numeric_limits<SampleCount>::max() - static_cast<SampleCount>(kSkipBatch)) {
            break;
        }
    }
    return total;
}

} // namespace

/// Owns the dr_flac handle and the cursor its callbacks read through.
struct FlacReader::Decoder {
    detail::ByteSourceCursor cursor;
    drflac* handle = nullptr;
    /// The frame the decoder will hand back next, tracked so that a sequential
    /// read -- every streaming analysis pass -- never pays for a seek.
    SampleIndex nextFrame = 0;

    Decoder() = default;

    ~Decoder() {
        if (handle != nullptr) {
            drflac_close(handle);
        }
    }

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    Decoder(Decoder&&) = delete;
    Decoder& operator=(Decoder&&) = delete;
};

Result<FlacReader> FlacReader::open(const std::filesystem::path& path) {
    auto source = FileByteSource::open(path);
    if (!source) {
        return source.error();
    }
    return decode(std::shared_ptr<const ByteSource>{std::move(source).value()});
}

Result<FlacReader> FlacReader::fromMemory(std::span<const std::byte> bytes) {
    return decode(std::make_shared<const MemoryByteSource>(bytes));
}

Result<FlacReader> FlacReader::decode(std::shared_ptr<const ByteSource> source) {
    if (source == nullptr) {
        return Error{ErrorCode::InvalidArgument, "no byte source"};
    }

    // Check the magic ourselves before handing the stream over. dr_flac reports
    // every failure the same way -- a null pointer -- and the distinction
    // between "this is not a FLAC" and "this FLAC is broken" is exactly what a
    // caller shows the user, so it is worth four bytes to keep it.
    std::array<std::byte, 4> magic{};
    if (source->read(0, magic) != magic.size() || std::to_integer<char>(magic[0]) != 'f' ||
        std::to_integer<char>(magic[1]) != 'L' || std::to_integer<char>(magic[2]) != 'a' ||
        std::to_integer<char>(magic[3]) != 'C') {
        return Error{ErrorCode::UnsupportedFormat, "not a native FLAC stream"};
    }

    auto decoder = std::make_shared<Decoder>();
    decoder->cursor.source = std::move(source);
    decoder->handle = drflac_open(onRead, onSeek, onTell, &decoder->cursor, nullptr);
    if (decoder->handle == nullptr) {
        return Error{ErrorCode::CorruptData, "FLAC stream has no usable STREAMINFO"};
    }

    const drflac& flac = *decoder->handle;
    if (flac.channels == 0 || flac.channels > kMaxChannels) {
        return Error{ErrorCode::UnsupportedFormat,
                     "unsupported channel count (" + std::to_string(flac.channels) + ")"};
    }
    const SampleRate sampleRate{static_cast<double>(flac.sampleRate)};
    if (!sampleRate.isValid()) {
        return Error{ErrorCode::UnsupportedFormat, "unsupported sample rate"};
    }
    const SampleFormat format = resolveFormat(flac.bitsPerSample);
    if (format == SampleFormat::Unknown) {
        return Error{ErrorCode::UnsupportedFormat,
                     "unsupported bit depth (" + std::to_string(flac.bitsPerSample) + ")"};
    }

    const auto channels = static_cast<int>(flac.channels);

    FlacReader reader;
    reader.info_.sampleRate = sampleRate;
    reader.info_.layout = channels == 1   ? ChannelLayout::mono()
                          : channels == 2 ? ChannelLayout::stereo()
                                          : ChannelLayout::discrete(channels);
    reader.info_.format = format;

    // The declared count is attacker-controlled like every other length in a
    // container, and this is the one that turns into an allocation, so it is
    // believed only as far as the file could back it up.
    const std::uint64_t sourceBytes = decoder->cursor.source->size();
    const std::uint64_t plausibleFrames =
        sourceBytes > std::numeric_limits<std::uint64_t>::max() / kMaxFramesPerByte
            ? std::numeric_limits<std::uint64_t>::max()
            : sourceBytes * kMaxFramesPerByte;

    if (flac.totalPCMFrameCount == 0 || flac.totalPCMFrameCount > plausibleFrames) {
        reader.info_.frameCount = countFramesByDecoding(decoder->handle);
        if (drflac_seek_to_pcm_frame(decoder->handle, 0) == DRFLAC_FALSE) {
            return Error{ErrorCode::CorruptData, "FLAC stream cannot be rewound"};
        }
    } else {
        constexpr auto kFrameCeiling =
            static_cast<drflac_uint64>(std::numeric_limits<SampleCount>::max());
        reader.info_.frameCount =
            static_cast<SampleCount>(std::min(flac.totalPCMFrameCount, kFrameCeiling));
    }

    reader.decoder_ = std::move(decoder);
    return reader;
}

Result<SampleCount> FlacReader::read(SampleIndex startFrame, AudioBufferView destination) const {
    if (decoder_ == nullptr || decoder_->handle == nullptr) {
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

    // Seeking is the expensive operation in a compressed format, so a
    // sequential read must not pay for one it does not need.
    if (decoder.nextFrame != startFrame) {
        if (drflac_seek_to_pcm_frame(decoder.handle, static_cast<drflac_uint64>(startFrame)) ==
            DRFLAC_FALSE) {
            return Error{ErrorCode::CorruptData,
                         "cannot seek to frame " + std::to_string(startFrame)};
        }
        decoder.nextFrame = startFrame;
    }

    // Fixed stack block, as in WavReader: streaming reads must not allocate per
    // call. dr_flac hands back interleaved float, so this is also where the
    // de-interleave into the planar destination happens.
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
        const auto got = static_cast<SampleCount>(drflac_read_pcm_frames_f32(
            decoder.handle, static_cast<drflac_uint64>(batch), block.data()));
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

Result<AudioBuffer> FlacReader::readAll() const {
    AudioBuffer buffer;
    try {
        buffer.resize(info_.layout, info_.frameCount);
    } catch (const std::bad_alloc&) {
        // This is the one call here that allocates in proportion to a number
        // taken out of a file, so it is the one that can fail on a long
        // recording or a corrupt header. Failure is reported, never thrown.
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
