#pragma once

#include <sa/core/ChannelLayout.h>
#include <sa/core/Types.h>

#include <cstddef>
#include <type_traits>
#include <vector>

namespace sa {

/// Non-owning view over planar (non-interleaved) audio.
///
/// This is the type DSP operates on. It never allocates, never owns, and
/// sub-ranging is pointer arithmetic -- so a processor can work on part of a
/// buffer without copying. Interleaving happens only at device and codec
/// boundaries.
///
/// The channel pointer array is always stored as `float* const*` regardless of
/// whether the view is mutable; constness is applied when handing out a channel
/// pointer. That keeps mutable-to-const conversion a plain copy rather than a
/// cast between incompatible pointer-to-pointer types.
///
/// Use the AudioBufferView / ConstAudioBufferView aliases below.
template <typename Sample>
class BasicAudioBufferView {
public:
    static_assert(std::is_same_v<std::remove_const_t<Sample>, float>,
                  "Sound Analyser processes float32 internally");

    BasicAudioBufferView() = default;

    BasicAudioBufferView(float* const* channels, int channelCount, SampleCount frames) noexcept
        : channels_(channels), channelCount_(channelCount), frames_(frames) {}

    /// Mutable view converts implicitly to a const view.
    template <typename Other,
              typename = std::enable_if_t<std::is_const_v<Sample> && !std::is_const_v<Other>>>
    BasicAudioBufferView(const BasicAudioBufferView<Other>& other) noexcept // NOLINT
        : channels_(other.channels_), channelCount_(other.channelCount_), frames_(other.frames_),
          offset_(other.offset_) {}

    [[nodiscard]] int channelCount() const noexcept { return channelCount_; }

    [[nodiscard]] SampleCount frames() const noexcept { return frames_; }

    [[nodiscard]] SampleCount offset() const noexcept { return offset_; }

    [[nodiscard]] bool isEmpty() const noexcept { return channelCount_ == 0 || frames_ == 0; }

    /// Pointer to the first sample of `channel`. Undefined for an out-of-range
    /// channel: audio-thread callers should not pay for a check they can hoist,
    /// so validate at the boundary instead.
    [[nodiscard]] Sample* channel(int index) const noexcept {
        return channels_[static_cast<std::size_t>(index)] + offset_;
    }

    /// A view over `count` frames starting `frameOffset` into this view.
    /// Clamped to the available range, so the result is always valid.
    [[nodiscard]] BasicAudioBufferView subRange(SampleCount frameOffset,
                                                SampleCount count) const noexcept {
        const SampleCount start =
            frameOffset < 0 ? 0 : (frameOffset > frames_ ? frames_ : frameOffset);
        const SampleCount available = frames_ - start;
        const SampleCount taken = count < 0 ? 0 : (count > available ? available : count);

        BasicAudioBufferView result;
        result.channels_ = channels_;
        result.channelCount_ = channelCount_;
        result.offset_ = offset_ + start;
        result.frames_ = taken;
        return result;
    }

private:
    template <typename>
    friend class BasicAudioBufferView;

    float* const* channels_ = nullptr;
    int channelCount_ = 0;
    SampleCount frames_ = 0;
    SampleCount offset_ = 0;
};

using AudioBufferView = BasicAudioBufferView<float>;
using ConstAudioBufferView = BasicAudioBufferView<const float>;

/// Owning planar audio buffer with aligned per-channel storage.
///
/// Each channel is a separate aligned allocation, so every channel is
/// independently SIMD-aligned regardless of frame count, and a single channel
/// can be replaced without touching the others.
///
/// Move-only: copying audio should be explicit and visible at the call site.
class AudioBuffer {
public:
    AudioBuffer() = default;
    AudioBuffer(ChannelLayout layout, SampleCount frames);
    ~AudioBuffer();

    AudioBuffer(const AudioBuffer&) = delete;
    AudioBuffer& operator=(const AudioBuffer&) = delete;
    AudioBuffer(AudioBuffer&& other) noexcept;
    AudioBuffer& operator=(AudioBuffer&& other) noexcept;

    /// Reallocate to `layout` x `frames`. Existing contents are not preserved,
    /// and the new storage is zeroed. Allocates -- never call on the audio thread.
    void resize(ChannelLayout layout, SampleCount frames);

    /// Zero every sample. Allocation-free and audio-thread safe.
    void clear() noexcept;

    [[nodiscard]] const ChannelLayout& layout() const noexcept { return layout_; }

    [[nodiscard]] int channelCount() const noexcept { return layout_.count(); }

    [[nodiscard]] SampleCount frames() const noexcept { return frames_; }

    [[nodiscard]] bool isEmpty() const noexcept { return channelCount() == 0 || frames_ == 0; }

    /// Pointer to the first sample of `channel`. Valid for any index below
    /// channelCount(), including on a buffer with no frames -- where it is
    /// null, and where the only legal thing to do with it is to add zero.
    /// That case is deliberate: it makes
    /// `std::reverse(b.channel(c), b.channel(c) + b.frames())` correct on an
    /// empty buffer instead of a crash.
    ///
    /// Out of range is still undefined. Audio-thread callers should not pay
    /// for a check they can hoist, so validate at the boundary instead.
    [[nodiscard]] float* channel(int index) noexcept {
        return channels_[static_cast<std::size_t>(index)];
    }

    [[nodiscard]] const float* channel(int index) const noexcept {
        return channels_[static_cast<std::size_t>(index)];
    }

    [[nodiscard]] AudioBufferView view() noexcept {
        return AudioBufferView{channels_.data(), channelCount(), frames_};
    }

    [[nodiscard]] ConstAudioBufferView view() const noexcept { return constView(); }

    [[nodiscard]] ConstAudioBufferView constView() const noexcept {
        return ConstAudioBufferView{channels_.data(), channelCount(), frames_};
    }

private:
    void release() noexcept;

    ChannelLayout layout_;
    SampleCount frames_ = 0;
    std::vector<float*> channels_;
};

} // namespace sa
