#include <sa/core/ChannelLayout.h>

#include <catch2/catch_test_macros.hpp>

using namespace sa;

TEST_CASE("Standard layouts have the expected channel counts", "[core][layout]") {
    CHECK(ChannelLayout{}.count() == 0);
    CHECK(ChannelLayout::mono().count() == 1);
    CHECK(ChannelLayout::stereo().count() == 2);
    CHECK(ChannelLayout::fiveOne().count() == 6);
    CHECK(ChannelLayout::sevenOne().count() == 8);
}

TEST_CASE("Speaker assignment is ordered", "[core][layout]") {
    const auto layout = ChannelLayout::fiveOne();
    CHECK(layout.at(0) == Speaker::Left);
    CHECK(layout.at(1) == Speaker::Right);
    CHECK(layout.at(2) == Speaker::Centre);
    CHECK(layout.at(3) == Speaker::Lfe);
    CHECK(layout.at(4) == Speaker::LeftSurround);
    CHECK(layout.at(5) == Speaker::RightSurround);
}

TEST_CASE("Out-of-range access yields Unknown rather than reading past the end", "[core][layout]") {
    const auto layout = ChannelLayout::stereo();
    CHECK(layout.at(-1) == Speaker::Unknown);
    CHECK(layout.at(2) == Speaker::Unknown);
    CHECK(layout.at(9999) == Speaker::Unknown);
}

TEST_CASE("indexOf finds assigned speakers", "[core][layout]") {
    const auto layout = ChannelLayout::fiveOne();
    CHECK(layout.indexOf(Speaker::Centre) == 2);
    CHECK(layout.indexOf(Speaker::Lfe) == 3);
    CHECK(layout.indexOf(Speaker::LeftHeight) == -1);
}

TEST_CASE("Discrete layouts clamp to the channel ceiling", "[core][layout]") {
    CHECK(ChannelLayout::discrete(0).count() == 0);
    CHECK(ChannelLayout::discrete(16).count() == 16);
    CHECK(ChannelLayout::discrete(-5).count() == 0);
    CHECK(ChannelLayout::discrete(kMaxChannels + 100).count() == kMaxChannels);
}

TEST_CASE("Equality compares both count and speaker assignment", "[core][layout]") {
    CHECK(ChannelLayout::stereo() == ChannelLayout::stereo());
    CHECK_FALSE(ChannelLayout::stereo() == ChannelLayout::mono());

    // Same channel count, different meaning: a 2-channel discrete recording is
    // not a stereo pair, and processing must not treat them alike.
    CHECK_FALSE(ChannelLayout::stereo() == ChannelLayout::discrete(2));
}

TEST_CASE("Layout names", "[core][layout]") {
    CHECK(ChannelLayout::mono().name() == "mono");
    CHECK(ChannelLayout::stereo().name() == "stereo");
    CHECK(ChannelLayout::fiveOne().name() == "5.1");
    CHECK(ChannelLayout::sevenOne().name() == "7.1");
    CHECK(ChannelLayout{}.name() == "empty");
    CHECK(ChannelLayout::discrete(12).name() == "12ch");
}

TEST_CASE("Discrete layout names stay valid after later calls", "[core][layout]") {
    // The name is interned; a naive implementation returning a view of a local
    // string would dangle here.
    const auto first = ChannelLayout::discrete(9).name();
    const auto second = ChannelLayout::discrete(11).name();
    CHECK(first == "9ch");
    CHECK(second == "11ch");
}
