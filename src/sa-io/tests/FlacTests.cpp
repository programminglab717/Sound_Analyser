/// Tests for the FLAC reader.
///
/// The fixtures are encoded here rather than checked in, the same as every
/// other reader's tests: a binary blob in the repository proves that one file
/// decodes, where a generator proves that a family of them does and lets a
/// hostile-input test mutate a structurally valid file rather than a random
/// one. The encoder below emits VERBATIM subframes -- raw samples, no
/// prediction -- which is a real and legal FLAC stream that any decoder must
/// accept, and which makes the decode losslessly comparable against the samples
/// that went in.

#include <sa/io/AudioFile.h>
#include <sa/io/FlacReader.h>
#include <sa/io/PeakPyramid.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace sa;
using namespace sa::io;

namespace {

/// A bit-level writer, most significant bit first.
///
/// FLAC is bit-packed rather than byte-aligned: a frame header is a run of
/// three- and four-bit fields, and a VERBATIM subframe is raw samples at
/// whatever the stream's bit depth happens to be.
class BitWriter {
public:
    void write(std::uint64_t value, int bits) {
        for (int shift = bits - 1; shift >= 0; --shift) {
            const auto bit = static_cast<std::uint8_t>((value >> shift) & 1u);
            accumulator_ = static_cast<std::uint8_t>((accumulator_ << 1) | bit);
            ++pending_;
            if (pending_ == 8) {
                bytes_.push_back(static_cast<std::byte>(accumulator_));
                accumulator_ = 0;
                pending_ = 0;
            }
        }
    }

    /// Two's complement in `bits` bits, which is how FLAC stores samples.
    void writeSigned(std::int32_t value, int bits) {
        const std::uint64_t mask = bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1u;
        write(static_cast<std::uint64_t>(static_cast<std::int64_t>(value)) & mask, bits);
    }

    void padToByte() {
        while (pending_ != 0) {
            write(0, 1);
        }
    }

    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return bytes_; }

private:
    std::vector<std::byte> bytes_;
    std::uint8_t accumulator_ = 0;
    int pending_ = 0;
};

/// CRC-8 over a frame header: polynomial x^8 + x^2 + x + 1, initialised to zero.
std::uint8_t crc8(std::span<const std::byte> bytes) {
    std::uint8_t crc = 0;
    for (std::byte value : bytes) {
        crc ^= std::to_integer<std::uint8_t>(value);
        for (int bit = 0; bit < 8; ++bit) {
            // Unsigned throughout: the promotion of a uint8_t to int is what
            // would otherwise make this a signed shift mixed with an unsigned
            // mask, which -Wsign-conversion rightly objects to.
            const unsigned int shifted = static_cast<unsigned int>(crc) << 1U;
            crc = static_cast<std::uint8_t>((crc & 0x80U) != 0U ? (shifted ^ 0x07U) : shifted);
        }
    }
    return crc;
}

/// CRC-16 over a whole frame: polynomial x^16 + x^15 + x^2 + 1, initialised to
/// zero. dr_flac checks this, so getting it wrong fails loudly rather than
/// quietly producing a fixture that only our own code accepts.
std::uint16_t crc16(std::span<const std::byte> bytes) {
    std::uint16_t crc = 0;
    for (std::byte value : bytes) {
        crc ^= static_cast<std::uint16_t>(
            static_cast<unsigned int>(std::to_integer<std::uint8_t>(value)) << 8U);
        for (int bit = 0; bit < 8; ++bit) {
            const unsigned int shifted = static_cast<unsigned int>(crc) << 1U;
            crc = static_cast<std::uint16_t>((crc & 0x8000U) != 0U ? (shifted ^ 0x8005U) : shifted);
        }
    }
    return crc;
}

/// FLAC numbers its frames with the variable-length encoding UTF-8 uses for
/// code points, widened to 36 bits.
void writeFrameNumber(BitWriter& out, std::uint64_t number) {
    if (number < 0x80u) {
        out.write(number, 8);
        return;
    }
    int byteCount = 2;
    while (byteCount < 7 && number >= (std::uint64_t{1} << (5 * byteCount + 1))) {
        ++byteCount;
    }
    const int payloadBits = 6 * (byteCount - 1);
    const std::uint64_t lead = (0xFFuLL << (8 - byteCount)) & 0xFFuLL;
    out.write(lead | (number >> payloadBits), 8);
    for (int i = byteCount - 2; i >= 0; --i) {
        out.write(0x80u | ((number >> (6 * i)) & 0x3Fu), 8);
    }
}

/// Everything about the stream to be built. At namespace scope on purpose:
/// a struct nested in a class cannot be named in a default argument on MSVC.
struct FlacSpec {
    std::uint32_t sampleRate = 44100;
    int bitsPerSample = 16;
    SampleCount blockSize = 512;
    std::uint32_t paddingBytes = 0;
    /// Declare a total sample count of zero, which in FLAC means "unknown"
    /// rather than "empty" -- what an encoder writes when it cannot seek back
    /// to patch STREAMINFO, as happens when FLAC is produced on a pipe.
    bool declareUnknownLength = false;
};

void appendBigU24(std::vector<std::byte>& out, std::uint32_t value) {
    for (int shift = 16; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
    }
}

/// Encode planar integer samples as a native FLAC stream.
std::vector<std::byte> encodeFlac(const std::vector<std::vector<std::int32_t>>& channels,
                                  const FlacSpec& spec) {
    REQUIRE(!channels.empty());
    const auto channelCount = static_cast<int>(channels.size());
    const auto frameCount = static_cast<SampleCount>(channels.front().size());

    std::vector<std::byte> stream{std::byte{'f'}, std::byte{'L'}, std::byte{'a'}, std::byte{'C'}};

    // STREAMINFO, which the format requires to come first and to be 34 bytes.
    const bool hasPadding = spec.paddingBytes > 0;
    stream.push_back(static_cast<std::byte>(hasPadding ? 0x00u : 0x80u)); // type 0, last?
    appendBigU24(stream, 34);
    {
        BitWriter info;
        const SampleCount lastBlock = frameCount % spec.blockSize;
        const SampleCount smallestBlock =
            (lastBlock == 0 || frameCount <= spec.blockSize) ? spec.blockSize : lastBlock;
        info.write(static_cast<std::uint64_t>(smallestBlock), 16);
        info.write(static_cast<std::uint64_t>(spec.blockSize), 16);
        info.write(0, 24); // minimum frame size: unknown
        info.write(0, 24); // maximum frame size: unknown
        info.write(spec.sampleRate, 20);
        info.write(static_cast<std::uint64_t>(channelCount - 1), 3);
        info.write(static_cast<std::uint64_t>(spec.bitsPerSample - 1), 5);
        info.write(spec.declareUnknownLength ? 0u : static_cast<std::uint64_t>(frameCount), 36);
        for (int i = 0; i < 16; ++i) {
            info.write(0, 8); // MD5 of the source audio: not computed
        }
        stream.insert(stream.end(), info.bytes().begin(), info.bytes().end());
    }

    // An optional PADDING block, so the metadata walk has more than one block
    // to step over.
    if (hasPadding) {
        stream.push_back(std::byte{0x81}); // type 1, last block
        appendBigU24(stream, spec.paddingBytes);
        stream.insert(stream.end(), spec.paddingBytes, std::byte{0});
    }

    std::uint64_t frameNumber = 0;
    for (SampleCount start = 0; start < frameCount; start += spec.blockSize) {
        const SampleCount blockFrames = std::min(spec.blockSize, frameCount - start);

        BitWriter header;
        header.write(0x3FFEu, 14);                                     // sync
        header.write(0, 1);                                            // reserved
        header.write(0, 1);                                            // fixed block size
        header.write(0x7u, 4);                                         // 16-bit size follows
        header.write(0, 4);                                            // rate: from STREAMINFO
        header.write(static_cast<std::uint64_t>(channelCount - 1), 4); // independent channels
        header.write(0, 3);                                            // depth: from STREAMINFO
        header.write(0, 1);                                            // reserved
        writeFrameNumber(header, frameNumber);
        header.write(static_cast<std::uint64_t>(blockFrames - 1), 16);
        header.write(crc8(header.bytes()), 8);

        BitWriter frame;
        for (std::byte value : header.bytes()) {
            frame.write(std::to_integer<std::uint8_t>(value), 8);
        }
        for (int channel = 0; channel < channelCount; ++channel) {
            frame.write(0, 1);     // mandatory zero
            frame.write(0x01u, 6); // VERBATIM
            frame.write(0, 1);     // no wasted bits
            for (SampleCount i = 0; i < blockFrames; ++i) {
                frame.writeSigned(channels[static_cast<std::size_t>(channel)]
                                          [static_cast<std::size_t>(start + i)],
                                  spec.bitsPerSample);
            }
        }
        frame.padToByte();

        std::vector<std::byte> frameBytes = frame.bytes();
        const std::uint16_t checksum = crc16(frameBytes);
        frameBytes.push_back(static_cast<std::byte>((checksum >> 8) & 0xFFu));
        frameBytes.push_back(static_cast<std::byte>(checksum & 0xFFu));

        stream.insert(stream.end(), frameBytes.begin(), frameBytes.end());
        ++frameNumber;
    }

    return stream;
}

/// A signal with a different shape in every channel, so a de-interleave bug
/// cannot pass. Deterministic, and it uses the full range of the bit depth.
std::vector<std::vector<std::int32_t>> makeSignal(int channelCount, SampleCount frames,
                                                  int bitsPerSample) {
    const auto peak = static_cast<double>((std::int64_t{1} << (bitsPerSample - 1)) - 1);
    std::vector<std::vector<std::int32_t>> channels(static_cast<std::size_t>(channelCount));
    for (int channel = 0; channel < channelCount; ++channel) {
        auto& samples = channels[static_cast<std::size_t>(channel)];
        samples.resize(static_cast<std::size_t>(frames));
        for (SampleCount i = 0; i < frames; ++i) {
            const double phase = 0.013 * static_cast<double>(i) * static_cast<double>(channel + 1);
            const double value = std::sin(phase) * 0.9 * peak;
            samples[static_cast<std::size_t>(i)] = static_cast<std::int32_t>(value);
        }
    }
    return channels;
}

/// What a sample of this depth must decode to. FLAC is lossless and dr_flac
/// scales by a power of two, so this is exact rather than approximate -- any
/// tolerance here would hide a real defect.
float expectedSample(std::int32_t value, int bitsPerSample) {
    return static_cast<float>(value) / static_cast<float>(std::int64_t{1} << (bitsPerSample - 1));
}

/// A real FLAC, encoded by libFLAC rather than by the generator above.
///
/// The generator emits VERBATIM subframes, which is a legal FLAC and the right
/// thing for building hostile variants -- but it means every test above it
/// exercises the container and none of them exercise the codec. A real encoder
/// writes LPC and fixed predictors, Rice-coded residuals and left/side stereo
/// decorrelation, which is the code that actually turns bits back into audio.
/// So one real file is checked in, as base64 for the same reasons the MP3
/// fixtures are.
///
/// 1024 frames of 44.1 kHz 16-bit stereo. Regenerate with libFLAC via its
/// Python binding (pip install pyflac numpy), writing the signal that
/// realFixtureSignal() below rebuilds:
///
///     import wave, numpy, pyflac
///     # ... write the samples to a wav, then:
///     pyflac.FileEncoder(input_file=wav, output_file=out, compression_level=8,
///                        verify=True, blocksize=256).process()
constexpr std::string_view kRealFlac =
    "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";

std::vector<std::byte> fromBase64(std::string_view text) {
    const auto valueOf = [](char symbol) -> int {
        if (symbol >= 'A' && symbol <= 'Z') {
            return symbol - 'A';
        }
        if (symbol >= 'a' && symbol <= 'z') {
            return symbol - 'a' + 26;
        }
        if (symbol >= '0' && symbol <= '9') {
            return symbol - '0' + 52;
        }
        if (symbol == '+') {
            return 62;
        }
        if (symbol == '/') {
            return 63;
        }
        return -1;
    };

    std::vector<std::byte> bytes;
    bytes.reserve(text.size() * 3 / 4);
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (char symbol : text) {
        const int value = valueOf(symbol);
        if (value < 0) {
            continue; // padding
        }
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<std::byte>((accumulator >> bits) & 0xFFu));
        }
    }
    return bytes;
}

/// The signal kRealFlac was encoded from, rebuilt exactly.
///
/// Integer arithmetic throughout, so that this and the Python that produced the
/// file cannot disagree in the last bit the way two libraries' sin() can. The
/// first 800 frames are a sawtooth, which a predictor gets almost exactly
/// right; the rest is a linear congruential sequence, which no predictor can
/// get right at all. Between them they drive both ends of the residual coder,
/// and the right channel is derived from the left so that the encoder has a
/// reason to use stereo decorrelation.
std::vector<std::vector<std::int32_t>> realFixtureSignal() {
    constexpr SampleCount kFrames = 1024;
    constexpr SampleCount kSawtoothFrames = 800;

    std::vector<std::vector<std::int32_t>> channels(2);
    channels[0].resize(static_cast<std::size_t>(kFrames));
    channels[1].resize(static_cast<std::size_t>(kFrames));

    std::uint32_t state = 12345;
    for (SampleCount i = 0; i < kFrames; ++i) {
        std::int32_t left = 0;
        if (i < kSawtoothFrames) {
            left = static_cast<std::int32_t>((i * 37) % 2001) - 1000;
        } else {
            state = (state * 1103515245U + 12345U) & 0x7FFFFFFFU;
            left = static_cast<std::int32_t>(state % 601U) - 300;
        }
        channels[0][static_cast<std::size_t>(i)] = left;
        channels[1][static_cast<std::size_t>(i)] =
            (left >> 1) + static_cast<std::int32_t>((i * 11) % 101) - 50;
    }
    return channels;
}

/// Parse and, if that worked, decode too: a header can parse cleanly and still
/// point reads out of bounds.
///
/// Note that every caller here keeps its encoded bytes in a named local.
/// FlacReader::fromMemory does not copy -- that is what makes it usable on a
/// file too large to hold -- so handing it a temporary is a use-after-free, and
/// one that reads plausible-looking audio back rather than crashing.
void parseAndRead(std::span<const std::byte> bytes) {
    auto reader = FlacReader::fromMemory(bytes);
    if (!reader) {
        return;
    }
    AudioBuffer destination{ChannelLayout::stereo(), 256};
    (void)reader.value().read(0, destination.view());
    (void)reader.value().read(reader.value().info().frameCount / 2, destination.view());
    (void)reader.value().read(reader.value().info().frameCount - 1, destination.view());
    (void)reader.value().readAll();
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

    void write(std::span<const std::byte> bytes) const {
        std::ofstream stream{path_, std::ios::binary | std::ios::trunc};
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }

private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("A FLAC decodes to exactly the samples that were encoded", "[io][flac]") {
    // 2000 frames over a 512-sample block is three full frames and a partial
    // one, which is where an off-by-one in the last block would show.
    constexpr SampleCount kFrames = 2000;
    FlacSpec spec;
    spec.sampleRate = 48000;
    spec.bitsPerSample = 16;
    spec.blockSize = 512;

    const auto signal = makeSignal(2, kFrames, spec.bitsPerSample);
    const auto encoded = encodeFlac(signal, spec);

    auto reader = FlacReader::fromMemory(encoded);
    REQUIRE(reader.hasValue());
    CHECK(reader.value().info().frameCount == kFrames);
    CHECK(reader.value().info().channelCount() == 2);
    CHECK(reader.value().info().sampleRate == kSampleRate48000);
    CHECK(reader.value().info().format == SampleFormat::PcmInt16);

    auto decoded = reader.value().readAll();
    REQUIRE(decoded.hasValue());
    REQUIRE(decoded.value().frames() == kFrames);

    SampleCount mismatches = 0;
    for (int channel = 0; channel < 2; ++channel) {
        const float* got = decoded.value().channel(channel);
        for (SampleCount i = 0; i < kFrames; ++i) {
            const float wanted = expectedSample(
                signal[static_cast<std::size_t>(channel)][static_cast<std::size_t>(i)],
                spec.bitsPerSample);
            if (got[i] != wanted) {
                ++mismatches;
            }
        }
    }
    CHECK(mismatches == 0);
}

TEST_CASE("Every supported bit depth round-trips losslessly", "[io][flac]") {
    // FLAC allows any depth from 4 to 32 bits. AudioFileInfo has to report the
    // narrowest PCM width that holds the samples, because that is what an
    // export would have to write to keep them.
    struct Case {
        int bits;
        SampleFormat format;
    };

    for (const Case& test : {Case{8, SampleFormat::PcmInt16}, Case{16, SampleFormat::PcmInt16},
                             Case{20, SampleFormat::PcmInt24}, Case{24, SampleFormat::PcmInt24},
                             Case{32, SampleFormat::PcmInt32}}) {
        INFO("bit depth " << test.bits);
        FlacSpec spec;
        spec.bitsPerSample = test.bits;
        spec.blockSize = 256;

        const auto signal = makeSignal(1, 700, test.bits);
        const auto encoded = encodeFlac(signal, spec);
        auto reader = FlacReader::fromMemory(encoded);
        REQUIRE(reader.hasValue());
        CHECK(reader.value().info().format == test.format);
        CHECK(reader.value().info().frameCount == 700);

        auto decoded = reader.value().readAll();
        REQUIRE(decoded.hasValue());
        SampleCount mismatches = 0;
        for (SampleCount i = 0; i < 700; ++i) {
            if (decoded.value().channel(0)[i] !=
                expectedSample(signal[0][static_cast<std::size_t>(i)], test.bits)) {
                ++mismatches;
            }
        }
        CHECK(mismatches == 0);
    }
}

TEST_CASE("Reading a FLAC from an arbitrary frame matches reading from the start", "[io][flac]") {
    // Random access is the whole point of the AudioSource contract, and in a
    // compressed format it is the part most likely to be quietly wrong: a seek
    // lands on a frame boundary and the decoder has to discard the rest.
    FlacSpec spec;
    spec.blockSize = 128;
    const auto signal = makeSignal(2, 1500, spec.bitsPerSample);
    const auto encoded = encodeFlac(signal, spec);
    auto reader = FlacReader::fromMemory(encoded);
    REQUIRE(reader.hasValue());

    auto whole = reader.value().readAll();
    REQUIRE(whole.hasValue());

    // Backwards, forwards, across block boundaries and straddling them.
    for (SampleIndex start :
         {SampleIndex{0}, SampleIndex{1}, SampleIndex{127}, SampleIndex{128}, SampleIndex{129},
          SampleIndex{900}, SampleIndex{384}, SampleIndex{1499}}) {
        INFO("from frame " << start);
        AudioBuffer piece{ChannelLayout::stereo(), 200};
        auto got = reader.value().read(start, piece.view());
        REQUIRE(got.hasValue());
        const SampleCount expected = std::min<SampleCount>(200, 1500 - start);
        REQUIRE(got.value() == expected);

        SampleCount mismatches = 0;
        for (int channel = 0; channel < 2; ++channel) {
            for (SampleCount i = 0; i < expected; ++i) {
                if (piece.channel(channel)[i] != whole.value().channel(channel)[start + i]) {
                    ++mismatches;
                }
            }
        }
        CHECK(mismatches == 0);
    }
}

TEST_CASE("A stream that declares no length is measured by decoding", "[io][flac]") {
    // A total sample count of zero means unknown, not empty. Reporting an empty
    // file would be the worst answer: the user sees a valid recording as
    // nothing at all.
    FlacSpec spec;
    spec.blockSize = 64;
    spec.declareUnknownLength = true;

    const auto signal = makeSignal(1, 321, spec.bitsPerSample);
    const auto encoded = encodeFlac(signal, spec);
    auto reader = FlacReader::fromMemory(encoded);
    REQUIRE(reader.hasValue());
    CHECK(reader.value().info().frameCount == 321);

    // And the count has to be usable afterwards -- the counting pass leaves the
    // decoder at the end of the stream unless it is rewound.
    auto decoded = reader.value().readAll();
    REQUIRE(decoded.hasValue());
    CHECK(decoded.value().channel(0)[10] == expectedSample(signal[0][10], spec.bitsPerSample));
}

TEST_CASE("Metadata blocks before the audio are stepped over", "[io][flac]") {
    FlacSpec spec;
    spec.blockSize = 256;
    spec.paddingBytes = 4096;

    const auto signal = makeSignal(1, 600, spec.bitsPerSample);
    const auto encoded = encodeFlac(signal, spec);
    auto reader = FlacReader::fromMemory(encoded);
    REQUIRE(reader.hasValue());
    CHECK(reader.value().info().frameCount == 600);
}

TEST_CASE("A FLAC decode fills only the channels the destination has", "[io][flac]") {
    FlacSpec spec;
    spec.blockSize = 128;
    const auto signal = makeSignal(2, 400, spec.bitsPerSample);
    const auto encoded = encodeFlac(signal, spec);
    auto reader = FlacReader::fromMemory(encoded);
    REQUIRE(reader.hasValue());

    // A mono destination takes the first channel and drops the rest.
    AudioBuffer mono{ChannelLayout::mono(), 100};
    auto got = reader.value().read(0, mono.view());
    REQUIRE(got.hasValue());
    CHECK(got.value() == 100);
    CHECK(mono.channel(0)[5] == expectedSample(signal[0][5], spec.bitsPerSample));

    // A destination with more channels than the file leaves the extras alone.
    AudioBuffer wide{ChannelLayout::discrete(4), 100};
    for (int channel = 0; channel < 4; ++channel) {
        wide.channel(channel)[0] = -7.0f;
    }
    got = reader.value().read(0, wide.view());
    REQUIRE(got.hasValue());
    CHECK(got.value() == 100);
    CHECK(wide.channel(1)[0] == expectedSample(signal[1][0], spec.bitsPerSample));
    CHECK(wide.channel(2)[0] == -7.0f);
    CHECK(wide.channel(3)[0] == -7.0f);
}

TEST_CASE("Reads outside a FLAC are refused or short, never invented", "[io][flac]") {
    FlacSpec spec;
    spec.blockSize = 64;
    const auto encoded = encodeFlac(makeSignal(1, 200, 16), spec);
    auto reader = FlacReader::fromMemory(encoded);
    REQUIRE(reader.hasValue());

    AudioBuffer destination{ChannelLayout::mono(), 64};

    auto negative = reader.value().read(-1, destination.view());
    REQUIRE_FALSE(negative.hasValue());
    CHECK(negative.error().code() == ErrorCode::OutOfRange);

    auto past = reader.value().read(200, destination.view());
    REQUIRE(past.hasValue());
    CHECK(past.value() == 0);

    auto straddling = reader.value().read(180, destination.view());
    REQUIRE(straddling.hasValue());
    CHECK(straddling.value() == 20);
}

TEST_CASE("Input that is not a FLAC is rejected", "[io][flac][robustness]") {
    const std::vector<std::byte> empty;
    auto none = FlacReader::fromMemory(empty);
    REQUIRE_FALSE(none.hasValue());
    CHECK(none.error().code() == ErrorCode::UnsupportedFormat);

    const std::vector<std::byte> junk(512, std::byte{0x5A});
    CHECK_FALSE(FlacReader::fromMemory(junk).hasValue());

    // The magic alone is not a stream: there has to be a STREAMINFO behind it.
    std::vector<std::byte> magicOnly{std::byte{'f'}, std::byte{'L'}, std::byte{'a'},
                                     std::byte{'C'}};
    magicOnly.resize(64, std::byte{0xEE});
    auto stub = FlacReader::fromMemory(magicOnly);
    REQUIRE_FALSE(stub.hasValue());
    CHECK(stub.error().code() == ErrorCode::CorruptData);
}

TEST_CASE("A FLAC truncated at every offset never crashes or hangs", "[io][flac][robustness]") {
    // The cheapest useful fuzz, and the one that catches a decoder trusting a
    // length it has not checked. Worth little without ASan; worth a lot with it.
    FlacSpec spec;
    spec.blockSize = 64;
    const auto complete = encodeFlac(makeSignal(1, 300, 16), spec);

    for (std::size_t length = 0; length < complete.size(); ++length) {
        const std::vector<std::byte> truncated{
            complete.begin(), complete.begin() + static_cast<std::ptrdiff_t>(length)};
        INFO("truncated to " << length << " of " << complete.size() << " bytes");
        parseAndRead(truncated);
    }
    SUCCEED("every truncation terminated");
}

TEST_CASE("Corrupted FLAC bytes are survived", "[io][flac][robustness]") {
    // Every byte of a FLAC frame is attacker-controlled, including the ones the
    // decoder uses to decide how many samples to write. A corrupt file must
    // come back as an error or a short read, never as a crash.
    FlacSpec spec;
    spec.blockSize = 128;
    const auto complete = encodeFlac(makeSignal(2, 400, 24), spec);

    std::mt19937 generator{20240918u};
    std::uniform_int_distribution<std::size_t> offset{0, complete.size() - 1};
    std::uniform_int_distribution<int> value{0, 255};

    for (int attempt = 0; attempt < 400; ++attempt) {
        auto mutated = complete;
        for (int flip = 0; flip < 4; ++flip) {
            mutated[offset(generator)] = static_cast<std::byte>(value(generator));
        }
        INFO("attempt " << attempt);
        parseAndRead(mutated);
    }
    SUCCEED("every mutation terminated");
}

TEST_CASE("A STREAMINFO claiming an absurd length cannot be trusted", "[io][flac][robustness]") {
    FlacSpec spec;
    spec.blockSize = 64;
    auto bytes = encodeFlac(makeSignal(1, 200, 16), spec);

    // The 36-bit total-sample field ends at the last bit of STREAMINFO byte 17,
    // which is offset 8 + 13 in the stream. Set every bit of it: the file now
    // claims 2^36 frames it plainly does not hold.
    for (std::size_t i = 21; i < 26; ++i) {
        bytes[i] = std::byte{0xFF};
    }

    auto reader = FlacReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());

    // The claim is thrown away and the stream measured instead. This matters
    // beyond tidiness: readAll() allocates a buffer of frameCount frames, so a
    // believed 2^36 would be a request for a terabyte, and the honest answer to
    // that is not an exception thrown out of a module that reports by return.
    CHECK(reader.value().info().frameCount == 200);

    AudioBuffer destination{ChannelLayout::mono(), 512};
    auto got = reader.value().read(0, destination.view());
    REQUIRE(got.hasValue());
    CHECK(got.value() == 200);

    auto whole = reader.value().readAll();
    REQUIRE(whole.hasValue());
    CHECK(whole.value().frames() == 200);
}

TEST_CASE("openAudioFile recognises a FLAC by content", "[io][flac][format]") {
    // Named .wav on purpose: detection reads the bytes, never the extension.
    const ScopedFile file{"sa-flac-by-content.wav"};
    FlacSpec spec;
    spec.sampleRate = 44100;
    spec.blockSize = 256;
    const auto signal = makeSignal(2, 900, spec.bitsPerSample);
    file.write(encodeFlac(signal, spec));

    auto detected = detectFormat(file.path());
    REQUIRE(detected.hasValue());
    CHECK(detected.value() == ContainerFormat::Flac);
    CHECK(toString(ContainerFormat::Flac) == "FLAC");

    auto opened = openAudioFile(file.path());
    REQUIRE(opened.hasValue());
    const AudioSource& audio = *opened.value();
    CHECK(audio.info().frameCount == 900);
    CHECK(audio.info().sampleRate == kSampleRate44100);

    AudioBuffer decoded{ChannelLayout::stereo(), 900};
    auto got = audio.read(0, decoded.view());
    REQUIRE(got.hasValue());
    CHECK(got.value() == 900);
    CHECK(decoded.channel(1)[42] == expectedSample(signal[1][42], spec.bitsPerSample));
}

TEST_CASE("A FLAC drives the streaming peak pyramid", "[io][flac][peaks]") {
    // This is how a waveform actually gets drawn: the pyramid reads the source
    // in blocks and never holds more than one, which over a compressed format
    // is a long run of adjacent reads. Agreeing exactly with a build over the
    // decoded buffer is what says the reader behaves like a WAV in the place it
    // is really used -- and the block size here is deliberately not a power of
    // two, so no FLAC frame boundary lines up with a pyramid bin.
    FlacSpec spec;
    spec.blockSize = 199;
    const auto signal = makeSignal(2, 20000, spec.bitsPerSample);
    const auto encoded = encodeFlac(signal, spec);

    auto reader = FlacReader::fromMemory(encoded);
    REQUIRE(reader.hasValue());
    auto whole = reader.value().readAll();
    REQUIRE(whole.hasValue());

    auto inMemory = PeakPyramid::build(whole.value().constView());
    auto streamed = PeakPyramid::buildStreaming(reader.value());
    REQUIRE(inMemory.hasValue());
    REQUIRE(streamed.hasValue());

    REQUIRE(streamed.value().levelCount() == inMemory.value().levelCount());
    REQUIRE(streamed.value().sourceFrames() == inMemory.value().sourceFrames());
    SampleCount mismatches = 0;
    for (int level = 0; level < inMemory.value().levelCount(); ++level) {
        REQUIRE(streamed.value().frameCountAt(level) == inMemory.value().frameCountAt(level));
        for (int channel = 0; channel < inMemory.value().channelCount(); ++channel) {
            for (SampleCount f = 0; f < inMemory.value().frameCountAt(level); ++f) {
                const PeakFrame& expected = inMemory.value().frameAt(level, channel, f);
                const PeakFrame& actual = streamed.value().frameAt(level, channel, f);
                if (actual.minimum != expected.minimum || actual.maximum != expected.maximum) {
                    ++mismatches;
                }
            }
        }
    }
    CHECK(mismatches == 0);
}

TEST_CASE("A FLAC from a real encoder decodes to the samples it was made from", "[io][flac]") {
    // Everything above this builds its own fixtures, which proves the container
    // handling and nothing about the codec: a VERBATIM subframe is the samples
    // written out plainly. This file came from libFLAC, so its frames carry
    // predictors, Rice-coded residuals and left/side stereo -- the paths a file
    // a user actually opens goes through. FLAC is lossless, so the comparison
    // is exact, and a single wrong residual would show.
    const auto bytes = fromBase64(kRealFlac);
    auto reader = FlacReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());

    const AudioFileInfo& info = reader.value().info();
    CHECK(info.sampleRate == kSampleRate44100);
    CHECK(info.channelCount() == 2);
    CHECK(info.frameCount == 1024);
    CHECK(info.format == SampleFormat::PcmInt16);

    const auto expected = realFixtureSignal();
    auto decoded = reader.value().readAll();
    REQUIRE(decoded.hasValue());
    REQUIRE(decoded.value().frames() == 1024);

    SampleCount mismatches = 0;
    for (int channel = 0; channel < 2; ++channel) {
        for (SampleCount i = 0; i < 1024; ++i) {
            const float wanted = expectedSample(
                expected[static_cast<std::size_t>(channel)][static_cast<std::size_t>(i)], 16);
            if (decoded.value().channel(channel)[i] != wanted) {
                ++mismatches;
            }
        }
    }
    CHECK(mismatches == 0);

    // And the same again from part way in, which is where a seek has to land on
    // a frame boundary and discard the samples before the one asked for.
    AudioBuffer piece{ChannelLayout::stereo(), 300};
    auto got = reader.value().read(700, piece.view());
    REQUIRE(got.hasValue());
    REQUIRE(got.value() == 300);
    SampleCount seekedMismatches = 0;
    for (int channel = 0; channel < 2; ++channel) {
        for (SampleCount i = 0; i < 300; ++i) {
            const float wanted = expectedSample(
                expected[static_cast<std::size_t>(channel)][static_cast<std::size_t>(700 + i)], 16);
            if (piece.channel(channel)[i] != wanted) {
                ++seekedMismatches;
            }
        }
    }
    CHECK(seekedMismatches == 0);
}
