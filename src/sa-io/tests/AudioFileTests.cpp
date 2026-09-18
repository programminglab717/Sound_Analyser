#include <sa/io/AudioFile.h>
#include <sa/io/WavWriter.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdio>
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
