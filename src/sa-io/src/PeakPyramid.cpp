#include <sa/io/PeakPyramid.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace sa::io {

namespace {

bool isPowerOfTwo(SampleCount value) noexcept {
    return value >= 2 && (value & (value - 1)) == 0;
}

/// Frames needed to cover `total` samples in bins of `binSize`, rounding up so
/// the tail is never dropped.
SampleCount binsToCover(SampleCount total, SampleCount binSize) noexcept {
    return (total + binSize - 1) / binSize;
}

} // namespace

Result<PeakPyramid> PeakPyramid::build(ConstAudioBufferView source, SampleCount baseBinSize) {
    if (!isPowerOfTwo(baseBinSize)) {
        return Error{ErrorCode::InvalidArgument, "baseBinSize must be a power of two >= 2"};
    }

    PeakPyramid pyramid;
    pyramid.channelCount_ = source.channelCount();
    pyramid.sourceFrames_ = source.frames();
    pyramid.baseBinSize_ = baseBinSize;

    if (source.isEmpty()) {
        return pyramid;
    }

    const auto channels = static_cast<std::size_t>(source.channelCount());

    // --- Level 0: scan the source once -------------------------------------
    Level base;
    base.binSize = baseBinSize;
    base.frameCount = binsToCover(source.frames(), baseBinSize);
    base.frames.resize(static_cast<std::size_t>(base.frameCount) * channels);

    for (int channel = 0; channel < source.channelCount(); ++channel) {
        const float* samples = source.channel(channel);
        const auto channelOffset =
            static_cast<std::size_t>(channel) * static_cast<std::size_t>(base.frameCount);

        for (SampleCount frame = 0; frame < base.frameCount; ++frame) {
            const SampleCount start = frame * baseBinSize;
            const SampleCount count = std::min(baseBinSize, source.frames() - start);

            float minimum = std::numeric_limits<float>::max();
            float maximum = std::numeric_limits<float>::lowest();
            double sumOfSquares = 0.0;

            for (SampleCount i = 0; i < count; ++i) {
                const float sample = samples[start + i];
                minimum = std::min(minimum, sample);
                maximum = std::max(maximum, sample);
                sumOfSquares += static_cast<double>(sample) * static_cast<double>(sample);
            }

            PeakFrame& out = base.frames[channelOffset + static_cast<std::size_t>(frame)];
            out.minimum = minimum;
            out.maximum = maximum;
            out.rms = static_cast<float>(std::sqrt(sumOfSquares / static_cast<double>(count)));
        }
    }
    pyramid.levels_.push_back(std::move(base));

    pyramid.buildUpperLevels();
    return pyramid;
}

void PeakPyramid::buildUpperLevels() {
    if (levels_.empty()) {
        return;
    }
    const auto channels = static_cast<std::size_t>(channelCount_);

    // --- Levels 1..n: fold pairs from the level below -----------------------
    //
    // Each parent takes the min of its children's minima and the max of their
    // maxima, so a transient present at level 0 is present at every level. RMS
    // combines through sum of squares weighted by sample count -- averaging the
    // children's RMS values would be wrong, and wrong in a way that quietly
    // understates loud passages.
    while (levels_.back().frameCount > kMinimumTopLevelFrames) {
        const Level& previous = levels_.back();
        const int previousIndex = static_cast<int>(levels_.size()) - 1;

        Level next;
        next.binSize = previous.binSize * 2;
        next.frameCount = binsToCover(previous.frameCount, 2);
        next.frames.resize(static_cast<std::size_t>(next.frameCount) * channels);

        for (int channel = 0; channel < channelCount_; ++channel) {
            const auto previousOffset =
                static_cast<std::size_t>(channel) * static_cast<std::size_t>(previous.frameCount);
            const auto nextOffset =
                static_cast<std::size_t>(channel) * static_cast<std::size_t>(next.frameCount);

            for (SampleCount frame = 0; frame < next.frameCount; ++frame) {
                const SampleCount firstChild = frame * 2;
                const SampleCount childCount =
                    std::min<SampleCount>(2, previous.frameCount - firstChild);

                float minimum = std::numeric_limits<float>::max();
                float maximum = std::numeric_limits<float>::lowest();
                double sumOfSquares = 0.0;
                double totalSamples = 0.0;

                for (SampleCount c = 0; c < childCount; ++c) {
                    const PeakFrame& child =
                        previous.frames[previousOffset + static_cast<std::size_t>(firstChild + c)];
                    const auto childSamples =
                        static_cast<double>(samplesInFrame(previousIndex, firstChild + c));

                    minimum = std::min(minimum, child.minimum);
                    maximum = std::max(maximum, child.maximum);
                    sumOfSquares += static_cast<double>(child.rms) *
                                    static_cast<double>(child.rms) * childSamples;
                    totalSamples += childSamples;
                }

                PeakFrame& out = next.frames[nextOffset + static_cast<std::size_t>(frame)];
                out.minimum = minimum;
                out.maximum = maximum;
                out.rms = totalSamples > 0.0
                              ? static_cast<float>(std::sqrt(sumOfSquares / totalSamples))
                              : 0.0f;
            }
        }
        levels_.push_back(std::move(next));
    }
}

Result<PeakPyramid> PeakPyramid::buildStreaming(const AudioSource& source, SampleCount baseBinSize,
                                                const JobMonitor& monitor) {
    if (!isPowerOfTwo(baseBinSize)) {
        return Error{ErrorCode::InvalidArgument, "baseBinSize must be a power of two >= 2"};
    }

    const AudioFileInfo& info = source.info();
    PeakPyramid pyramid;
    pyramid.channelCount_ = info.channelCount();
    pyramid.sourceFrames_ = info.frameCount;
    pyramid.baseBinSize_ = baseBinSize;

    if (info.frameCount <= 0 || info.channelCount() <= 0) {
        return pyramid;
    }

    const auto channels = static_cast<std::size_t>(info.channelCount());

    Level base;
    base.binSize = baseBinSize;
    base.frameCount = binsToCover(info.frameCount, baseBinSize);
    base.frames.resize(static_cast<std::size_t>(base.frameCount) * channels);

    // Read in whole multiples of the bin size so a bin never straddles two
    // reads -- that would require carrying partial accumulator state between
    // blocks, which is where a streaming summariser usually goes subtly wrong.
    constexpr SampleCount kTargetBlockFrames = 1 << 16;
    const SampleCount binsPerBlock = std::max<SampleCount>(1, kTargetBlockFrames / baseBinSize);
    const SampleCount blockFrames = binsPerBlock * baseBinSize;

    AudioBuffer block{info.layout, blockFrames};

    SampleIndex position = 0;
    SampleCount frameIndex = 0;
    while (position < info.frameCount) {
        if (monitor.shouldCancel()) {
            return Error{ErrorCode::Cancelled, "peak pyramid build cancelled"};
        }

        // Fill the whole block before summarising. A source is free to return
        // fewer frames than asked -- a decoder or network-backed read routinely
        // does -- and summarising a short read as though it were a full block
        // would treat a handful of samples as an entire bin and slide every bin
        // after it out of alignment.
        SampleCount readFrames = 0;
        while (readFrames < blockFrames && position + readFrames < info.frameCount) {
            auto got = source.read(position + readFrames,
                                   block.view().subRange(readFrames, blockFrames - readFrames));
            if (!got) {
                return got.error();
            }
            if (got.value() <= 0) {
                break; // genuine end of source
            }
            readFrames += got.value();
        }

        if (readFrames <= 0) {
            // Source ended earlier than its header claimed. Summarise what
            // exists rather than failing, and correct the frame count so the
            // pyramid describes the audio we actually have.
            break;
        }

        const SampleCount binsInBlock = binsToCover(readFrames, baseBinSize);
        for (int channel = 0; channel < info.channelCount(); ++channel) {
            const float* samples = block.channel(channel);
            const auto channelOffset =
                static_cast<std::size_t>(channel) * static_cast<std::size_t>(base.frameCount);

            for (SampleCount bin = 0; bin < binsInBlock; ++bin) {
                const SampleCount start = bin * baseBinSize;
                const SampleCount count = std::min(baseBinSize, readFrames - start);

                float minimum = std::numeric_limits<float>::max();
                float maximum = std::numeric_limits<float>::lowest();
                double sumOfSquares = 0.0;

                for (SampleCount i = 0; i < count; ++i) {
                    const float sample = samples[start + i];
                    minimum = std::min(minimum, sample);
                    maximum = std::max(maximum, sample);
                    sumOfSquares += static_cast<double>(sample) * static_cast<double>(sample);
                }

                const SampleCount target = frameIndex + bin;
                if (target >= base.frameCount) {
                    break;
                }
                PeakFrame& out = base.frames[channelOffset + static_cast<std::size_t>(target)];
                out.minimum = minimum;
                out.maximum = maximum;
                out.rms = static_cast<float>(std::sqrt(sumOfSquares / static_cast<double>(count)));
            }
        }

        frameIndex += binsInBlock;
        position += readFrames;
        monitor.report(static_cast<double>(position) / static_cast<double>(info.frameCount));
    }

    if (position < info.frameCount) {
        pyramid.sourceFrames_ = position;
        base.frameCount = binsToCover(position, baseBinSize);
        base.frames.resize(static_cast<std::size_t>(base.frameCount) * channels);
    }

    pyramid.levels_.push_back(std::move(base));
    pyramid.buildUpperLevels();
    monitor.report(1.0);
    return pyramid;
}

SampleCount PeakPyramid::binSizeAt(int level) const noexcept {
    if (level < 0 || level >= levelCount()) {
        return 0;
    }
    return levels_[static_cast<std::size_t>(level)].binSize;
}

SampleCount PeakPyramid::frameCountAt(int level) const noexcept {
    if (level < 0 || level >= levelCount()) {
        return 0;
    }
    return levels_[static_cast<std::size_t>(level)].frameCount;
}

SampleCount PeakPyramid::samplesInFrame(int level, SampleCount index) const noexcept {
    const SampleCount binSize = binSizeAt(level);
    if (binSize == 0 || index < 0) {
        return 0;
    }
    const SampleCount start = index * binSize;
    if (start >= sourceFrames_) {
        return 0;
    }
    return std::min(binSize, sourceFrames_ - start);
}

const PeakFrame& PeakPyramid::frameAt(int level, int channel, SampleCount index) const noexcept {
    static const PeakFrame silent{};
    if (level < 0 || level >= levelCount() || channel < 0 || channel >= channelCount_) {
        return silent;
    }
    const Level& data = levels_[static_cast<std::size_t>(level)];
    if (index < 0 || index >= data.frameCount) {
        return silent;
    }
    const auto offset =
        static_cast<std::size_t>(channel) * static_cast<std::size_t>(data.frameCount);
    return data.frames[offset + static_cast<std::size_t>(index)];
}

int PeakPyramid::levelForSamplesPerPixel(SampleCount samplesPerPixel) const noexcept {
    if (levels_.empty()) {
        return 0;
    }
    int chosen = 0;
    for (int level = 0; level < levelCount(); ++level) {
        if (binSizeAt(level) <= samplesPerPixel) {
            chosen = level;
        } else {
            break;
        }
    }
    return chosen;
}

PeakFrame PeakPyramid::aggregate(int level, int channel, SampleIndex startSample,
                                 SampleIndex endSample) const noexcept {
    PeakFrame result;
    const SampleCount binSize = binSizeAt(level);
    if (binSize == 0 || endSample <= startSample) {
        return result;
    }

    const SampleCount frameCount = frameCountAt(level);
    const SampleCount first = std::max<SampleIndex>(0, startSample / binSize);
    // endSample is exclusive; the last touched frame is the one holding
    // endSample - 1.
    const SampleCount last = std::min<SampleIndex>(frameCount - 1, (endSample - 1) / binSize);
    if (first > last) {
        return result;
    }

    float minimum = std::numeric_limits<float>::max();
    float maximum = std::numeric_limits<float>::lowest();
    double sumOfSquares = 0.0;
    double totalSamples = 0.0;

    for (SampleCount index = first; index <= last; ++index) {
        const PeakFrame& frame = frameAt(level, channel, index);
        const auto samples = static_cast<double>(samplesInFrame(level, index));

        minimum = std::min(minimum, frame.minimum);
        maximum = std::max(maximum, frame.maximum);
        sumOfSquares += static_cast<double>(frame.rms) * static_cast<double>(frame.rms) * samples;
        totalSamples += samples;
    }

    result.minimum = minimum;
    result.maximum = maximum;
    result.rms =
        totalSamples > 0.0 ? static_cast<float>(std::sqrt(sumOfSquares / totalSamples)) : 0.0f;
    return result;
}

void PeakPyramid::query(int channel, SampleIndex startSample, SampleIndex endSample, PeakFrame* out,
                        int outFrames) const noexcept {
    if (out == nullptr || outFrames <= 0) {
        return;
    }

    // Any column we cannot answer for stays silent rather than undefined, so a
    // view scrolled past the end of the file still draws.
    for (int i = 0; i < outFrames; ++i) {
        out[i] = PeakFrame{};
    }

    if (levels_.empty() || endSample <= startSample || channel < 0 || channel >= channelCount_) {
        return;
    }

    const SampleCount span = endSample - startSample;
    const SampleCount samplesPerPixel = std::max<SampleCount>(1, span / outFrames);
    const int level = levelForSamplesPerPixel(samplesPerPixel);

    for (int i = 0; i < outFrames; ++i) {
        // Compute bounds from the full span each time rather than accumulating,
        // so rounding cannot drift across a wide view.
        const SampleIndex columnStart = startSample + (span * i) / outFrames;
        const SampleIndex columnEnd = startSample + (span * (i + 1)) / outFrames;
        out[i] = aggregate(level, channel, columnStart, columnEnd);
    }
}

std::size_t PeakPyramid::memoryFootprint() const noexcept {
    std::size_t bytes = 0;
    for (const Level& level : levels_) {
        bytes += level.frames.size() * sizeof(PeakFrame);
    }
    return bytes;
}

} // namespace sa::io
