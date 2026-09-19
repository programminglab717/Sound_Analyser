#include "FrameAnalysis.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace sa::spectral::detail {

namespace {

constexpr SampleCount kReadBlock = 1 << 16;

} // namespace

void quantiseFrame(const std::complex<float>* spectra, int bins, float reference,
                   const SpectrogramConfig& config, std::uint8_t* out) noexcept {
    const float span = config.maximumDecibels - config.minimumDecibels;
    for (int bin = 0; bin < bins; ++bin) {
        const float magnitude = std::abs(spectra[bin]) / reference;
        // Floor before the log so that true silence maps to the bottom of the
        // range rather than to negative infinity.
        const float decibels =
            magnitude > 1e-12f ? 20.0f * std::log10(magnitude) : config.minimumDecibels;
        const float normalised = (decibels - config.minimumDecibels) / span;
        out[bin] = static_cast<std::uint8_t>(std::clamp(normalised, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
}

FrameAnalyser::FrameAnalyser(const io::AudioSource& source, int channel,
                             const SpectrogramConfig& config, dsp::Stft stft)
    : source_(&source), channel_(channel), config_(config), stft_(std::move(stft)),
      fft_(config.fftSize), readInto_(source.info().layout, kReadBlock) {
    const io::AudioFileInfo& info = source.info();
    sourceFrames_ = info.frameCount;
    frameCount_ = stft_.frameCount(info.frameCount);
    padding_ = config_.fftSize - config_.hopSize;
    reference_ = static_cast<float>(stft_.window().coherentGain() * 0.5);

    spectrum_.resize(static_cast<std::size_t>(stft_.binCount()));
    windowed_.assign(static_cast<std::size_t>(config_.fftSize), 0.0f);

    capacity_ = kReadBlock + config_.fftSize;
    buffer_.assign(static_cast<std::size_t>(capacity_), 0.0f);
}

Result<FrameAnalyser> FrameAnalyser::create(const io::AudioSource& source, int channel,
                                            const SpectrogramConfig& config) {
    if (config.maximumDecibels <= config.minimumDecibels) {
        return Error{ErrorCode::InvalidArgument, "maximumDecibels must exceed minimumDecibels"};
    }
    const io::AudioFileInfo& info = source.info();
    if (channel < 0 || channel >= info.channelCount()) {
        return Error{ErrorCode::OutOfRange, "channel index outside the source"};
    }
    auto stft = dsp::Stft::create(config.fftSize, config.hopSize, config.window);
    if (!stft) {
        return stft.error();
    }
    return FrameAnalyser{source, channel, config, std::move(stft).value()};
}

Status FrameAnalyser::fill(SampleIndex wantFrom, SampleIndex wantTo) {
    // Anything already buffered that the window still needs is kept; the rest
    // is dropped and the buffer refilled from where the window starts.
    while (wantTo > bufferStart_ + bufferFill_ && bufferStart_ + bufferFill_ < sourceFrames_) {
        const SampleIndex keepFrom = std::max<SampleIndex>(bufferStart_, wantFrom);
        const SampleCount keep = std::max<SampleCount>(0, bufferStart_ + bufferFill_ - keepFrom);
        if (keep > 0 && keepFrom != bufferStart_) {
            std::copy(buffer_.begin() + static_cast<std::ptrdiff_t>(keepFrom - bufferStart_),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(keepFrom - bufferStart_ + keep),
                      buffer_.begin());
        }
        const SampleIndex readCursor =
            keep > 0 ? keepFrom + keep : std::max<SampleIndex>(0, wantFrom);
        bufferStart_ = keep > 0 ? keepFrom : readCursor;
        bufferFill_ = keep;

        if (readCursor >= sourceFrames_) {
            break;
        }
        const SampleCount room = capacity_ - bufferFill_;
        const SampleCount want =
            std::min<SampleCount>({room, kReadBlock, sourceFrames_ - readCursor});
        if (want <= 0) {
            break;
        }
        AudioBufferView view = readInto_.view().subRange(0, want);
        const auto read = source_->read(readCursor, view);
        if (!read) {
            return read.error();
        }
        const SampleCount got = read.value();
        if (got <= 0) {
            break;
        }
        std::copy_n(view.channel(channel_), got,
                    buffer_.begin() + static_cast<std::ptrdiff_t>(bufferFill_));
        bufferFill_ += got;
    }
    return {};
}

Status FrameAnalyser::analyse(SampleCount frame, std::uint8_t* out) {
    const SampleIndex windowStart = frame * config_.hopSize - padding_;
    const SampleIndex windowEnd = windowStart + config_.fftSize;

    const SampleIndex wantFrom = std::max<SampleIndex>(0, windowStart);
    const SampleIndex wantTo = std::min<SampleIndex>(sourceFrames_, windowEnd);

    // A backwards jump leaves the buffer sitting past the window, and the
    // forward-only refill above cannot rewind. Reset and let it read again --
    // correct, and the price of seeking rather than sweeping.
    if (wantFrom < bufferStart_) {
        bufferStart_ = wantFrom;
        bufferFill_ = 0;
    }
    if (const Status status = fill(wantFrom, wantTo); !status) {
        return status;
    }

    for (int i = 0; i < config_.fftSize; ++i) {
        const SampleIndex index = windowStart + i;
        const bool inside = index >= 0 && index < sourceFrames_ && index >= bufferStart_ &&
                            index < bufferStart_ + bufferFill_;
        const float sample =
            inside ? buffer_[static_cast<std::size_t>(index - bufferStart_)] : 0.0f;
        windowed_[static_cast<std::size_t>(i)] = sample * stft_.window()[i];
    }
    fft_.forward(windowed_.data(), spectrum_.data());
    quantiseFrame(spectrum_.data(), stft_.binCount(), reference_, config_, out);
    return {};
}

} // namespace sa::spectral::detail
