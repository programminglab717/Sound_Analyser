#include <sa/core/Result.h>
#include <sa/core/Types.h>

#include <catch2/catch_test_macros.hpp>

using namespace sa;

TEST_CASE("SampleRate validity", "[core][types]") {
    CHECK(kSampleRate44100.isValid());
    CHECK(kSampleRate48000.isValid());
    CHECK(SampleRate{768000.0}.isValid());

    CHECK_FALSE(SampleRate{}.isValid());
    CHECK_FALSE(SampleRate{0.0}.isValid());
    CHECK_FALSE(SampleRate{-48000.0}.isValid());
    CHECK_FALSE(SampleRate{768001.0}.isValid());
}

TEST_CASE("Sample and second conversions round-trip", "[core][types]") {
    CHECK(samplesToSeconds(48000, kSampleRate48000) == 1.0);
    CHECK(secondsToSamples(1.0, kSampleRate48000) == 48000);
    CHECK(secondsToSamples(2.5, kSampleRate44100) == 110250);
}

TEST_CASE("Second conversion rounds to nearest, both signs", "[core][types]") {
    // 0.5 samples up, not truncated toward zero -- truncation accumulates a
    // systematic bias when converting many marker positions.
    CHECK(secondsToSamples(1.0 / 96000.0, kSampleRate48000) == 1);
    CHECK(secondsToSamples(-1.0 / 96000.0, kSampleRate48000) == -1);
    CHECK(secondsToSamples(-1.0, kSampleRate48000) == -48000);
}

TEST_CASE("Invalid rates convert to zero rather than infinity", "[core][types]") {
    // An infinity here would propagate silently through a whole timeline.
    CHECK(samplesToSeconds(48000, SampleRate{0.0}) == 0.0);
    CHECK(secondsToSamples(1.0, SampleRate{0.0}) == 0);
}

TEST_CASE("Result carries either a value or an error", "[core][result]") {
    Result<int> good{42};
    REQUIRE(good.hasValue());
    CHECK(good.value() == 42);
    CHECK(good.valueOr(7) == 42);

    Result<int> bad{Error{ErrorCode::NotFound, "no such file"}};
    REQUIRE_FALSE(bad.hasValue());
    CHECK(bad.error().code() == ErrorCode::NotFound);
    CHECK(bad.error().what() == "no such file");
    CHECK(bad.valueOr(7) == 7);
}

TEST_CASE("Error falls back to the category name", "[core][result]") {
    const Error error{ErrorCode::UnsupportedFormat};
    CHECK(error.what() == "unsupported format");
}

TEST_CASE("Status defaults to ok", "[core][result]") {
    CHECK(Status{}.ok());
    CHECK_FALSE(Status{Error{ErrorCode::IoFailure}}.ok());
}
