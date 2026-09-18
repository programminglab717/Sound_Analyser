#include <sa/io/AiffReader.h>
#include <sa/io/AudioFile.h>
#include <sa/io/WavReader.h>

#include <array>
#include <fstream>

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

} // namespace

std::string_view toString(ContainerFormat format) noexcept {
    switch (format) {
    case ContainerFormat::Unknown:
        return "unknown";
    case ContainerFormat::Wave:
        return "WAVE";
    case ContainerFormat::Aiff:
        return "AIFF";
    }
    return "unknown";
}

ContainerFormat detectFormat(std::span<const std::byte> leadingBytes) noexcept {
    // Both containers are IFF-derived: a four-character type at offset 0 and a
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
    return ContainerFormat::Unknown;
}

Result<ContainerFormat> detectFormat(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        return Error{ErrorCode::IoFailure, "cannot open " + path.string()};
    }
    std::array<std::byte, 12> header{};
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
    case ContainerFormat::Unknown:
        break;
    }
    return Error{ErrorCode::UnsupportedFormat,
                 path.string() + ": not a container this build can decode"};
}

} // namespace sa::io
