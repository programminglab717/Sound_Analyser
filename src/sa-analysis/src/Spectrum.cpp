#include <sa/analysis/Decibels.h>
#include <sa/analysis/Spectrum.h>

#include <algorithm>
#include <cmath>

namespace sa::analysis {

namespace {

[[nodiscard]] bool isPowerOfTwo(int value) noexcept {
    return value > 0 && (value & (value - 1)) == 0;
}

} // namespace

Result<SpectrumAnalyser> SpectrumAnalyser::create(SampleRate rate,
                                                  const SpectrumSettings& settings) {
    if (!rate.isValid()) {
        return Error{ErrorCode::InvalidArgument, "the sample rate is not usable"};
    }
    if (!isPowerOfTwo(settings.fftSize) || settings.fftSize < 64) {
        return Error{ErrorCode::InvalidArgument, "the transform size must be a power of two >= 64"};
    }
    if (settings.overlap < 1 || settings.overlap > 16 || settings.fftSize % settings.overlap != 0) {
        return Error{ErrorCode::InvalidArgument,
                     "the overlap must be between 1 and 16 and divide the transform size"};
    }
    return SpectrumAnalyser{rate, settings.fftSize, settings.fftSize / settings.overlap,
                            settings.window};
}

SpectrumAnalyser::SpectrumAnalyser(SampleRate rate, int fftSize, int hopSize,
                                   dsp::WindowType window)
    : rate_(rate), fftSize_(fftSize), hopSize_(hopSize), fft_(fftSize), window_(window, fftSize) {
    // A window scatters a sine's energy over several bins and scales it by the
    // window's own mean. Dividing by the coherent gain and doubling for the
    // half spectrum puts a full-scale sine at 0 dBFS, which is what makes two
    // spectra taken with different windows comparable.
    scale_ = window_.coherentGain() > 0.0 ? 2.0 / window_.coherentGain() : 1.0;

    history_.assign(static_cast<std::size_t>(fftSize_), 0.0f);
    block_.assign(static_cast<std::size_t>(fftSize_), 0.0f);
    spectrum_.assign(static_cast<std::size_t>(fft_.binCount()), {});
    total_.assign(static_cast<std::size_t>(fft_.binCount()), 0.0);
    loudest_.assign(static_cast<std::size_t>(fft_.binCount()), 0.0);
    untilNextFrame_ = fftSize_;
}

double SpectrumAnalyser::binFrequency(int bin) const noexcept {
    return fftSize_ > 0 ? rate_.hz() * static_cast<double>(bin) / static_cast<double>(fftSize_)
                        : 0.0;
}

void SpectrumAnalyser::startSegment() noexcept {
    std::fill(history_.begin(), history_.end(), 0.0f);
    written_ = 0;
    untilNextFrame_ = fftSize_;
}

void SpectrumAnalyser::reset() noexcept {
    std::fill(history_.begin(), history_.end(), 0.0f);
    std::fill(total_.begin(), total_.end(), 0.0);
    std::fill(loudest_.begin(), loudest_.end(), 0.0);
    written_ = 0;
    untilNextFrame_ = fftSize_;
    frames_ = 0;
}

void SpectrumAnalyser::analyseFrame() {
    // The ring holds the last fftSize samples; unwrap it into the block in
    // order, oldest first.
    const auto size = static_cast<SampleCount>(fftSize_);
    const SampleCount start = written_ % size;
    for (SampleCount i = 0; i < size; ++i) {
        block_[static_cast<std::size_t>(i)] =
            history_[static_cast<std::size_t>((start + i) % size)] * window_[static_cast<int>(i)];
    }

    fft_.forward(block_.data(), spectrum_.data());
    for (std::size_t bin = 0; bin < spectrum_.size(); ++bin) {
        const double magnitude = std::abs(spectrum_[bin]) * scale_;
        total_[bin] += magnitude;
        loudest_[bin] = std::max(loudest_[bin], magnitude);
    }
    ++frames_;
}

void SpectrumAnalyser::add(const float* samples, SampleCount count) {
    if (samples == nullptr || count <= 0) {
        return;
    }
    const auto size = static_cast<SampleCount>(fftSize_);
    for (SampleCount i = 0; i < count; ++i) {
        history_[static_cast<std::size_t>(written_ % size)] = samples[i];
        ++written_;
        if (--untilNextFrame_ == 0) {
            analyseFrame();
            untilNextFrame_ = hopSize_;
        }
    }
}

void SpectrumAnalyser::add(ConstAudioBufferView audio, int channel) {
    if (channel < 0 || channel >= audio.channelCount()) {
        return;
    }
    add(audio.channel(channel), audio.frames());
}

std::vector<float> SpectrumAnalyser::averageDb() const {
    std::vector<float> result(total_.size(), kSilenceDb);
    if (frames_ == 0) {
        return result;
    }
    for (std::size_t bin = 0; bin < total_.size(); ++bin) {
        const double mean = total_[bin] / static_cast<double>(frames_);
        result[bin] = mean > 0.0 ? static_cast<float>(20.0 * std::log10(mean)) : kSilenceDb;
        result[bin] = std::max(result[bin], kSilenceDb);
    }
    return result;
}

std::vector<float> SpectrumAnalyser::peakDb() const {
    std::vector<float> result(loudest_.size(), kSilenceDb);
    for (std::size_t bin = 0; bin < loudest_.size(); ++bin) {
        result[bin] =
            loudest_[bin] > 0.0
                ? std::max(static_cast<float>(20.0 * std::log10(loudest_[bin])), kSilenceDb)
                : kSilenceDb;
    }
    return result;
}

} // namespace sa::analysis
