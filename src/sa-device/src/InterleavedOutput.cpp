#include <sa/core/ChannelLayout.h>
#include <sa/core/RealtimeGuard.h>
#include <sa/device/InterleavedOutput.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace sa::device {

namespace {

/// Clamps to the representable range and turns NaN into silence.
///
/// The NaN case is tested first on purpose: NaN compares false against both
/// bounds, so a clamp written as two comparisons would pass it straight through
/// to a float-to-integer conversion, which is undefined. On x86 that conversion
/// yields INT_MIN, so a single NaN sample -- one denormal mishandled somewhere
/// upstream -- becomes a full-scale click.
[[nodiscard]] float sanitise(float value) noexcept {
    if (std::isnan(value)) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    if (value < -1.0f) {
        return -1.0f;
    }
    return value;
}

/// Scales by 32767 rather than 32768 so that +1.0 lands exactly on the positive
/// full-scale code instead of wrapping. Truncation rather than rounding costs at
/// most one LSB, which is far below the dither floor of any endpoint actually
/// running at 16 bits, and avoids depending on the current rounding mode inside
/// the render loop.
[[nodiscard]] std::int16_t toInt16(float value) noexcept {
    return static_cast<std::int16_t>(sanitise(value) * 32767.0f);
}

/// Done in double: 2147483647 is not representable as a float, so scaling in
/// single precision would round the multiplier up to 2^31 and overflow on any
/// sample at full scale.
[[nodiscard]] std::int32_t toInt32(float value) noexcept {
    return static_cast<std::int32_t>(static_cast<double>(sanitise(value)) * 2147483647.0);
}

/// One destination channel, strided over the interleaved block. Templated on
/// the conversion so the three formats share the loop shape without a per-sample
/// branch on the format.
template <typename Sample, typename Convert>
void writeChannel(Sample* destination, SampleCount stride, const float* source, SampleCount frames,
                  Convert convert) noexcept {
    for (SampleCount i = 0; i < frames; ++i) {
        destination[i * stride] = convert(source[i]);
    }
}

template <typename Sample>
void silenceChannel(Sample* destination, SampleCount stride, SampleCount frames) noexcept {
    for (SampleCount i = 0; i < frames; ++i) {
        destination[i * stride] = Sample{0};
    }
}

template <typename Sample, typename Convert>
void interleaveAs(ConstAudioBufferView source, void* destination, int destinationChannels,
                  Convert convert) noexcept {
    auto* out = static_cast<Sample*>(destination);
    const SampleCount frames = source.frames();
    const SampleCount stride = destinationChannels;
    const int shared =
        source.channelCount() < destinationChannels ? source.channelCount() : destinationChannels;

    for (int channel = 0; channel < destinationChannels; ++channel) {
        Sample* target = out + channel;
        if (channel < shared) {
            writeChannel(target, stride, source.channel(channel), frames, convert);
        } else {
            silenceChannel(target, stride, frames);
        }
    }
}

} // namespace

void interleave(ConstAudioBufferView source, void* destination, InterleavedFormat format,
                int destinationChannels) noexcept {
    if (destination == nullptr || destinationChannels <= 0 || source.frames() <= 0) {
        return;
    }

    switch (format) {
    case InterleavedFormat::Float32:
        interleaveAs<float>(source, destination, destinationChannels,
                            [](float value) noexcept { return value; });
        return;
    case InterleavedFormat::Int16:
        interleaveAs<std::int16_t>(source, destination, destinationChannels, toInt16);
        return;
    case InterleavedFormat::Int32:
        interleaveAs<std::int32_t>(source, destination, destinationChannels, toInt32);
        return;
    }
}

void OutputBlocker::prepare(int channels, int blockFrames) {
    channels_ = channels > 0 ? channels : 0;
    blockFrames_ = blockFrames > 0 ? blockFrames : 0;

    const auto frames = static_cast<SampleCount>(blockFrames_);
    input_.resize(ChannelLayout::discrete(0), frames);
    output_.resize(ChannelLayout::discrete(channels_), frames);

    // The block is "already spent", so the first render() asks the callback for
    // audio instead of publishing a block nobody wrote.
    consumed_ = frames;
    blocksRendered_.store(0, std::memory_order_release);
}

std::size_t OutputBlocker::blocksRendered() const noexcept {
    return blocksRendered_.load(std::memory_order_acquire);
}

void OutputBlocker::render(const AudioCallback& callback, void* destination, SampleCount frames,
                           InterleavedFormat format, int destinationChannels) noexcept {
    if (destination == nullptr || frames <= 0 || destinationChannels <= 0) {
        return;
    }

    if (blockFrames_ <= 0 || channels_ <= 0 || !callback) {
        // Nothing to render with, which happens between a stream being rebuilt
        // and its callback being handed over. Silence beats leaving the
        // driver's buffer alone: what is in there is the last block, and
        // playing it again is a loop of noise rather than a gap.
        std::memset(destination, 0, interleavedByteCount(frames, destinationChannels, format));
        return;
    }

    const SampleCount block = blockFrames_;
    const auto frameBytes =
        static_cast<std::ptrdiff_t>(static_cast<std::size_t>(destinationChannels) *
                                    static_cast<std::size_t>(bytesPerSample(format)));
    auto* bytes = static_cast<std::byte*>(destination);

    SampleCount done = 0;
    while (done < frames) {
        if (consumed_ >= block) {
            {
                // Marks the thread for the duration of the callback only. The
                // driver calls around it allocate inside the OS, and counting
                // those would drown the signal this guard exists to give.
                const rt::ScopedAudioThread guard;
                callback(input_.constView(), output_.view());
            }
            consumed_ = 0;
            blocksRendered_.fetch_add(1, std::memory_order_release);
        }

        const SampleCount remaining = frames - done;
        const SampleCount available = block - consumed_;
        const SampleCount chunk = remaining < available ? remaining : available;

        interleave(output_.constView().subRange(consumed_, chunk), bytes + done * frameBytes,
                   format, destinationChannels);

        consumed_ += chunk;
        done += chunk;
    }
}

} // namespace sa::device
