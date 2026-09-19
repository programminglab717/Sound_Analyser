#include <sa/io/AiffWriter.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sa::io {

namespace {

void appendBigU16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::byte>(value & 0xFFu));
}

void appendBigU32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
    }
}

void appendTag(std::vector<std::byte>& out, const char (&tag)[5]) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>(tag[i]));
    }
}

/// Encode a sample rate as the 80-bit IEEE 754 extended float COMM requires.
///
/// The inverse of AiffReader::readExtendedFloat, and written out by hand for
/// the same reason it is read by hand: no mainstream compiler exposes the type.
/// One sign bit (always clear -- a negative sample rate is not a thing), a
/// 15-bit exponent biased by 16383, and a 64-bit mantissa whose leading integer
/// bit is explicit rather than implied.
///
/// frexp gives a fraction in [0.5, 1), so the mantissa is that scaled by 2^64
/// and the stored exponent is one less than frexp's -- hence 16382 rather than
/// 16383. The product stays below 2^64 because the fraction is strictly less
/// than one, so the cast cannot overflow.
void appendExtendedFloat(std::vector<std::byte>& out, double value) {
    if (!(value > 0.0)) {
        out.insert(out.end(), 10, std::byte{0});
        return;
    }
    int exponent = 0;
    const double fraction = std::frexp(value, &exponent);
    const auto mantissa = static_cast<std::uint64_t>(std::ldexp(fraction, 64));
    appendBigU16(out, static_cast<std::uint16_t>(exponent + 16382));
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((mantissa >> shift) & 0xFFu));
    }
}

/// A NAME/AUTH/ANNO text chunk, padded to an even length.
///
/// The pad byte is outside the chunk's declared size, which is what IFF asks
/// for: a reader advances by the size plus the pad, and AiffReader does.
void appendTextChunk(std::vector<std::byte>& out, const char (&tag)[5], const std::string& text) {
    if (text.empty()) {
        return;
    }
    appendTag(out, tag);
    appendBigU32(out, static_cast<std::uint32_t>(text.size()));
    for (const char character : text) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    if ((text.size() & 1u) != 0) {
        out.push_back(std::byte{0});
    }
}

void writeBigEndian(std::ostream& stream, std::uint32_t value) {
    std::array<char, 4> bytes{};
    for (int shift = 24, i = 0; shift >= 0; shift -= 8, ++i) {
        bytes[static_cast<std::size_t>(i)] = static_cast<char>((value >> shift) & 0xFFu);
    }
    stream.write(bytes.data(), 4);
}

/// Encode one float sample as big-endian integer PCM.
///
/// Scaling and clamping follow WavWriter::encodeSample exactly -- 2^(bits-1) as
/// the divisor, clamp before scaling -- so a buffer written as 24-bit AIFF and
/// one written as 24-bit WAV hold the same integers, and the round trip through
/// either reader is the same round trip. The only difference is which end the
/// bytes come out of.
void encodeSample(float value, SampleFormat format, std::byte* out) noexcept {
    const double clamped = std::clamp(static_cast<double>(value), -1.0, 1.0);

    switch (format) {
    case SampleFormat::PcmInt16: {
        const auto scaled = static_cast<std::int32_t>(std::lround(clamped * 32768.0));
        const auto raw = static_cast<std::uint16_t>(
            static_cast<std::int16_t>(std::clamp(scaled, -32768, 32767)));
        out[0] = static_cast<std::byte>((raw >> 8) & 0xFFu);
        out[1] = static_cast<std::byte>(raw & 0xFFu);
        break;
    }
    case SampleFormat::PcmInt24: {
        const auto scaled = static_cast<std::int32_t>(std::lround(clamped * 8388608.0));
        const auto raw = static_cast<std::uint32_t>(std::clamp(scaled, -8388608, 8388607));
        out[0] = static_cast<std::byte>((raw >> 16) & 0xFFu);
        out[1] = static_cast<std::byte>((raw >> 8) & 0xFFu);
        out[2] = static_cast<std::byte>(raw & 0xFFu);
        break;
    }
    case SampleFormat::PcmInt32: {
        const double scaled = std::round(clamped * 2147483648.0);
        const auto raw = static_cast<std::uint32_t>(
            static_cast<std::int32_t>(std::clamp(scaled, -2147483648.0, 2147483647.0)));
        for (int shift = 24, i = 0; shift >= 0; shift -= 8, ++i) {
            out[i] = static_cast<std::byte>((raw >> shift) & 0xFFu);
        }
        break;
    }
    case SampleFormat::PcmUInt8:
    case SampleFormat::Float32:
    case SampleFormat::Float64:
    case SampleFormat::Unknown:
        break;
    }
}

[[nodiscard]] bool isSupported(SampleFormat format) noexcept {
    return format == SampleFormat::PcmInt16 || format == SampleFormat::PcmInt24 ||
           format == SampleFormat::PcmInt32;
}

} // namespace

AiffWriter::AiffWriter(std::ostream& stream, SampleRate sampleRate, ChannelLayout layout,
                       AiffOptions options)
    : stream_(&stream), sampleRate_(sampleRate), layout_(layout), options_(std::move(options)) {
    bytesPerFrame_ = static_cast<std::uint32_t>(bytesPerSample(options_.format)) *
                     static_cast<std::uint32_t>(layout_.count());
}

Result<AiffWriter> AiffWriter::create(std::ostream& stream, SampleRate sampleRate,
                                      ChannelLayout layout, AiffOptions options) {
    if (!sampleRate.isValid()) {
        return Error{ErrorCode::InvalidArgument, "invalid sample rate"};
    }
    if (layout.count() <= 0 || layout.count() > kMaxChannels) {
        return Error{ErrorCode::InvalidArgument, "invalid channel count"};
    }
    if (!isSupported(options.format)) {
        return Error{ErrorCode::InvalidArgument, "AIFF output is 16-, 24- or 32-bit integer PCM; " +
                                                     std::string{toString(options.format)} +
                                                     " is not one of them"};
    }

    AiffWriter writer{stream, sampleRate, layout, std::move(options)};

    const auto channels = static_cast<std::uint16_t>(layout.count());
    const auto bits = static_cast<std::uint16_t>(bytesPerSample(writer.options_.format) * 8);

    std::vector<std::byte> header;
    header.reserve(512);

    appendTag(header, "FORM");
    appendBigU32(header, 0); // patched in finish()
    appendTag(header, "AIFF");

    // COMM before the text chunks and SSND, so that every field finish() has to
    // patch sits at an offset known the moment the header is written.
    appendTag(header, "COMM");
    appendBigU32(header, 18);
    appendBigU16(header, channels);
    const auto frameCountOffset = static_cast<std::streamoff>(header.size());
    appendBigU32(header, 0); // patched in finish()
    appendBigU16(header, bits);
    appendExtendedFloat(header, sampleRate.hz());

    const AudioFileMetadata& metadata = writer.options_.metadata;
    appendTextChunk(header, "NAME", metadata.title);
    appendTextChunk(header, "AUTH", metadata.artist);
    appendTextChunk(header, "ANNO", metadata.comment);

    appendTag(header, "SSND");
    const auto soundSizeOffset = static_cast<std::streamoff>(header.size());
    appendBigU32(header, 0); // patched in finish()
    appendBigU32(header, 0); // offset: the samples start immediately
    appendBigU32(header, 0); // block size: no alignment requested

    stream.write(reinterpret_cast<const char*>(header.data()),
                 static_cast<std::streamsize>(header.size()));
    if (!stream) {
        return Error{ErrorCode::IoFailure, "failed to write AIFF header"};
    }

    writer.formSizePosition_ = 4;
    writer.frameCountPosition_ = frameCountOffset;
    writer.soundSizePosition_ = soundSizeOffset;
    return writer;
}

Status AiffWriter::write(ConstAudioBufferView frames) {
    if (stream_ == nullptr || finished_) {
        return Error{ErrorCode::InvalidArgument, "writer is not open"};
    }
    if (frames.isEmpty()) {
        return Status{};
    }

    const int channels = layout_.count();
    const int sampleBytes = bytesPerSample(options_.format);

    constexpr std::size_t kBlockBytes = 16 * 1024;
    std::array<std::byte, kBlockBytes> block{};
    const auto framesPerBlock =
        static_cast<SampleCount>(kBlockBytes / std::max<std::uint32_t>(1, bytesPerFrame_));
    if (framesPerBlock == 0) {
        return Error{ErrorCode::UnsupportedFormat, "frame larger than the write block"};
    }

    SampleCount done = 0;
    while (done < frames.frames()) {
        const SampleCount batch = std::min(framesPerBlock, frames.frames() - done);

        for (int channel = 0; channel < channels; ++channel) {
            // Channels the caller did not supply are written as silence rather
            // than left as whatever was in the block buffer.
            const bool haveChannel = channel < frames.channelCount();
            const float* in = haveChannel ? frames.channel(channel) + done : nullptr;
            std::byte* base = block.data() + static_cast<std::size_t>(channel) *
                                                 static_cast<std::size_t>(sampleBytes);

            for (SampleCount frame = 0; frame < batch; ++frame) {
                encodeSample(haveChannel ? in[frame] : 0.0f, options_.format,
                             base + static_cast<std::size_t>(frame) * bytesPerFrame_);
            }
        }

        stream_->write(
            reinterpret_cast<const char*>(block.data()),
            static_cast<std::streamsize>(static_cast<std::size_t>(batch) * bytesPerFrame_));
        if (!*stream_) {
            return Error{ErrorCode::IoFailure, "failed to write AIFF audio"};
        }
        done += batch;
    }

    framesWritten_ += frames.frames();
    return Status{};
}

Status AiffWriter::finish() {
    if (stream_ == nullptr || finished_) {
        return Error{ErrorCode::InvalidArgument, "writer is not open"};
    }
    finished_ = true;

    const auto dataBytes = static_cast<std::uint64_t>(framesWritten_) * bytesPerFrame_;
    const auto headerBytes = static_cast<std::uint64_t>(soundSizePosition_) + 12;

    // IFF pads an odd-length chunk to an even boundary. The pad is part of the
    // FORM's length and not part of SSND's, and it has to be on disk before the
    // size fields are patched -- the stream is positioned at the end of the
    // audio right now, which is exactly where it belongs.
    const bool needsPad = (dataBytes & 1u) != 0;
    if (needsPad) {
        const char pad = 0;
        stream_->write(&pad, 1);
    }

    // FORM carries a 32-bit size, so the file cannot exceed 4 GB. Say so rather
    // than write a header whose length field has silently wrapped.
    const std::uint64_t formBytes = headerBytes + dataBytes + (needsPad ? 1u : 0u) - 8;
    if (formBytes > 0xFFFFFFFFull) {
        return Error{ErrorCode::UnsupportedFormat,
                     "output exceeds the 4 GB limit an AIFF FORM header can express"};
    }

    const auto endPosition = stream_->tellp();

    stream_->seekp(formSizePosition_);
    writeBigEndian(*stream_, static_cast<std::uint32_t>(formBytes));

    // AIFF states its length as a frame count in COMM rather than implying it
    // from the chunk size, so there are two numbers to keep in step and both
    // are patched here.
    stream_->seekp(frameCountPosition_);
    writeBigEndian(*stream_, static_cast<std::uint32_t>(framesWritten_));

    stream_->seekp(soundSizePosition_);
    writeBigEndian(*stream_, static_cast<std::uint32_t>(dataBytes + 8));

    stream_->seekp(endPosition);
    stream_->flush();

    if (!*stream_) {
        return Error{ErrorCode::IoFailure, "failed to patch AIFF chunk sizes"};
    }
    return Status{};
}

Status AiffWriter::writeFile(const std::filesystem::path& path, ConstAudioBufferView frames,
                             SampleRate sampleRate, ChannelLayout layout, AiffOptions options) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    if (!stream) {
        return Error{ErrorCode::IoFailure, "cannot create " + path.string()};
    }

    auto writer = AiffWriter::create(stream, sampleRate, layout, std::move(options));
    if (!writer) {
        return writer.error();
    }
    if (auto status = writer.value().write(frames); !status) {
        return status;
    }
    return writer.value().finish();
}

} // namespace sa::io
