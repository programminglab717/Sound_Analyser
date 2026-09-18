#include <sa/core/ChannelLayout.h>

#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <unordered_map>

namespace sa {

namespace {

ChannelLayout makeLayout(std::initializer_list<Speaker> speakers) noexcept {
    return ChannelLayout::discrete(0).withSpeakers(speakers);
}

} // namespace

std::string_view toString(Speaker speaker) noexcept {
    switch (speaker) {
    case Speaker::Unknown:
        return "unknown";
    case Speaker::Mono:
        return "M";
    case Speaker::Left:
        return "L";
    case Speaker::Right:
        return "R";
    case Speaker::Centre:
        return "C";
    case Speaker::Lfe:
        return "LFE";
    case Speaker::LeftSurround:
        return "Ls";
    case Speaker::RightSurround:
        return "Rs";
    case Speaker::LeftSurroundRear:
        return "Lrs";
    case Speaker::RightSurroundRear:
        return "Rrs";
    case Speaker::LeftHeight:
        return "Lh";
    case Speaker::RightHeight:
        return "Rh";
    }
    return "unknown";
}

ChannelLayout ChannelLayout::discrete(int count) noexcept {
    ChannelLayout layout;
    layout.count_ = std::clamp(count, 0, kMaxChannels);
    layout.speakers_.fill(Speaker::Unknown);
    return layout;
}

ChannelLayout ChannelLayout::withSpeakers(std::initializer_list<Speaker> speakers) const noexcept {
    ChannelLayout layout;
    layout.speakers_.fill(Speaker::Unknown);

    const auto n = std::min(speakers.size(), static_cast<std::size_t>(kMaxChannels));
    std::size_t index = 0;
    for (Speaker speaker : speakers) {
        if (index >= n) {
            break;
        }
        layout.speakers_[index++] = speaker;
    }
    layout.count_ = static_cast<int>(n);
    return layout;
}

ChannelLayout ChannelLayout::mono() noexcept {
    return makeLayout({Speaker::Mono});
}

ChannelLayout ChannelLayout::stereo() noexcept {
    return makeLayout({Speaker::Left, Speaker::Right});
}

ChannelLayout ChannelLayout::fiveOne() noexcept {
    return makeLayout({Speaker::Left, Speaker::Right, Speaker::Centre, Speaker::Lfe,
                       Speaker::LeftSurround, Speaker::RightSurround});
}

ChannelLayout ChannelLayout::sevenOne() noexcept {
    return makeLayout({Speaker::Left, Speaker::Right, Speaker::Centre, Speaker::Lfe,
                       Speaker::LeftSurround, Speaker::RightSurround, Speaker::LeftSurroundRear,
                       Speaker::RightSurroundRear});
}

int ChannelLayout::indexOf(Speaker speaker) const noexcept {
    for (int i = 0; i < count_; ++i) {
        if (speakers_[static_cast<std::size_t>(i)] == speaker) {
            return i;
        }
    }
    return -1;
}

std::string_view ChannelLayout::name() const noexcept {
    if (*this == mono()) {
        return "mono";
    }
    if (*this == stereo()) {
        return "stereo";
    }
    if (*this == fiveOne()) {
        return "5.1";
    }
    if (*this == sevenOne()) {
        return "7.1";
    }
    if (count_ == 0) {
        return "empty";
    }

    // Discrete layouts get a generated "<n>ch" name. Interned in a table so the
    // returned string_view stays valid for the caller's lifetime, which a
    // locally-built std::string could not guarantee.
    static std::mutex mutex;
    static std::unordered_map<int, std::string> names;

    const std::lock_guard<std::mutex> lock(mutex);
    auto it = names.find(count_);
    if (it == names.end()) {
        it = names.emplace(count_, std::to_string(count_) + "ch").first;
    }
    return it->second;
}

bool operator==(const ChannelLayout& a, const ChannelLayout& b) noexcept {
    if (a.count_ != b.count_) {
        return false;
    }
    for (int i = 0; i < a.count_; ++i) {
        const auto index = static_cast<std::size_t>(i);
        if (a.speakers_[index] != b.speakers_[index]) {
            return false;
        }
    }
    return true;
}

} // namespace sa
