#pragma once

#include <sa/core/Types.h>

#include <array>
#include <initializer_list>
#include <string_view>

namespace sa {

/// Speaker position for a single channel.
enum class Speaker : std::uint8_t {
    Unknown = 0,
    Mono,
    Left,
    Right,
    Centre,
    Lfe,
    LeftSurround,
    RightSurround,
    LeftSurroundRear,
    RightSurroundRear,
    LeftHeight,
    RightHeight,
};

[[nodiscard]] std::string_view toString(Speaker speaker) noexcept;

/// Channel count plus speaker assignment.
///
/// Fixed-capacity and trivially copyable so it can be stored in audio-thread
/// structures without allocating. Discrete layouts (arbitrary channel counts
/// with no speaker meaning) are supported for multichannel field recordings
/// and measurement rigs.
class ChannelLayout {
public:
    ChannelLayout() = default;

    static ChannelLayout mono() noexcept;
    static ChannelLayout stereo() noexcept;
    static ChannelLayout fiveOne() noexcept;
    static ChannelLayout sevenOne() noexcept;

    /// A layout of `count` channels with no speaker assignment. Clamped to
    /// [0, kMaxChannels].
    static ChannelLayout discrete(int count) noexcept;

    /// A layout with exactly these speakers, in order. Truncated at
    /// kMaxChannels. Ignores the layout it is called on -- it is a builder,
    /// not a modifier.
    [[nodiscard]] ChannelLayout
    withSpeakers(std::initializer_list<Speaker> speakers) const noexcept;

    [[nodiscard]] int count() const noexcept { return count_; }

    [[nodiscard]] bool isEmpty() const noexcept { return count_ == 0; }

    /// Speaker at `index`, or Speaker::Unknown if out of range.
    ///
    /// The kMaxChannels bound is redundant with count_ at runtime -- count_ can
    /// never exceed it -- but the compiler cannot prove that, and at -O2 it
    /// flags the subscript on the branch it cannot rule out. Stating the bound
    /// makes the guarantee explicit rather than inferred, and it also holds if
    /// count_ is ever corrupted by a bad deserialisation.
    [[nodiscard]] Speaker at(int index) const noexcept {
        if (index < 0 || index >= count_ || index >= kMaxChannels) {
            return Speaker::Unknown;
        }
        return speakers_[static_cast<std::size_t>(index)];
    }

    /// Index of the first channel assigned to `speaker`, or -1.
    [[nodiscard]] int indexOf(Speaker speaker) const noexcept;

    /// Short human-readable name, e.g. "stereo", "5.1", "8ch".
    [[nodiscard]] std::string_view name() const noexcept;

    friend bool operator==(const ChannelLayout& a, const ChannelLayout& b) noexcept;

private:
    std::array<Speaker, kMaxChannels> speakers_{};
    int count_ = 0;
};

} // namespace sa
