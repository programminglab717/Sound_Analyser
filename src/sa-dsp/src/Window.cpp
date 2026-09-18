#include <sa/dsp/Window.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>
#include <vector>

namespace sa::dsp {

Window::Window(WindowType type, int length) : type_(type) {
    if (length <= 0) {
        return;
    }

    coefficients_.resize(static_cast<std::size_t>(length));
    const auto n = static_cast<double>(length);

    for (int i = 0; i < length; ++i) {
        // Periodic (not symmetric) form: divide by N, not N-1. The periodic
        // form is what satisfies constant-overlap-add; the symmetric form is
        // for filter design and would break the round-trip guarantee.
        const double phase = 2.0 * std::numbers::pi * static_cast<double>(i) / n;
        double value = 1.0;

        switch (type) {
        case WindowType::Rectangular:
            value = 1.0;
            break;
        case WindowType::Hann:
            value = 0.5 - 0.5 * std::cos(phase);
            break;
        case WindowType::Hamming:
            value = 0.54 - 0.46 * std::cos(phase);
            break;
        case WindowType::BlackmanHarris:
            value = 0.35875 - 0.48829 * std::cos(phase) + 0.14128 * std::cos(2.0 * phase) -
                    0.01168 * std::cos(3.0 * phase);
            break;
        }

        coefficients_[static_cast<std::size_t>(i)] = static_cast<float>(value);
        coherentGain_ += value;
    }
}

double Window::colaDeviation(int hopSize) const {
    const int length = this->length();
    if (length <= 0 || hopSize <= 0 || hopSize > length) {
        return 0.0;
    }

    // Overlap-add the window onto itself at the given hop and measure how far
    // the resulting envelope departs from flat. Only one hop period needs
    // checking -- the envelope is periodic with period hopSize.
    std::vector<double> envelope(static_cast<std::size_t>(hopSize), 0.0);
    for (int i = 0; i < length; ++i) {
        const auto slot = static_cast<std::size_t>(i % hopSize);
        envelope[slot] += static_cast<double>(coefficients_[static_cast<std::size_t>(i)]);
    }

    const auto [minimum, maximum] = std::minmax_element(envelope.begin(), envelope.end());
    const double mean =
        std::accumulate(envelope.begin(), envelope.end(), 0.0) / static_cast<double>(hopSize);
    if (mean == 0.0) {
        return 0.0;
    }
    return std::max(std::abs(*maximum - mean), std::abs(*minimum - mean)) / mean;
}

} // namespace sa::dsp
