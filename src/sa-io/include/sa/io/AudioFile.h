#pragma once

#include <sa/core/Result.h>
#include <sa/io/AudioSource.h>

#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace sa::io {

/// Containers this build can decode.
enum class ContainerFormat {
    Unknown,
    Wave, ///< RIFF/WAVE and RF64
    Aiff, ///< AIFF and AIFF-C
};

[[nodiscard]] std::string_view toString(ContainerFormat format) noexcept;

/// Identify a container from its leading bytes.
///
/// By content, never by file extension. Extensions are routinely wrong -- a
/// `.wav` holding an AIFF is a real thing that comes out of badly written
/// exporters -- and trusting one would hand the wrong parser a hostile file.
[[nodiscard]] ContainerFormat detectFormat(std::span<const std::byte> leadingBytes) noexcept;

[[nodiscard]] Result<ContainerFormat> detectFormat(const std::filesystem::path& path);

/// Open any supported audio file.
///
/// The dispatch point every caller should use: analysis and editing code takes
/// an AudioSource and never learns which container it came from, so adding
/// FLAC later changes only this function.
[[nodiscard]] Result<std::shared_ptr<const AudioSource>>
openAudioFile(const std::filesystem::path& path);

} // namespace sa::io
