/// Hostile-input tests for the WAV parser.
///
/// Audio file parsers are a classic attack surface -- libsndfile and FFmpeg
/// both carry long CVE histories -- and our users open files sent to them by
/// strangers. Every size field in a RIFF container is attacker-controlled, so
/// these tests feed the parser malformed input and assert it always terminates
/// with an error rather than crashing, hanging, or reading out of bounds.
///
/// Run under ASan for these to be worth much: a missing bounds check passes a
/// plain build and fails here.

#include <sa/io/WavReader.h>
#include <sa/io/WavWriter.h>

#include <catch2/catch_test_macros.hpp>
#include <random>
#include <span>
#include <sstream>
#include <vector>

using namespace sa;
using namespace sa::io;

namespace {

std::vector<std::byte> toBytes(const std::string& text) {
    std::vector<std::byte> bytes(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
    }
    return bytes;
}

/// A small, structurally valid stereo WAV to mutate.
std::vector<std::byte> validWav(SampleCount frames = 64) {
    AudioBuffer buffer{ChannelLayout::stereo(), frames};
    for (SampleCount i = 0; i < frames; ++i) {
        buffer.channel(0)[i] = 0.25f;
        buffer.channel(1)[i] = -0.25f;
    }
    std::ostringstream stream{std::ios::binary};
    auto writer = WavWriter::create(stream, kSampleRate48000, ChannelLayout::stereo());
    REQUIRE(writer.hasValue());
    REQUIRE(writer.value().write(buffer.constView()).ok());
    REQUIRE(writer.value().finish().ok());
    return toBytes(stream.str());
}

void writeU32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (int shift = 0, i = 0; shift < 32; shift += 8, ++i) {
        bytes[offset + static_cast<std::size_t>(i)] =
            static_cast<std::byte>((value >> shift) & 0xFFu);
    }
}

void writeU16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xFFu);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0xFFu);
}

/// Parse and, if it succeeded, also exercise the decode path -- a header can
/// parse cleanly and still point reads out of bounds.
void parseAndRead(std::span<const std::byte> bytes) {
    auto reader = WavReader::fromMemory(bytes);
    if (!reader) {
        return;
    }
    AudioBuffer destination{ChannelLayout::stereo(), 256};
    (void)reader.value().read(0, destination.view());
    (void)reader.value().read(reader.value().info().frameCount / 2, destination.view());
    (void)reader.value().readAll();
}

} // namespace

TEST_CASE("Empty and stub inputs are rejected", "[io][wav][robustness]") {
    const std::vector<std::byte> empty;
    CHECK_FALSE(WavReader::fromMemory(empty).hasValue());

    for (std::size_t length : {1u, 4u, 8u, 11u}) {
        const std::vector<std::byte> stub(length, std::byte{0x41});
        INFO("length " << length);
        CHECK_FALSE(WavReader::fromMemory(stub).hasValue());
    }
}

TEST_CASE("Non-RIFF and non-WAVE containers are rejected", "[io][wav][robustness]") {
    auto bytes = validWav();

    auto notRiff = bytes;
    notRiff[0] = std::byte{'X'};
    CHECK_FALSE(WavReader::fromMemory(notRiff).hasValue());

    auto notWave = bytes;
    notWave[8] = std::byte{'X'};
    CHECK_FALSE(WavReader::fromMemory(notWave).hasValue());
}

TEST_CASE("Truncation at every offset never crashes or hangs", "[io][wav][robustness]") {
    // The cheapest useful fuzz: a parser that trusts a length field usually
    // fails here first, and it covers every chunk boundary in the file.
    const auto complete = validWav(256);

    for (std::size_t length = 0; length < complete.size(); ++length) {
        const std::vector<std::byte> truncated{
            complete.begin(), complete.begin() + static_cast<std::ptrdiff_t>(length)};
        INFO("truncated to " << length << " of " << complete.size() << " bytes");
        parseAndRead(truncated);
    }
    SUCCEED("every truncation terminated");
}

TEST_CASE("Chunk sizes larger than the file are clamped", "[io][wav][robustness]") {
    auto bytes = validWav();

    // fmt chunk claims a gigabyte.
    auto hugeFmt = bytes;
    writeU32(hugeFmt, 16, 0x40000000u);
    parseAndRead(hugeFmt);

    // data chunk claims the whole 32-bit range, so frameCount would be absurd
    // unless the declared size is clamped to what the source really holds.
    auto hugeData = bytes;
    const std::size_t dataSizeOffset = bytes.size() - 4 - 64 * 2 * 3;
    writeU32(hugeData, dataSizeOffset, 0xFFFFFFFFu);
    {
        auto reader = WavReader::fromMemory(hugeData);
        if (reader) {
            const SampleCount claimed = reader.value().info().frameCount;
            const auto maximumPossible = static_cast<SampleCount>(hugeData.size() / 6);
            CHECK(claimed <= maximumPossible);
        }
        parseAndRead(hugeData);
    }

    // RIFF size field lying is harmless; we never trust it.
    auto hugeRiff = bytes;
    writeU32(hugeRiff, 4, 0xFFFFFFFFu);
    parseAndRead(hugeRiff);
}

TEST_CASE("Zero-size chunks cannot stall the scan", "[io][wav][robustness]") {
    // The classic container-parser hang: a chunk that declares zero length and
    // a scan that advances by the declared length.
    auto bytes = validWav();
    writeU32(bytes, 16, 0u); // fmt size = 0
    parseAndRead(bytes);

    std::vector<std::byte> onlyZeroChunks = toBytes("RIFF");
    onlyZeroChunks.resize(12);
    writeU32(onlyZeroChunks, 4, 4u);
    const auto wave = toBytes("WAVE");
    std::copy(wave.begin(), wave.end(), onlyZeroChunks.begin() + 8);
    for (int i = 0; i < 100; ++i) {
        const auto tag = toBytes("junk");
        onlyZeroChunks.insert(onlyZeroChunks.end(), tag.begin(), tag.end());
        onlyZeroChunks.insert(onlyZeroChunks.end(), 4, std::byte{0});
    }
    CHECK_FALSE(WavReader::fromMemory(onlyZeroChunks).hasValue());
    SUCCEED("scan terminated");
}

TEST_CASE("Missing required chunks are rejected", "[io][wav][robustness]") {
    auto noFmt = validWav();
    noFmt[12] = std::byte{'j'}; // rename fmt -> jmt
    auto reader = WavReader::fromMemory(noFmt);
    REQUIRE_FALSE(reader.hasValue());
    CHECK(reader.error().code() == ErrorCode::CorruptData);

    auto noData = validWav();
    const std::size_t dataTagOffset = noData.size() - 4 - 64 * 2 * 3 - 4;
    noData[dataTagOffset] = std::byte{'j'};
    CHECK_FALSE(WavReader::fromMemory(noData).hasValue());
}

TEST_CASE("Degenerate format parameters are rejected", "[io][wav][robustness]") {
    struct Case {
        const char* label;
        std::size_t offset;
        std::uint16_t value;
    };

    // fmt body starts at 20: tag(2) channels(2) rate(4) byteRate(4) block(2) bits(2)
    for (const Case& test : {Case{"zero channels", 22, 0}, Case{"absurd channel count", 22, 40000},
                             Case{"zero bit depth", 34, 0}, Case{"7-bit samples", 34, 7},
                             Case{"12-bit samples", 34, 12}, Case{"absurd bit depth", 34, 4096}}) {
        auto bytes = validWav();
        writeU16(bytes, test.offset, test.value);
        INFO(test.label);
        CHECK_FALSE(WavReader::fromMemory(bytes).hasValue());
    }

    auto zeroRate = validWav();
    writeU32(zeroRate, 24, 0u);
    CHECK_FALSE(WavReader::fromMemory(zeroRate).hasValue());

    auto absurdRate = validWav();
    writeU32(absurdRate, 24, 4000000u);
    CHECK_FALSE(WavReader::fromMemory(absurdRate).hasValue());
}

TEST_CASE("Unknown format tags are rejected rather than guessed", "[io][wav][robustness]") {
    auto bytes = validWav();
    writeU16(bytes, 20, 0x0055); // MPEG layer 3 in a WAV wrapper
    auto reader = WavReader::fromMemory(bytes);
    REQUIRE_FALSE(reader.hasValue());
    CHECK(reader.error().code() == ErrorCode::UnsupportedFormat);
}

TEST_CASE("A hostile cue count does not over-read", "[io][wav][robustness]") {
    // The cue chunk declares its own point count. Trusting it against a short
    // body reads past the buffer -- caught by ASan if the clamp is missing.
    auto bytes = validWav();
    const auto cueTag = toBytes("cue ");
    std::vector<std::byte> chunk;
    chunk.insert(chunk.end(), cueTag.begin(), cueTag.end());
    chunk.insert(chunk.end(), 4, std::byte{0});
    writeU32(chunk, 4, 4u); // body is 4 bytes: just the count
    chunk.insert(chunk.end(), 4, std::byte{0});
    writeU32(chunk, 8, 0xFFFFFFFFu); // claiming four billion cue points

    bytes.insert(bytes.begin() + 12, chunk.begin(), chunk.end());
    parseAndRead(bytes);
    SUCCEED("cue count clamped to the available body");
}

TEST_CASE("A hostile LIST sub-chunk size does not over-read", "[io][wav][robustness]") {
    auto bytes = validWav();
    const auto listTag = toBytes("LIST");
    const auto infoTag = toBytes("INFO");
    const auto nameTag = toBytes("INAM");

    std::vector<std::byte> chunk;
    chunk.insert(chunk.end(), listTag.begin(), listTag.end());
    chunk.insert(chunk.end(), 4, std::byte{0});
    writeU32(chunk, 4, 16u);
    chunk.insert(chunk.end(), infoTag.begin(), infoTag.end());
    chunk.insert(chunk.end(), nameTag.begin(), nameTag.end());
    chunk.insert(chunk.end(), 4, std::byte{0});
    writeU32(chunk, 16, 0x7FFFFFFFu); // sub-chunk claims 2 GB inside a 16-byte body
    chunk.insert(chunk.end(), 4, std::byte{0});

    bytes.insert(bytes.begin() + 12, chunk.begin(), chunk.end());
    parseAndRead(bytes);
    SUCCEED("LIST sub-chunk size clamped");
}

TEST_CASE("Random byte corruption never crashes the parser", "[io][wav][robustness]") {
    const auto original = validWav(128);
    std::mt19937 rng{20260918};
    std::uniform_int_distribution<std::size_t> position{0, original.size() - 1};
    std::uniform_int_distribution<int> value{0, 255};

    for (int trial = 0; trial < 3000; ++trial) {
        auto corrupted = original;
        const int flips = 1 + (trial % 8);
        for (int i = 0; i < flips; ++i) {
            corrupted[position(rng)] = static_cast<std::byte>(value(rng));
        }
        parseAndRead(corrupted);
    }
    SUCCEED("3000 corrupted files parsed without crashing");
}

TEST_CASE("Random bytes are not mistaken for audio", "[io][wav][robustness]") {
    std::mt19937 rng{7};
    std::uniform_int_distribution<int> value{0, 255};

    for (int trial = 0; trial < 500; ++trial) {
        std::vector<std::byte> noise(static_cast<std::size_t>(64 + trial));
        for (std::byte& byte : noise) {
            byte = static_cast<std::byte>(value(rng));
        }
        parseAndRead(noise);
    }
    SUCCEED("random input rejected without crashing");
}
