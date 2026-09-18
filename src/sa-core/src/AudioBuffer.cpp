#include <sa/core/AudioBuffer.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace sa {

namespace {

/// Bytes needed for `frames` samples, rounded up so each channel allocation is
/// a whole number of alignment units. Over-allocating the tail lets SIMD kernels
/// read and write a full final vector without a scalar epilogue.
std::size_t paddedByteSize(SampleCount frames) noexcept {
    const auto raw = static_cast<std::size_t>(frames) * sizeof(float);
    const auto unit = kSampleAlignment;
    return ((raw + unit - 1) / unit) * unit;
}

} // namespace

AudioBuffer::AudioBuffer(ChannelLayout layout, SampleCount frames) {
    resize(layout, frames);
}

AudioBuffer::~AudioBuffer() {
    release();
}

AudioBuffer::AudioBuffer(AudioBuffer&& other) noexcept
    : layout_(other.layout_), frames_(other.frames_), channels_(std::move(other.channels_)) {
    other.layout_ = ChannelLayout{};
    other.frames_ = 0;
    other.channels_.clear();
}

AudioBuffer& AudioBuffer::operator=(AudioBuffer&& other) noexcept {
    if (this != &other) {
        release();
        layout_ = other.layout_;
        frames_ = other.frames_;
        channels_ = std::move(other.channels_);
        other.layout_ = ChannelLayout{};
        other.frames_ = 0;
        other.channels_.clear();
    }
    return *this;
}

void AudioBuffer::release() noexcept {
    for (float* channel : channels_) {
        ::operator delete(channel, std::align_val_t{kSampleAlignment});
    }
    channels_.clear();
    frames_ = 0;
    layout_ = ChannelLayout{};
}

void AudioBuffer::resize(ChannelLayout layout, SampleCount frames) {
    release();

    const int channelCount = layout.count();
    if (channelCount <= 0 || frames <= 0) {
        layout_ = layout;
        frames_ = frames > 0 ? frames : 0;
        return;
    }

    const std::size_t bytes = paddedByteSize(frames);
    channels_.reserve(static_cast<std::size_t>(channelCount));

    // Allocate one channel at a time so a failure part-way through still
    // unwinds cleanly: release() frees exactly what was pushed.
    for (int i = 0; i < channelCount; ++i) {
        void* memory = ::operator new(bytes, std::align_val_t{kSampleAlignment});
        std::memset(memory, 0, bytes);
        channels_.push_back(static_cast<float*>(memory));
    }

    layout_ = layout;
    frames_ = frames;
}

void AudioBuffer::clear() noexcept {
    if (frames_ <= 0) {
        return;
    }
    const std::size_t bytes = paddedByteSize(frames_);
    for (float* channel : channels_) {
        std::memset(channel, 0, bytes);
    }
}

} // namespace sa
