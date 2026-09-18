#include <sa/dsp/Window.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace sa::dsp;
using Catch::Approx;

TEST_CASE("Window lengths and degenerate input", "[dsp][window]") {
    CHECK(Window{WindowType::Hann, 1024}.length() == 1024);
    CHECK(Window{WindowType::Hann, 0}.length() == 0);
    CHECK(Window{WindowType::Hann, -4}.length() == 0);
}

TEST_CASE("A rectangular window is flat", "[dsp][window]") {
    const Window window{WindowType::Rectangular, 64};
    for (int i = 0; i < window.length(); ++i) {
        CHECK(window[i] == 1.0f);
    }
    CHECK(window.coherentGain() == Approx(64.0));
}

TEST_CASE("Hann uses the periodic form", "[dsp][window]") {
    // Periodic Hann starts at exactly zero and, crucially, does NOT return to
    // zero at the final sample -- that asymmetry is what makes overlap-add sum
    // to a constant. The symmetric form would break the round-trip guarantee.
    const Window window{WindowType::Hann, 8};
    CHECK(window[0] == Approx(0.0).margin(1e-6));
    CHECK(window[4] == Approx(1.0).epsilon(1e-6));
    CHECK(window[7] > 0.0f);
    CHECK(window.coherentGain() == Approx(4.0).epsilon(1e-6));
}

TEST_CASE("Hann satisfies constant-overlap-add at power-of-two hops", "[dsp][window][cola]") {
    // Tolerance is float32 epsilon, not zero: the coefficients are stored as
    // float, so the summed envelope lands within ~1e-7 of flat. Demanding
    // exact zero would be testing the storage type, not the window.
    const Window window{WindowType::Hann, 1024};
    CHECK(window.colaDeviation(512) == Approx(0.0).margin(1e-6));
    CHECK(window.colaDeviation(256) == Approx(0.0).margin(1e-6));
    CHECK(window.colaDeviation(128) == Approx(0.0).margin(1e-6));
}

TEST_CASE("A rectangular window fails COLA where Hann succeeds", "[dsp][window][cola]") {
    // Guards against colaDeviation trivially returning zero: it must actually
    // discriminate between windows.
    const Window rectangular{WindowType::Rectangular, 1024};
    CHECK(rectangular.colaDeviation(1024) == Approx(0.0).margin(1e-6));

    const Window blackman{WindowType::BlackmanHarris, 1024};
    CHECK(blackman.colaDeviation(512) > 0.1);
}

TEST_CASE("Coherent gain is the coefficient sum", "[dsp][window]") {
    const Window window{WindowType::Hamming, 512};
    double sum = 0.0;
    for (int i = 0; i < window.length(); ++i) {
        sum += static_cast<double>(window[i]);
    }
    CHECK(window.coherentGain() == Approx(sum));
}
