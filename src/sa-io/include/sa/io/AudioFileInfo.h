#pragma once

#include <sa/core/ChannelLayout.h>
#include <sa/core/Types.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sa::io {

/// How samples are encoded in the file. Everything is converted to float32 on
/// read; this records what was there so an export can round-trip it.
enum class SampleFormat {
    Unknown,
    PcmUInt8,
    PcmInt16,
    PcmInt24,
    PcmInt32,
    Float32,
    Float64,
};

[[nodiscard]] std::string_view toString(SampleFormat format) noexcept;

/// Bytes each sample occupies in the file, or 0 if unknown.
[[nodiscard]] int bytesPerSample(SampleFormat format) noexcept;

/// A marker or region carried by the file.
struct FileMarker {
    SampleIndex position = 0;
    SampleCount length = 0; ///< 0 for a point marker
    std::string label;
};

/// Broadcast Wave metadata (the `bext` chunk).
///
/// Post-production workflows depend on this: `timeReference` is the timecode
/// that lines a recording up against picture, and losing it on a round-trip
/// makes a file useless to the next person in the chain.
struct BroadcastInfo {
    std::string description;
    std::string originator;
    std::string originatorReference;
    std::string originationDate;     ///< yyyy-mm-dd
    std::string originationTime;     ///< hh:mm:ss
    std::uint64_t timeReference = 0; ///< samples since midnight
    bool present = false;
};

/// Everything a reader recovered about a file besides its audio.
struct AudioFileMetadata {
    std::string title;
    std::string artist;
    std::string comment;
    std::string software;
    std::string date;
    BroadcastInfo broadcast;
    std::vector<FileMarker> markers;
};

/// Shape of the audio in a file.
struct AudioFileInfo {
    SampleRate sampleRate;
    ChannelLayout layout;
    SampleCount frameCount = 0;
    SampleFormat format = SampleFormat::Unknown;

    [[nodiscard]] int channelCount() const noexcept { return layout.count(); }

    [[nodiscard]] double durationSeconds() const noexcept {
        return samplesToSeconds(frameCount, sampleRate);
    }
};

} // namespace sa::io
