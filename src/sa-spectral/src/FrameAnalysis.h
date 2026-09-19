#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/dsp/Fft.h>
#include <sa/dsp/Stft.h>
#include <sa/io/AudioSource.h>
#include <sa/spectral/SpectrogramPyramid.h>

#include <complex>
#include <cstdint>
#include <vector>

/// Internal to sa-spectral. Not in include/ because nothing outside this
/// library should depend on how a spectrogram frame is produced.
namespace sa::spectral::detail {

/// Quantise one frame of complex spectra into the pyramid's 8-bit log scale.
void quantiseFrame(const std::complex<float>* spectra, int bins, float reference,
                   const SpectrogramConfig& config, std::uint8_t* out) noexcept;

/// Produces the quantised magnitudes of any single STFT frame of a source.
///
/// Extracted so that the whole-file build and the tiled cache cannot disagree
/// about framing. They did not, when this was written -- but they were two
/// copies of the same twenty lines, which is two chances for one of them to
/// drift by a hop, and a spectrogram out of step with the waveform above it is
/// the kind of bug nobody reports because it just looks slightly wrong.
///
/// The framing is the one analyse() defines: the signal is treated as
/// zero-padded at the front by fftSize - hopSize, so frame f begins at
/// f * hop - padding and is zero outside the file at both ends.
class FrameAnalyser {
public:
    [[nodiscard]] static Result<FrameAnalyser> create(const io::AudioSource& source, int channel,
                                                      const SpectrogramConfig& config);

    /// Total frames covering the whole source.
    [[nodiscard]] SampleCount frameCount() const noexcept { return frameCount_; }

    [[nodiscard]] int binCount() const noexcept { return stft_.binCount(); }

    /// Write `binCount()` quantised magnitudes for `frame` into `out`.
    ///
    /// Frames are expected roughly in order: the read buffer slides forward and
    /// refills when a window runs past it, so a sequential sweep reads the file
    /// once. Jumping backwards is correct but re-reads.
    [[nodiscard]] Status analyse(SampleCount frame, std::uint8_t* out);

private:
    FrameAnalyser(const io::AudioSource& source, int channel, const SpectrogramConfig& config,
                  dsp::Stft stft);

    [[nodiscard]] Status fill(SampleIndex wantFrom, SampleIndex wantTo);

    const io::AudioSource* source_ = nullptr;
    int channel_ = 0;
    SpectrogramConfig config_;
    dsp::Stft stft_;
    dsp::RealFft fft_;
    SampleCount sourceFrames_ = 0;
    SampleCount frameCount_ = 0;
    int padding_ = 0;
    float reference_ = 1.0f;

    std::vector<std::complex<float>> spectrum_;
    std::vector<float> windowed_;

    // The sliding read buffer. Windows overlap by fftSize - hopSize, so reading
    // one window per frame would read the file fftSize/hopSize times over --
    // four times at the display settings, and that dominated the time to open a
    // long file.
    std::vector<float> buffer_;
    AudioBuffer readInto_;
    SampleIndex bufferStart_ = 0;
    SampleCount bufferFill_ = 0;
    SampleCount capacity_ = 0;
};

} // namespace sa::spectral::detail
