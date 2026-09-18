#include <sa/io/AiffReader.h>
#include <sa/io/AudioFile.h>
#include <sa/io/FlacReader.h>
#include <sa/io/Mp3Reader.h>
#include <sa/io/WavReader.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <optional>

namespace sa::io {

namespace {

bool matches(std::span<const std::byte> bytes, std::size_t offset, const char* tag) noexcept {
    if (bytes.size() < offset + 4) {
        return false;
    }
    for (std::size_t i = 0; i < 4; ++i) {
        if (std::to_integer<char>(bytes[offset + i]) != tag[i]) {
            return false;
        }
    }
    return true;
}

std::uint8_t byteAt(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    return std::to_integer<std::uint8_t>(bytes[offset]);
}

/// True if a native FLAC stream starts at `offset`.
///
/// The magic is only four bytes, so where the buffer reaches further the first
/// metadata block header is checked as well: FLAC requires it to be a STREAMINFO
/// block of exactly 34 bytes. That is the same reasoning as checking a RIFF
/// container's form type -- it rules out a file that merely starts with the
/// right four letters.
bool isFlacStream(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    if (!matches(bytes, offset, "fLaC")) {
        return false;
    }
    if (bytes.size() < offset + 8) {
        return true; // nothing further to check it against
    }
    const auto blockType = static_cast<std::uint8_t>(byteAt(bytes, offset + 4) & 0x7Fu);
    const std::uint32_t blockLength =
        (static_cast<std::uint32_t>(byteAt(bytes, offset + 5)) << 16) |
        (static_cast<std::uint32_t>(byteAt(bytes, offset + 6)) << 8) |
        static_cast<std::uint32_t>(byteAt(bytes, offset + 7));
    return blockType == 0 && blockLength == 34;
}

/// Sample rates in Hz, indexed by the version and sample-rate fields of an MPEG
/// audio frame header. Zero entries are the values the specification reserves.
constexpr std::array<std::array<std::uint32_t, 4>, 4> kMpegSampleRates{{
    {{11025, 12000, 8000, 0}},  // version 0: MPEG 2.5
    {{0, 0, 0, 0}},             // version 1: reserved
    {{22050, 24000, 16000, 0}}, // version 2: MPEG 2
    {{44100, 48000, 32000, 0}}, // version 3: MPEG 1
}};

/// Bit rates in kbit/s, indexed by mpegBitRateRow() and the four-bit bit-rate
/// field. Index 0 is "free format" and index 15 is invalid; both read as zero
/// and both are rejected here, because a free-format frame does not state its
/// own length and length is the whole basis of the check below.
constexpr std::array<std::array<std::uint16_t, 16>, 5> kMpegBitRates{{
    {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0}}, // v1 layer I
    {{0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0}},    // v1 layer II
    {{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0}},     // v1 layer III
    {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0}},    // v2 layer I
    {{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0}},         // v2 layer II/III
}};

/// Which row of kMpegBitRates applies. Layer IDs run backwards in the header:
/// 3 is layer I, 2 is layer II, 1 is layer III.
std::size_t mpegBitRateRow(std::uint8_t versionId, std::uint8_t layerId) noexcept {
    if (versionId == 3) { // MPEG 1
        return layerId == 3 ? 0u : (layerId == 2 ? 1u : 2u);
    }
    return layerId == 3 ? 3u : 4u;
}

/// The fields of an MPEG audio frame header that a sniffer cares about.
struct MpegFrame {
    std::uint32_t lengthBytes = 0;
    std::uint8_t versionId = 0;
    std::uint8_t layerId = 0;
    std::uint8_t sampleRateIndex = 0;
};

/// Parse the four-byte frame header at `offset`, or nothing if it is not one.
///
/// Every field that has reserved values is checked: a reserved version, layer,
/// bit rate or sample rate means this is not a frame header however much the
/// sync bits want it to be.
std::optional<MpegFrame> parseMpegFrame(std::span<const std::byte> bytes,
                                        std::size_t offset) noexcept {
    if (bytes.size() < offset + 4) {
        return std::nullopt;
    }
    if (byteAt(bytes, offset) != 0xFFu || (byteAt(bytes, offset + 1) & 0xE0u) != 0xE0u) {
        return std::nullopt;
    }

    const std::uint8_t second = byteAt(bytes, offset + 1);
    const std::uint8_t third = byteAt(bytes, offset + 2);

    MpegFrame frame;
    frame.versionId = static_cast<std::uint8_t>((second >> 3) & 0x03u);
    frame.layerId = static_cast<std::uint8_t>((second >> 1) & 0x03u);
    frame.sampleRateIndex = static_cast<std::uint8_t>((third >> 2) & 0x03u);
    const auto bitRateIndex = static_cast<std::size_t>((third >> 4) & 0x0Fu);
    const auto padding = static_cast<std::uint32_t>((third >> 1) & 0x01u);

    if (frame.versionId == 1 || frame.layerId == 0) {
        return std::nullopt;
    }

    const std::uint32_t sampleRate = kMpegSampleRates[frame.versionId][frame.sampleRateIndex];
    const std::uint32_t bitRate =
        1000u * kMpegBitRates[mpegBitRateRow(frame.versionId, frame.layerId)][bitRateIndex];
    if (sampleRate == 0 || bitRate == 0) {
        return std::nullopt;
    }

    if (frame.layerId == 3) {
        // Layer I counts in four-byte slots and carries 384 samples a frame.
        frame.lengthBytes = (12u * bitRate / sampleRate + padding) * 4u;
    } else {
        // Layers II and III carry 1152 samples a frame, except layer III on
        // MPEG 2 and 2.5, which halves it to 576.
        const std::uint32_t bytesPerFrameCoefficient =
            (frame.layerId == 1 && frame.versionId != 3) ? 72u : 144u;
        frame.lengthBytes = bytesPerFrameCoefficient * bitRate / sampleRate + padding;
    }

    if (frame.lengthBytes < 4) {
        return std::nullopt; // cannot even hold its own header
    }
    return frame;
}

/// True if a run of consistent MPEG audio frames starts at `offset`.
///
/// One four-byte header is not evidence of anything: roughly one byte pair in
/// 2048 passes the sync test, and a file with an embedded image contains plenty
/// of them. What makes a header believable is that it predicts where the next
/// one starts, so this follows the chain and requires consecutive frames to
/// agree on version, layer and sample rate -- none of which a real stream
/// changes part way through.
bool hasMpegFrameChain(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    constexpr int kFramesWanted = 3;

    auto frame = parseMpegFrame(bytes, offset);
    if (!frame.has_value()) {
        return false;
    }

    std::size_t cursor = offset;
    for (int found = 1; found < kFramesWanted; ++found) {
        cursor += frame->lengthBytes;
        if (cursor + 4 > bytes.size()) {
            // The buffer ran out before the next frame could be checked. A file
            // that short has nothing left in it to contradict the first frame.
            return true;
        }
        const auto next = parseMpegFrame(bytes, cursor);
        if (!next.has_value() || next->versionId != frame->versionId ||
            next->layerId != frame->layerId || next->sampleRateIndex != frame->sampleRateIndex) {
            return false;
        }
        frame = next;
    }
    return true;
}

/// Where an ID3v2 tag at the start of the buffer ends, if there is one.
///
/// The tag is not a container: it sits in front of the audio, so it has to be
/// stepped over before anything can be decided. Its length is four "syncsafe"
/// bytes of seven bits each -- the format's own trick for making sure a length
/// can never contain a byte that looks like a frame sync -- so a high bit set
/// in any of them means this is not an ID3v2 header.
///
/// Only versions 2, 3 and 4 exist, and requiring one of them costs nothing
/// while keeping ordinary text files -- which pass the syncsafe test trivially,
/// every ASCII byte being under 0x80 -- from being read as tagged audio.
std::optional<std::uint64_t> id3TagEnd(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < 10) {
        return std::nullopt;
    }
    if (std::to_integer<char>(bytes[0]) != 'I' || std::to_integer<char>(bytes[1]) != 'D' ||
        std::to_integer<char>(bytes[2]) != '3') {
        return std::nullopt;
    }

    const std::uint8_t major = byteAt(bytes, 3);
    const std::uint8_t revision = byteAt(bytes, 4);
    const std::uint8_t flags = byteAt(bytes, 5);
    if (major < 2 || major > 4 || revision == 0xFFu) {
        return std::nullopt;
    }

    std::uint64_t size = 0;
    for (std::size_t i = 6; i < 10; ++i) {
        const std::uint8_t part = byteAt(bytes, i);
        if ((part & 0x80u) != 0) {
            return std::nullopt;
        }
        size = (size << 7) | part;
    }

    // Only ID3v2.4 has a footer, and only then does that flag bit mean this.
    const std::uint64_t footer = (major == 4 && (flags & 0x10u) != 0) ? 10u : 0u;
    return 10u + size + footer;
}

} // namespace

std::string_view toString(ContainerFormat format) noexcept {
    switch (format) {
    case ContainerFormat::Unknown:
        return "unknown";
    case ContainerFormat::Wave:
        return "WAVE";
    case ContainerFormat::Aiff:
        return "AIFF";
    case ContainerFormat::Flac:
        return "FLAC";
    case ContainerFormat::Mp3:
        return "MP3";
    }
    return "unknown";
}

ContainerFormat detectFormat(std::span<const std::byte> leadingBytes) noexcept {
    // Both IFF-derived containers carry a four-character type at offset 0 and a
    // form type at offset 8. Checking both rules out a file that merely starts
    // with the right four letters.
    if ((matches(leadingBytes, 0, "RIFF") || matches(leadingBytes, 0, "RF64")) &&
        matches(leadingBytes, 8, "WAVE")) {
        return ContainerFormat::Wave;
    }
    if (matches(leadingBytes, 0, "FORM") &&
        (matches(leadingBytes, 8, "AIFF") || matches(leadingBytes, 8, "AIFC"))) {
        return ContainerFormat::Aiff;
    }
    if (isFlacStream(leadingBytes, 0)) {
        return ContainerFormat::Flac;
    }

    if (const auto tagEnd = id3TagEnd(leadingBytes); tagEnd.has_value()) {
        const auto audioStart =
            static_cast<std::size_t>(std::min<std::uint64_t>(*tagEnd, leadingBytes.size()));
        // An ID3v2 tag on a FLAC is out of spec, but encoders do it and players
        // accept it, so look before assuming what the tag is attached to.
        if (isFlacStream(leadingBytes, audioStart)) {
            return ContainerFormat::Flac;
        }
        // Otherwise it is an MP3. The tag is usually larger than this window --
        // embedded artwork alone runs to tens of kilobytes -- so there is often
        // no first frame here to confirm it against, and guessing anything else
        // from an ID3v2 header at offset zero would be perverse. If the file
        // turns out to hold no audio, the decoder is the one that says so.
        return ContainerFormat::Mp3;
    }

    if (hasMpegFrameChain(leadingBytes, 0)) {
        return ContainerFormat::Mp3;
    }

    return ContainerFormat::Unknown;
}

Result<ContainerFormat> detectFormat(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        return Error{ErrorCode::IoFailure, "cannot open " + path.string()};
    }
    // Twelve bytes settled every container this could decode until MP3 arrived;
    // see kFormatSniffBytes for why that is no longer enough. One buffered read
    // of a few kilobytes costs no more than a read of twelve.
    std::array<std::byte, kFormatSniffBytes> header{};
    stream.read(reinterpret_cast<char*>(header.data()),
                static_cast<std::streamsize>(header.size()));
    const auto got = static_cast<std::size_t>(stream.gcount());
    return detectFormat(std::span{header.data(), got});
}

Result<std::shared_ptr<const AudioSource>> openAudioFile(const std::filesystem::path& path) {
    auto format = detectFormat(path);
    if (!format) {
        return format.error();
    }

    switch (format.value()) {
    case ContainerFormat::Wave: {
        auto reader = WavReader::open(path);
        if (!reader) {
            return reader.error();
        }
        return std::shared_ptr<const AudioSource>{
            std::make_shared<const WavReader>(std::move(reader).value())};
    }
    case ContainerFormat::Aiff: {
        auto reader = AiffReader::open(path);
        if (!reader) {
            return reader.error();
        }
        return std::shared_ptr<const AudioSource>{
            std::make_shared<const AiffReader>(std::move(reader).value())};
    }
    case ContainerFormat::Flac: {
        auto reader = FlacReader::open(path);
        if (!reader) {
            return reader.error();
        }
        return std::shared_ptr<const AudioSource>{
            std::make_shared<const FlacReader>(std::move(reader).value())};
    }
    case ContainerFormat::Mp3: {
        auto reader = Mp3Reader::open(path);
        if (!reader) {
            return reader.error();
        }
        return std::shared_ptr<const AudioSource>{
            std::make_shared<const Mp3Reader>(std::move(reader).value())};
    }
    case ContainerFormat::Unknown:
        break;
    }
    return Error{ErrorCode::UnsupportedFormat,
                 path.string() + ": not a container this build can decode"};
}

} // namespace sa::io
