#pragma once

#include <sa/core/Result.h>
#include <sa/io/AudioSource.h>

#include <cstddef>
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
    Flac, ///< native FLAC; Ogg-encapsulated FLAC is not decoded
    Mp3,  ///< MPEG-1/2/2.5 audio, layers I to III
};

[[nodiscard]] std::string_view toString(ContainerFormat format) noexcept;

/// How many leading bytes detectFormat wants to see.
///
/// Four would settle WAV, AIFF and FLAC, which all begin with a magic number.
/// MP3 does not: it has no header of its own, only a run of frames, and telling
/// a real frame from four bytes that happen to look like one takes finding the
/// next frame where the first one's length says it should be. Two frames at the
/// worst-case length -- 320 kbit/s at 32 kHz -- is under 3 KiB, and an ID3v2
/// tag ahead of them is common, so this is the window that makes the check
/// worth making.
inline constexpr std::size_t kFormatSniffBytes = 4096;

/// Identify a container from its leading bytes.
///
/// By content, never by file extension. Extensions are routinely wrong -- a
/// `.wav` holding an AIFF is a real thing that comes out of badly written
/// exporters -- and trusting one would hand the wrong parser a hostile file.
///
/// Fewer than kFormatSniffBytes may be passed, and short input is never
/// misidentified; more of the file simply means fewer ways to be fooled.
[[nodiscard]] ContainerFormat detectFormat(std::span<const std::byte> leadingBytes) noexcept;

[[nodiscard]] Result<ContainerFormat> detectFormat(const std::filesystem::path& path);

/// Open any supported audio file.
///
/// The dispatch point every caller should use: analysis and editing code takes
/// an AudioSource and never learns which container it came from. FLAC and MP3
/// were added here without a single caller changing, which is the whole point
/// of the abstraction.
[[nodiscard]] Result<std::shared_ptr<const AudioSource>>
openAudioFile(const std::filesystem::path& path);

} // namespace sa::io
