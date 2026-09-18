#include <sa/io/AudioFile.h>
#include <sa/io/WavWriter.h>

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace sa;
using namespace sa::io;
using Catch::Approx;

namespace {

std::span<const std::byte> asBytes(const char* text, std::size_t length) {
    return std::span{reinterpret_cast<const std::byte*>(text), length};
}

/// A temporary file that removes itself, so a failing assertion cannot leave
/// litter behind for the next run to trip over.
class ScopedFile {
public:
    explicit ScopedFile(std::string name)
        : path_(std::filesystem::temp_directory_path() / std::move(name)) {}

    ~ScopedFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    ScopedFile(const ScopedFile&) = delete;
    ScopedFile& operator=(const ScopedFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void writeBytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

/// A valid MPEG-1 layer III frame: a real four-byte header followed by a body
/// of exactly the length that header declares.
///
/// The body is zeroes. No audio is needed, because the thing being tested is
/// whether the sniffer follows the chain of headers -- which is what separates
/// a real frame from four bytes that happen to look like one.
std::vector<std::byte> mpegFrame(std::uint8_t bitRateIndex, std::uint8_t sampleRateIndex) {
    // 0xFB: MPEG 1, layer III, no CRC. 0xC4: single channel, no emphasis.
    const auto third = static_cast<std::uint8_t>((bitRateIndex << 4) | (sampleRateIndex << 2));
    std::vector<std::byte> frame{std::byte{0xFF}, std::byte{0xFB}, static_cast<std::byte>(third),
                                 std::byte{0xC4}};

    constexpr std::array<std::uint32_t, 16> kBitRatesKbps{0,   32,  40,  48,  56,  64,  80,  96,
                                                          112, 128, 160, 192, 224, 256, 320, 0};
    constexpr std::array<std::uint32_t, 4> kSampleRates{44100, 48000, 32000, 0};
    const std::uint32_t length =
        144u * 1000u * kBitRatesKbps[bitRateIndex] / kSampleRates[sampleRateIndex];
    frame.resize(length, std::byte{0});
    return frame;
}

/// An ID3v2 tag of `bodyBytes` bytes, as an encoder writes in front of the
/// audio.
std::vector<std::byte> id3v2Tag(std::uint8_t major, std::uint32_t bodyBytes) {
    std::vector<std::byte> tag{std::byte{'I'}, std::byte{'D'},
                               std::byte{'3'}, static_cast<std::byte>(major),
                               std::byte{0},   std::byte{0}};
    for (int shift = 21; shift >= 0; shift -= 7) {
        tag.push_back(static_cast<std::byte>((bodyBytes >> shift) & 0x7Fu));
    }
    tag.resize(tag.size() + bodyBytes, std::byte{0});
    return tag;
}

/// The first eight bytes of a FLAC stream: the magic, then the metadata block
/// header the format requires -- a STREAMINFO block of 34 bytes.
std::vector<std::byte> flacHeader() {
    return {std::byte{'f'},  std::byte{'L'}, std::byte{'a'}, std::byte{'C'},
            std::byte{0x80}, std::byte{0},   std::byte{0},   std::byte{34}};
}

void append(std::vector<std::byte>& into, const std::vector<std::byte>& what) {
    into.insert(into.end(), what.begin(), what.end());
}

AudioBuffer makeTone(SampleCount frames) {
    AudioBuffer buffer{ChannelLayout::stereo(), frames};
    for (SampleCount i = 0; i < frames; ++i) {
        const auto value = static_cast<float>(std::sin(static_cast<double>(i) * 0.01)) * 0.5f;
        buffer.channel(0)[i] = value;
        buffer.channel(1)[i] = -value;
    }
    return buffer;
}

} // namespace

TEST_CASE("Containers are identified from their leading bytes", "[io][format]") {
    CHECK(detectFormat(asBytes("RIFF\0\0\0\0WAVE", 12)) == ContainerFormat::Wave);
    CHECK(detectFormat(asBytes("RF64\0\0\0\0WAVE", 12)) == ContainerFormat::Wave);
    CHECK(detectFormat(asBytes("FORM\0\0\0\0AIFF", 12)) == ContainerFormat::Aiff);
    CHECK(detectFormat(asBytes("FORM\0\0\0\0AIFC", 12)) == ContainerFormat::Aiff);
}

TEST_CASE("Both the container tag and the form type must match", "[io][format]") {
    // A file merely starting with the right four letters is not a WAVE. RIFF
    // also carries AVI and WebP, and handing either to the WAV parser would be
    // giving the wrong parser a hostile file.
    CHECK(detectFormat(asBytes("RIFF\0\0\0\0AVI ", 12)) == ContainerFormat::Unknown);
    CHECK(detectFormat(asBytes("RIFF\0\0\0\0WEBP", 12)) == ContainerFormat::Unknown);
    CHECK(detectFormat(asBytes("FORM\0\0\0\0ILBM", 12)) == ContainerFormat::Unknown);
}

TEST_CASE("Short and empty input is not misidentified", "[io][format]") {
    CHECK(detectFormat(std::span<const std::byte>{}) == ContainerFormat::Unknown);
    CHECK(detectFormat(asBytes("RIFF", 4)) == ContainerFormat::Unknown);
    CHECK(detectFormat(asBytes("RIFF\0\0\0\0WAV", 11)) == ContainerFormat::Unknown);
    CHECK(detectFormat(asBytes("\0\0\0\0\0\0\0\0\0\0\0\0", 12)) == ContainerFormat::Unknown);
}

TEST_CASE("Detection ignores the file extension", "[io][format]") {
    // Exporters really do write AIFF into a .wav. Trusting the extension would
    // hand the file to the wrong parser, so detection reads content only.
    const ScopedFile misnamed{"sa-liar.wav"};

    std::vector<std::byte> aiffHeader;
    for (char c : std::string{"FORM\0\0\0\0AIFC", 12}) {
        aiffHeader.push_back(static_cast<std::byte>(c));
    }
    writeBytes(misnamed.path(), aiffHeader);

    auto detected = detectFormat(misnamed.path());
    REQUIRE(detected.hasValue());
    CHECK(detected.value() == ContainerFormat::Aiff);
}

TEST_CASE("openAudioFile decodes a real WAV", "[io][format]") {
    const ScopedFile file{"sa-openaudiofile.wav"};
    const AudioBuffer source = makeTone(500);
    REQUIRE(WavWriter::writeFile(file.path(), source.constView(), kSampleRate48000,
                                 ChannelLayout::stereo())
                .ok());

    auto opened = openAudioFile(file.path());
    REQUIRE(opened.hasValue());

    const auto& audio = *opened.value();
    CHECK(audio.info().frameCount == 500);
    CHECK(audio.info().channelCount() == 2);
    CHECK(audio.info().sampleRate == kSampleRate48000);

    AudioBuffer decoded{ChannelLayout::stereo(), 500};
    auto got = audio.read(0, decoded.view());
    REQUIRE(got.hasValue());
    CHECK(got.value() == 500);
    CHECK(decoded.channel(0)[100] == Approx(source.channel(0)[100]).margin(1e-4));
    CHECK(decoded.channel(1)[100] == Approx(source.channel(1)[100]).margin(1e-4));
}

TEST_CASE("The returned source outlives the call", "[io][format]") {
    // openAudioFile hands back a shared_ptr that owns its own file handle, so a
    // caller can keep it long after the factory returned -- which is what the
    // document model does with every source it loads.
    const ScopedFile file{"sa-lifetime.wav"};
    REQUIRE(WavWriter::writeFile(file.path(), makeTone(200).constView(), kSampleRate44100,
                                 ChannelLayout::stereo())
                .ok());

    std::shared_ptr<const AudioSource> kept;
    {
        auto opened = openAudioFile(file.path());
        REQUIRE(opened.hasValue());
        kept = opened.value();
    }

    AudioBuffer decoded{ChannelLayout::stereo(), 64};
    auto got = kept->read(10, decoded.view());
    REQUIRE(got.hasValue());
    CHECK(got.value() == 64);
}

TEST_CASE("Unsupported and missing files fail cleanly", "[io][format]") {
    auto missing = openAudioFile("/nonexistent/nope.wav");
    REQUIRE_FALSE(missing.hasValue());
    CHECK(missing.error().code() == ErrorCode::IoFailure);

    const ScopedFile garbage{"sa-garbage.wav"};
    writeBytes(garbage.path(), std::vector<std::byte>(64, std::byte{0x7F}));

    auto opened = openAudioFile(garbage.path());
    REQUIRE_FALSE(opened.hasValue());
    CHECK(opened.error().code() == ErrorCode::UnsupportedFormat);
}

TEST_CASE("An empty file is rejected without crashing", "[io][format]") {
    const ScopedFile empty{"sa-empty.wav"};
    writeBytes(empty.path(), {});
    CHECK_FALSE(openAudioFile(empty.path()).hasValue());
}

TEST_CASE("FLAC is identified by its magic and its first metadata block", "[io][format][flac]") {
    auto flac = flacHeader();
    CHECK(detectFormat(std::span{flac}) == ContainerFormat::Flac);

    // The format requires the first metadata block to be a 34-byte STREAMINFO.
    // Checking it is the same move as checking a RIFF file's form type: it
    // rules out something that merely starts with the right four letters.
    auto wrongBlockType = flac;
    wrongBlockType[4] = std::byte{0x84};
    CHECK(detectFormat(std::span{wrongBlockType}) == ContainerFormat::Unknown);

    auto wrongBlockSize = flac;
    wrongBlockSize[7] = std::byte{40};
    CHECK(detectFormat(std::span{wrongBlockSize}) == ContainerFormat::Unknown);

    // With only the magic visible there is nothing to check it against, and
    // four bytes of "fLaC" is evidence enough on its own.
    CHECK(detectFormat(std::span{flac}.first(4)) == ContainerFormat::Flac);
}

TEST_CASE("MP3 is identified by a chain of frames, not by one sync word", "[io][format][mp3]") {
    // Three consecutive frames, each starting exactly where the one before
    // said it would end.
    std::vector<std::byte> chain;
    for (int i = 0; i < 3; ++i) {
        append(chain, mpegFrame(9, 0)); // 128 kbit/s, 44.1 kHz
    }
    CHECK(detectFormat(std::span{chain}) == ContainerFormat::Mp3);

    // One frame header and then nothing that follows from it. This is the case
    // a naive sync search gets wrong, and it is not rare: a sync pattern turns
    // up in roughly one byte pair in two thousand, so any file with an image
    // in it has several.
    std::vector<std::byte> lonely = mpegFrame(9, 0);
    lonely.resize(4);
    lonely.resize(4096, std::byte{0});
    CHECK(detectFormat(std::span{lonely}) == ContainerFormat::Unknown);

    // Two good frames and then rubbish where the third should be.
    std::vector<std::byte> twoThenJunk;
    append(twoThenJunk, mpegFrame(9, 0));
    append(twoThenJunk, mpegFrame(9, 0));
    twoThenJunk.resize(twoThenJunk.size() + 512, std::byte{0x11});
    CHECK(detectFormat(std::span{twoThenJunk}) == ContainerFormat::Unknown);

    // Frames that disagree about the sample rate are not one stream.
    std::vector<std::byte> inconsistent;
    append(inconsistent, mpegFrame(9, 0));
    append(inconsistent, mpegFrame(9, 1));
    append(inconsistent, mpegFrame(9, 1));
    CHECK(detectFormat(std::span{inconsistent}) == ContainerFormat::Unknown);
}

TEST_CASE("Reserved values in a frame header disqualify it", "[io][format][mp3]") {
    // Each of these is a field the specification reserves. A parser that only
    // checks the sync bits accepts all of them.
    std::vector<std::byte> chain;
    for (int i = 0; i < 3; ++i) {
        append(chain, mpegFrame(9, 0));
    }

    auto freeFormat = chain;
    freeFormat[2] = std::byte{0x04}; // bit rate index 0: free format, no length
    CHECK(detectFormat(std::span{freeFormat}) == ContainerFormat::Unknown);

    auto badBitRate = chain;
    badBitRate[2] = std::byte{0xF4}; // bit rate index 15: invalid
    CHECK(detectFormat(std::span{badBitRate}) == ContainerFormat::Unknown);

    auto reservedRate = chain;
    reservedRate[2] = std::byte{0x9C}; // sample rate index 3: reserved
    CHECK(detectFormat(std::span{reservedRate}) == ContainerFormat::Unknown);

    auto reservedVersion = chain;
    reservedVersion[1] = std::byte{0xEB}; // version 1: reserved
    CHECK(detectFormat(std::span{reservedVersion}) == ContainerFormat::Unknown);

    auto reservedLayer = chain;
    reservedLayer[1] = std::byte{0xF9}; // layer 0: reserved
    CHECK(detectFormat(std::span{reservedLayer}) == ContainerFormat::Unknown);
}

TEST_CASE("Things that begin with 0xFF are not all MP3s", "[io][format][mp3]") {
    // A JPEG starts FF D8, which passes a sync test that only looks at the
    // first byte.
    const std::vector<std::byte> jpeg{std::byte{0xFF}, std::byte{0xD8}, std::byte{0xFF},
                                      std::byte{0xE0}, std::byte{0x00}, std::byte{0x10},
                                      std::byte{'J'},  std::byte{'F'},  std::byte{'I'},
                                      std::byte{'F'},  std::byte{0},    std::byte{1}};
    CHECK(detectFormat(std::span{jpeg}) == ContainerFormat::Unknown);

    const std::vector<std::byte> allOnes(2048, std::byte{0xFF});
    CHECK(detectFormat(std::span{allOnes}) == ContainerFormat::Unknown);

    const std::vector<std::byte> allZeroes(2048, std::byte{0});
    CHECK(detectFormat(std::span{allZeroes}) == ContainerFormat::Unknown);
}

TEST_CASE("An ID3v2 tag is looked past rather than through", "[io][format][mp3]") {
    // The tag is not a container; it sits in front of the audio. It is also
    // routinely larger than anything a sniffer will read, so an ID3v2 header at
    // offset zero has to be taken as an MP3 on its own account.
    const auto tag = id3v2Tag(3, 40000);
    CHECK(detectFormat(std::span{tag}) == ContainerFormat::Mp3);

    // ID3v2 in front of a FLAC is out of spec, but it happens, and the tag is
    // not what the file is.
    std::vector<std::byte> taggedFlac = id3v2Tag(4, 16);
    append(taggedFlac, flacHeader());
    CHECK(detectFormat(std::span{taggedFlac}) == ContainerFormat::Flac);

    // A tag size is stored seven bits to the byte precisely so that it can
    // never contain a byte that looks like a frame sync. A high bit set in one
    // means this is not an ID3v2 header.
    auto notSyncsafe = tag;
    notSyncsafe[7] = std::byte{0x80};
    CHECK(detectFormat(std::span{notSyncsafe}) == ContainerFormat::Unknown);

    // Only versions 2, 3 and 4 were ever published.
    auto impossibleVersion = tag;
    impossibleVersion[3] = std::byte{0xFF};
    CHECK(detectFormat(std::span{impossibleVersion}) == ContainerFormat::Unknown);

    // Prose passes the syncsafe test trivially -- every ASCII byte is under
    // 0x80 -- so the version check is what keeps a text file about ID3 from
    // being opened as audio.
    const std::string sentence{"ID3 is the tag format MP3 files carry their titles in."};
    std::vector<std::byte> text;
    for (char c : sentence) {
        text.push_back(static_cast<std::byte>(c));
    }
    CHECK(detectFormat(std::span{text}) == ContainerFormat::Unknown);
}

TEST_CASE("A WAV holding MPEG audio is still a WAV", "[io][format]") {
    // WAVE_FORMAT_MPEGLAYER3 exists, and the container is what decides which
    // parser sees the file. Checking RIFF first is what makes that so.
    std::vector<std::byte> riff;
    for (char c : std::string{"RIFF\0\0\0\0WAVE", 12}) {
        riff.push_back(static_cast<std::byte>(c));
    }
    append(riff, mpegFrame(9, 0));
    CHECK(detectFormat(std::span{riff}) == ContainerFormat::Wave);
}

TEST_CASE("Every container names itself", "[io][format]") {
    // The status line puts these in front of the user, so they are part of the
    // interface rather than debug output.
    CHECK(toString(ContainerFormat::Unknown) == "unknown");
    CHECK(toString(ContainerFormat::Wave) == "WAVE");
    CHECK(toString(ContainerFormat::Aiff) == "AIFF");
    CHECK(toString(ContainerFormat::Flac) == "FLAC");
    CHECK(toString(ContainerFormat::Mp3) == "MP3");
}
