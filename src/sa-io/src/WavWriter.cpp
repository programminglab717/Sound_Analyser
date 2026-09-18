#include <sa/io/WavWriter.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <vector>

namespace sa::io {

namespace {

void appendU16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xFFu));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xFFu));
}

void appendU32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
    }
}

void appendU64(std::vector<std::byte>& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
    }
}

void appendTag(std::vector<std::byte>& out, const char (&tag)[5]) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>(tag[i]));
    }
}

/// Fixed-width, NUL-padded, as the bext chunk requires.
void appendFixedString(std::vector<std::byte>& out, const std::string& text, std::size_t width) {
    const std::size_t count = std::min(text.size(), width);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(static_cast<std::byte>(text[i]));
    }
    out.insert(out.end(), width - count, std::byte{0});
}

void writeLittleEndian(std::ostream& stream, std::uint32_t value) {
    std::array<char, 4> bytes{};
    for (int shift = 0, i = 0; shift < 32; shift += 8, ++i) {
        bytes[static_cast<std::size_t>(i)] = static_cast<char>((value >> shift) & 0xFFu);
    }
    stream.write(bytes.data(), 4);
}

/// Encode one float sample into the target format.
///
/// Scaling uses the same 2^(bits-1) divisor the reader decodes with, so encode
/// and decode are exact inverses up to half a least-significant bit. Scaling by
/// 2^(bits-1) - 1 on the way out (a common shortcut) makes every sample read
/// back slightly quiet, and the error grows with amplitude.
///
/// Clamping happens before scaling: without it a sample past full scale wraps
/// to the opposite polarity, which sounds like a click rather than like the
/// clipping the user expects.
void encodeSample(float value, SampleFormat format, std::byte* out) noexcept {
    const double clamped = std::clamp(static_cast<double>(value), -1.0, 1.0);

    switch (format) {
    case SampleFormat::PcmUInt8: {
        // 8-bit WAV is unsigned with 128 as silence, unlike every other depth.
        const auto scaled = static_cast<int>(std::lround(clamped * 128.0)) + 128;
        out[0] = static_cast<std::byte>(std::clamp(scaled, 0, 255));
        break;
    }
    case SampleFormat::PcmInt16: {
        const auto scaled = static_cast<std::int32_t>(std::lround(clamped * 32768.0));
        const auto raw = static_cast<std::uint16_t>(
            static_cast<std::int16_t>(std::clamp(scaled, -32768, 32767)));
        out[0] = static_cast<std::byte>(raw & 0xFFu);
        out[1] = static_cast<std::byte>((raw >> 8) & 0xFFu);
        break;
    }
    case SampleFormat::PcmInt24: {
        const auto scaled = static_cast<std::int32_t>(std::lround(clamped * 8388608.0));
        const auto raw = static_cast<std::uint32_t>(std::clamp(scaled, -8388608, 8388607));
        out[0] = static_cast<std::byte>(raw & 0xFFu);
        out[1] = static_cast<std::byte>((raw >> 8) & 0xFFu);
        out[2] = static_cast<std::byte>((raw >> 16) & 0xFFu);
        break;
    }
    case SampleFormat::PcmInt32: {
        const double scaled = std::round(clamped * 2147483648.0);
        const auto raw = static_cast<std::uint32_t>(
            static_cast<std::int32_t>(std::clamp(scaled, -2147483648.0, 2147483647.0)));
        for (int shift = 0, i = 0; shift < 32; shift += 8, ++i) {
            out[i] = static_cast<std::byte>((raw >> shift) & 0xFFu);
        }
        break;
    }
    case SampleFormat::Float32: {
        // Float formats carry values past full scale unchanged: clipping is the
        // caller's decision, and a float export exists precisely to preserve
        // headroom for later processing.
        const auto raw = std::bit_cast<std::uint32_t>(value);
        for (int shift = 0, i = 0; shift < 32; shift += 8, ++i) {
            out[i] = static_cast<std::byte>((raw >> shift) & 0xFFu);
        }
        break;
    }
    case SampleFormat::Float64: {
        const auto raw = std::bit_cast<std::uint64_t>(static_cast<double>(value));
        for (int shift = 0, i = 0; shift < 64; shift += 8, ++i) {
            out[i] = static_cast<std::byte>((raw >> shift) & 0xFFu);
        }
        break;
    }
    case SampleFormat::Unknown:
        break;
    }
}

} // namespace

WavWriter::WavWriter(std::ostream& stream, SampleRate sampleRate, ChannelLayout layout,
                     WavOptions options)
    : stream_(&stream), sampleRate_(sampleRate), layout_(layout), options_(std::move(options)) {
    bytesPerFrame_ = static_cast<std::uint32_t>(bytesPerSample(options_.format)) *
                     static_cast<std::uint32_t>(layout_.count());
}

Result<WavWriter> WavWriter::create(std::ostream& stream, SampleRate sampleRate,
                                    ChannelLayout layout, WavOptions options) {
    if (!sampleRate.isValid()) {
        return Error{ErrorCode::InvalidArgument, "invalid sample rate"};
    }
    if (layout.count() <= 0 || layout.count() > kMaxChannels) {
        return Error{ErrorCode::InvalidArgument, "invalid channel count"};
    }
    if (bytesPerSample(options.format) == 0) {
        return Error{ErrorCode::InvalidArgument, "unsupported output sample format"};
    }

    WavWriter writer{stream, sampleRate, layout, std::move(options)};

    const auto channels = static_cast<std::uint16_t>(layout.count());
    const auto bits = static_cast<std::uint16_t>(bytesPerSample(writer.options_.format) * 8);
    const bool isFloat = writer.options_.format == SampleFormat::Float32 ||
                         writer.options_.format == SampleFormat::Float64;
    const auto rate = static_cast<std::uint32_t>(sampleRate.hz());

    std::vector<std::byte> header;
    header.reserve(512);

    appendTag(header, "RIFF");
    appendU32(header, 0); // patched in finish()
    appendTag(header, "WAVE");

    appendTag(header, "fmt ");
    appendU32(header, 16);
    appendU16(header, isFloat ? std::uint16_t{3} : std::uint16_t{1});
    appendU16(header, channels);
    appendU32(header, rate);
    appendU32(header, rate * writer.bytesPerFrame_); // byte rate
    appendU16(header, static_cast<std::uint16_t>(writer.bytesPerFrame_));
    appendU16(header, bits);

    const BroadcastInfo& broadcast = writer.options_.metadata.broadcast;
    if (broadcast.present) {
        // Fixed 602-byte bext with no coding history, which is the common case
        // and keeps the header a predictable size.
        appendTag(header, "bext");
        appendU32(header, 602);
        appendFixedString(header, broadcast.description, 256);
        appendFixedString(header, broadcast.originator, 32);
        appendFixedString(header, broadcast.originatorReference, 32);
        appendFixedString(header, broadcast.originationDate, 10);
        appendFixedString(header, broadcast.originationTime, 8);
        appendU64(header, broadcast.timeReference);
        appendU16(header, 1);                                // BWF version
        header.insert(header.end(), 64 + 190, std::byte{0}); // UMID + reserved
    }

    appendTag(header, "data");
    appendU32(header, 0); // patched in finish()

    stream.write(reinterpret_cast<const char*>(header.data()),
                 static_cast<std::streamsize>(header.size()));
    if (!stream) {
        return Error{ErrorCode::IoFailure, "failed to write WAV header"};
    }

    // Record where the two size fields live so finish() can seek to them.
    writer.riffSizePosition_ = 4;
    writer.dataSizePosition_ = static_cast<std::streamoff>(header.size()) - 4;
    return writer;
}

Status WavWriter::write(ConstAudioBufferView frames) {
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
            return Error{ErrorCode::IoFailure, "failed to write WAV audio"};
        }
        done += batch;
    }

    framesWritten_ += frames.frames();
    return Status{};
}

Status WavWriter::finish() {
    if (stream_ == nullptr || finished_) {
        return Error{ErrorCode::InvalidArgument, "writer is not open"};
    }
    finished_ = true;

    const auto dataBytes = static_cast<std::uint64_t>(framesWritten_) * bytesPerFrame_;
    const auto headerBytes = static_cast<std::uint64_t>(dataSizePosition_) + 4;

    // RIFF cannot express more than 4 GB. Rather than write a file that decodes
    // as garbage, say so -- RF64 output is the fix, and is Phase 1 work.
    if (dataBytes + headerBytes > 0xFFFFFFFFull) {
        return Error{ErrorCode::UnsupportedFormat,
                     "output exceeds the 4 GB RIFF limit; RF64 output is not yet implemented"};
    }

    const auto endPosition = stream_->tellp();

    stream_->seekp(riffSizePosition_);
    writeLittleEndian(*stream_, static_cast<std::uint32_t>(headerBytes + dataBytes - 8));

    stream_->seekp(dataSizePosition_);
    writeLittleEndian(*stream_, static_cast<std::uint32_t>(dataBytes));

    stream_->seekp(endPosition);
    stream_->flush();

    if (!*stream_) {
        return Error{ErrorCode::IoFailure, "failed to patch WAV header sizes"};
    }
    return Status{};
}

Status WavWriter::writeFile(const std::filesystem::path& path, ConstAudioBufferView frames,
                            SampleRate sampleRate, ChannelLayout layout, WavOptions options) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    if (!stream) {
        return Error{ErrorCode::IoFailure, "cannot create " + path.string()};
    }

    auto writer = WavWriter::create(stream, sampleRate, layout, std::move(options));
    if (!writer) {
        return writer.error();
    }
    if (auto status = writer.value().write(frames); !status) {
        return status;
    }
    return writer.value().finish();
}

} // namespace sa::io
