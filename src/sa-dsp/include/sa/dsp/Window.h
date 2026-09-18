#pragma once

#include <sa/core/Types.h>

#include <vector>

namespace sa::dsp {

/// Analysis window shape.
///
/// Hann at 75% overlap is the default: it satisfies the constant-overlap-add
/// condition exactly, so an untouched analysis/synthesis round-trip is
/// transparent, and its sidelobe rolloff is well suited to spectral editing.
enum class WindowType {
    Rectangular,
    Hann,
    Hamming,
    BlackmanHarris,
};

/// Precomputed window coefficients.
class Window {
public:
    Window() = default;
    Window(WindowType type, int length);

    [[nodiscard]] WindowType type() const noexcept { return type_; }

    [[nodiscard]] int length() const noexcept { return static_cast<int>(coefficients_.size()); }

    [[nodiscard]] const float* data() const noexcept { return coefficients_.data(); }

    [[nodiscard]] float operator[](int index) const noexcept {
        return coefficients_[static_cast<std::size_t>(index)];
    }

    /// Sum of the coefficients. Used to normalise magnitude spectra so that a
    /// full-scale sine reads 0 dBFS regardless of window choice.
    [[nodiscard]] double coherentGain() const noexcept { return coherentGain_; }

    /// Worst-case deviation of the overlap-added window envelope from its mean,
    /// for the given hop. Zero means the window satisfies constant-overlap-add
    /// exactly at that hop, and analysis/synthesis round-trips transparently.
    [[nodiscard]] double colaDeviation(int hopSize) const;

private:
    WindowType type_ = WindowType::Rectangular;
    std::vector<float> coefficients_;
    double coherentGain_ = 0.0;
};

} // namespace sa::dsp
