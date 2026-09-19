#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/dsp/Fft.h>
#include <sa/dsp/Window.h>

#include <vector>

namespace sa::analysis {

/// The average spectrum of a passage: what is there, by frequency, over time.
///
/// The spectrogram answers "when"; this answers "how much". They are the same
/// transform and they are not interchangeable -- finding a resonance, checking
/// a tonal balance against a reference, or seeing where a noise floor sits are
/// all questions about a whole passage, and reading them off a spectrogram
/// means averaging by eye.
///
/// Two curves come out, and the pair is more useful than either. The average
/// says what the passage is made of; the peak says what the loudest moment at
/// each frequency was. A resonance that rings shows up in both; a cymbal shows
/// up only in the peak; a mains hum shows up as a spike in the average that the
/// peak barely exceeds, because it never varies.
///
/// It accumulates rather than taking a buffer, so a two-hour programme costs
/// one block of memory: the caller streams whatever block size it has and the
/// carry between calls is handled here. A file analysed in one call and the
/// same file analysed in blocks of 37 give the same answer.
struct SpectrumSettings {
    /// 8192 gives bins about 5.9 Hz apart at 48 kHz -- fine enough to separate
    /// the harmonics of anything with a pitch, and coarse enough that the
    /// curve is readable rather than a comb.
    int fftSize = 8192;

    /// Hop as a fraction of the window: 2 means fifty per cent overlap. More
    /// overlap costs time and buys very little on an average, because the
    /// frames it adds are nearly the frames already counted.
    int overlap = 2;

    dsp::WindowType window = dsp::WindowType::Hann;
};

class SpectrumAnalyser {
public:
    [[nodiscard]] static Result<SpectrumAnalyser> create(SampleRate rate,
                                                         const SpectrumSettings& settings = {});

    [[nodiscard]] int binCount() const noexcept { return fft_.binCount(); }

    [[nodiscard]] int fftSize() const noexcept { return fftSize_; }

    [[nodiscard]] SampleRate sampleRate() const noexcept { return rate_; }

    /// Centre frequency of a bin, in Hz.
    [[nodiscard]] double binFrequency(int bin) const noexcept;

    /// How many analysis frames have gone in. Zero means the curves are empty
    /// and whatever the caller fed was shorter than one window.
    [[nodiscard]] SampleCount frameCount() const noexcept { return frames_; }

    /// Feed more samples. Any count, including none.
    void add(const float* samples, SampleCount count);

    /// Feed one channel of a buffer.
    void add(ConstAudioBufferView audio, int channel);

    /// Says that what comes next is not continuous with what came before.
    ///
    /// For a caller sampling a long programme: taking a minute here and a
    /// minute there is the only way to characterise two hours without reading
    /// two hours, but the join between two such stretches is a discontinuity,
    /// and a frame straddling it would measure the edit rather than the audio.
    /// This clears the history and starts the next frame afresh, keeping
    /// everything already accumulated.
    void startSegment() noexcept;

    void reset() noexcept;

    /// Mean magnitude per bin, in dBFS.
    ///
    /// Scaled so that a full-scale sine sitting on a bin centre reads 0 dBFS,
    /// whatever window is in use -- which is the only normalisation that makes
    /// two spectra comparable, and the one a reader assumes without being told.
    /// Bins that were never analysed read `kSilenceDb`.
    [[nodiscard]] std::vector<float> averageDb() const;

    /// Largest magnitude any single frame reached, per bin, in dBFS.
    [[nodiscard]] std::vector<float> peakDb() const;

    /// What an empty or silent bin reads. Far below anything audible, and a
    /// real number rather than an infinity, so it can be plotted.
    static constexpr float kSilenceDb = -200.0f;

private:
    SpectrumAnalyser(SampleRate rate, int fftSize, int hopSize, dsp::WindowType window);

    void analyseFrame();

    SampleRate rate_{0.0};
    int fftSize_ = 0;
    int hopSize_ = 0;
    double scale_ = 1.0;
    dsp::RealFft fft_;
    dsp::Window window_;

    /// A ring of the most recent fftSize samples, so a frame can be taken at
    /// any hop boundary regardless of how the caller chopped up its input.
    std::vector<float> history_;
    SampleCount written_ = 0;
    SampleCount untilNextFrame_ = 0;

    std::vector<float> block_;
    std::vector<std::complex<float>> spectrum_;
    std::vector<double> total_;
    std::vector<double> loudest_;
    SampleCount frames_ = 0;
};

} // namespace sa::analysis
