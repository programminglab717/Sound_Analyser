#include <sa/ui/Settings.h>

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace sa;
using namespace sa::ui;

namespace {

/// A settings map built from pairs, the way a file's contents arrive.
[[nodiscard]] SettingsMap mapOf(std::vector<std::pair<std::string, std::string>> entries) {
    SettingsMap stored;
    for (auto& [key, value] : entries) {
        stored.set(key, value);
    }
    return stored;
}

/// A 1920x1080 screen at the origin: the one everybody has.
constexpr Rect kPrimary{0, 0, 1920, 1080};

/// A second screen to the right of it, as a docked laptop has.
constexpr Rect kSecond{1920, 0, 2560, 1440};

/// A window that is comfortably on the primary screen.
constexpr Rect kOnScreen{100, 100, 1280, 760};

} // namespace

// --------------------------------------------------------------------------
// Nothing stored yet
// --------------------------------------------------------------------------

TEST_CASE("A first run with no settings file gets the defaults the window had", "[ui][settings]") {
    const SavedSettings saved = readSettings(SettingsMap{});

    // Not a stored 1280x760: there is a difference between "open at the
    // default size" and "open where you were", and only the second one should
    // stop a batch run being resized to something predictable.
    REQUIRE_FALSE(saved.window.has_value());
    REQUIRE(saved.mainSplit.empty());
    REQUIRE(saved.sideSplit.empty());
    REQUIRE(saved.recent.empty());
    REQUIRE(saved.writeBack);

    const Preferences defaults;
    REQUIRE(saved.preferences.exportFormat == defaults.exportFormat);
    REQUIRE(saved.preferences.dither == defaults.dither);
    REQUIRE(saved.preferences.fadeShape == defaults.fadeShape);
    REQUIRE(saved.preferences.loudnessTarget == defaults.loudnessTarget);
    REQUIRE(saved.preferences.fftSize == 4096);
    REQUIRE(saved.preferences.hopSize == 1024);
    REQUIRE(saved.preferences.spectrogramFloorDb == -96.0);
    REQUIRE(saved.preferences.frequencyScale == FrequencyScale::Logarithmic);
    REQUIRE(saved.preferences.colourmap == Colourmap::Magma);
    REQUIRE(saved.preferences.showBeatGrid);
    REQUIRE_FALSE(saved.preferences.showPitchContour);
    REQUIRE(saved.preferences.analysisSeconds == 120.0);
    REQUIRE(saved.preferences.keySeconds == 60.0);
}

TEST_CASE("Everything written comes back as what was written", "[ui][settings]") {
    SavedSettings wanted;
    wanted.window = WindowPlacement{Rect{40, 60, 1600, 900}, true};
    wanted.mainSplit = {300, 460};
    wanted.sideSplit = {420, 230, 140};
    wanted.recent.remember("/music/one.wav");
    wanted.recent.remember("/music/two.wav");
    wanted.preferences.exportFormat = io::SampleFormat::PcmInt16;
    wanted.preferences.dither = dsp::DitherType::TpdfNoiseShaped;
    wanted.preferences.fadeShape = engine::FadeShape::SCurve;
    wanted.preferences.loudnessTarget = analysis::LoudnessPlatform::Podcast;
    wanted.preferences.fftSize = 8192;
    wanted.preferences.hopSize = 2048;
    wanted.preferences.spectrogramFloorDb = -120.0;
    wanted.preferences.frequencyScale = FrequencyScale::Linear;
    wanted.preferences.colourmap = Colourmap::Viridis;
    wanted.preferences.showBeatGrid = false;
    wanted.preferences.showPitchContour = true;
    wanted.preferences.showOctaveBands = true;
    wanted.preferences.measureRoom = true;
    wanted.preferences.analysisSeconds = 300.0;
    wanted.preferences.keySeconds = 90.0;

    const SavedSettings back = readSettings(writeSettings(wanted));

    REQUIRE(back.window.has_value());
    REQUIRE(back.window->frame == Rect{40, 60, 1600, 900});
    REQUIRE(back.window->maximised);
    REQUIRE(back.mainSplit == std::vector<int>{300, 460});
    REQUIRE(back.sideSplit == std::vector<int>{420, 230, 140});
    REQUIRE(back.recent.paths().size() == 2);
    REQUIRE(back.recent.paths().front() == std::filesystem::path{"/music/two.wav"});
    REQUIRE(back.preferences.exportFormat == io::SampleFormat::PcmInt16);
    REQUIRE(back.preferences.dither == dsp::DitherType::TpdfNoiseShaped);
    REQUIRE(back.preferences.fadeShape == engine::FadeShape::SCurve);
    REQUIRE(back.preferences.loudnessTarget == analysis::LoudnessPlatform::Podcast);
    REQUIRE(back.preferences.fftSize == 8192);
    REQUIRE(back.preferences.hopSize == 2048);
    REQUIRE(back.preferences.spectrogramFloorDb == -120.0);
    REQUIRE(back.preferences.frequencyScale == FrequencyScale::Linear);
    REQUIRE(back.preferences.colourmap == Colourmap::Viridis);
    REQUIRE_FALSE(back.preferences.showBeatGrid);
    REQUIRE(back.preferences.showPitchContour);
    REQUIRE(back.preferences.showOctaveBands);
    REQUIRE(back.preferences.measureRoom);
    REQUIRE(back.preferences.analysisSeconds == 300.0);
    REQUIRE(back.preferences.keySeconds == 90.0);
}

// --------------------------------------------------------------------------
// A file somebody or something has damaged
// --------------------------------------------------------------------------

TEST_CASE("A value that is not a number leaves its setting at the default", "[ui][settings]") {
    const SavedSettings saved = readSettings(mapOf({
        {"window/x", "over there"},
        {"window/y", ""},
        {"window/width", "1280px"},
        {"window/height", "760"},
        {"preferences/fftsize", "four thousand"},
        {"preferences/analysisseconds", "ages"},
    }));

    // The rectangle is still read, because all four keys are present -- what
    // is rejected is the individual values, which fall back one at a time.
    REQUIRE(saved.window.has_value());
    REQUIRE(saved.window->frame.width == kDefaultWindowWidth);
    REQUIRE(saved.window->frame.height == 760);
    REQUIRE(saved.preferences.fftSize == 4096);
    REQUIRE(saved.preferences.analysisSeconds == 120.0);
}

TEST_CASE("Half a window rectangle is not a position", "[ui][settings]") {
    const SavedSettings saved = readSettings(mapOf({{"window/x", "40"}, {"window/y", "60"}}));
    REQUIRE_FALSE(saved.window.has_value());
}

TEST_CASE("A stored infinity or NaN is not a setting", "[ui][settings]") {
    const SavedSettings saved = readSettings(mapOf({
        {"preferences/analysisseconds", "inf"},
        {"preferences/keyseconds", "nan"},
        {"preferences/spectrogramfloordb", "-inf"},
    }));
    REQUIRE(saved.preferences.analysisSeconds == 120.0);
    REQUIRE(saved.preferences.keySeconds == 60.0);
    REQUIRE(saved.preferences.spectrogramFloorDb == -96.0);
}

TEST_CASE("Rubbish where a name should be leaves the default", "[ui][settings]") {
    const SavedSettings saved = readSettings(mapOf({
        {"preferences/colourmap", "rainbow"},
        {"preferences/dither", "loud"},
        {"preferences/exportformat", "8"},
        {"preferences/loudnesstarget", "myspace"},
        {"preferences/beatgrid", "perhaps"},
    }));
    REQUIRE(saved.preferences.colourmap == Colourmap::Magma);
    REQUIRE(saved.preferences.dither == dsp::DitherType::Tpdf);
    REQUIRE(saved.preferences.exportFormat == io::SampleFormat::PcmInt24);
    REQUIRE(saved.preferences.loudnessTarget == analysis::LoudnessPlatform::EbuR128);
    REQUIRE(saved.preferences.showBeatGrid);
}

TEST_CASE("A name is matched whatever case it was typed in", "[ui][settings]") {
    const SavedSettings saved = readSettings(mapOf({
        {"preferences/colourmap", "Viridis"},
        {"preferences/beatgrid", "FALSE"},
        {"preferences/octavebands", "1"},
    }));
    REQUIRE(saved.preferences.colourmap == Colourmap::Viridis);
    REQUIRE_FALSE(saved.preferences.showBeatGrid);
    REQUIRE(saved.preferences.showOctaveBands);
}

TEST_CASE("Keys this build has never heard of are ignored", "[ui][settings]") {
    const SavedSettings saved = readSettings(mapOf({
        {"preferences/telepathy", "on"},
        {"window/monitor-serial-number", "37"},
        {"preferences/colourmap", "grey"},
    }));
    REQUIRE(saved.preferences.colourmap == Colourmap::Grey);
    REQUIRE(saved.writeBack);
}

// --------------------------------------------------------------------------
// A file from a build that knows more than this one
// --------------------------------------------------------------------------

TEST_CASE("A settings file from a newer build is read but not written over", "[ui][settings]") {
    const SavedSettings saved = readSettings(mapOf({
        {"version", "9"},
        {"preferences/colourmap", "viridis"},
        {"window/x", "10"},
        {"window/y", "20"},
        {"window/width", "800"},
        {"window/height", "600"},
    }));

    // Read: a user running the old build once still gets their window back.
    REQUIRE(saved.preferences.colourmap == Colourmap::Viridis);
    REQUIRE(saved.window.has_value());
    // Not written: their newer settings are not this build's to destroy.
    REQUIRE_FALSE(saved.writeBack);
}

TEST_CASE("This build's own file, and one older than it, are written back", "[ui][settings]") {
    REQUIRE(readSettings(mapOf({{"version", std::to_string(kSettingsVersion)}})).writeBack);
    REQUIRE(readSettings(mapOf({{"version", "0"}})).writeBack);
    // A version that is not a number is damage rather than the future, and a
    // damaged file that is never written again is a damaged file for ever.
    REQUIRE(readSettings(mapOf({{"version", "banana"}})).writeBack);
}

// --------------------------------------------------------------------------
// The window opening where nobody can see it
// --------------------------------------------------------------------------

TEST_CASE("A window saved where it still fits is left exactly where it was", "[ui][settings]") {
    REQUIRE(confineToScreens(kOnScreen, {kPrimary}) == kOnScreen);
}

TEST_CASE("A window saved on a monitor that is gone comes back onto one that is here",
          "[ui][settings]") {
    // The case this rule exists for: saved on the second screen of a docked
    // laptop, reopened on the train.
    const Rect saved{2400, 300, 1280, 760};
    const Rect placed = confineToScreens(saved, {kPrimary});

    REQUIRE(placed.width == 1280);
    REQUIRE(placed.height == 760);
    REQUIRE(isReachable(placed, kPrimary));
    // Centred, which for these numbers is exactly this.
    REQUIRE(placed.x == (1920 - 1280) / 2);
    REQUIRE(placed.y == (1080 - 760) / 2);
}

TEST_CASE("A window still on its own monitor is left there", "[ui][settings]") {
    const Rect saved{2400, 300, 1280, 760};
    REQUIRE(confineToScreens(saved, {kPrimary, kSecond}) == saved);
}

TEST_CASE("A window is judged against the screen holding most of it", "[ui][settings]") {
    // Mostly on the second screen, hanging over its left edge onto the first.
    const Rect saved{1820, 200, 1280, 760};
    const Rect placed = confineToScreens(saved, {kPrimary, kSecond});
    // Grabbable on the screen that holds most of it, so nothing moves.
    REQUIRE(placed == saved);
}

TEST_CASE("A window with only a sliver on screen is brought back", "[ui][settings]") {
    // Twenty columns visible at the right-hand edge: enough to see, not enough
    // to grab, and not what anybody left it as.
    const Rect saved{1900, 400, 1280, 760};
    const Rect placed = confineToScreens(saved, {kPrimary});
    REQUIRE_FALSE(placed == saved);
    REQUIRE(isReachable(placed, kPrimary));
}

TEST_CASE("A window whose title bar is above the screen is brought back", "[ui][settings]") {
    // Plenty of it visible -- and on Windows it cannot be dragged, because
    // what you drag a window by is off the top of the screen.
    const Rect saved{200, -40, 1280, 760};
    REQUIRE_FALSE(isReachable(saved, kPrimary));

    const Rect placed = confineToScreens(saved, {kPrimary});
    REQUIRE(placed.y >= kPrimary.y);
    REQUIRE(isReachable(placed, kPrimary));
}

TEST_CASE("A window hanging over an edge, but grabbable, keeps where it was", "[ui][settings]") {
    // Deliberately parked half off the right of the screen is a thing people
    // do, and is not an error to correct.
    const Rect saved{1200, 100, 1280, 760};
    REQUIRE(isReachable(saved, kPrimary));
    REQUIRE(confineToScreens(saved, {kPrimary}) == saved);
}

TEST_CASE("A window saved on a bigger monitor is cut down to the one it opens on",
          "[ui][settings]") {
    const Rect saved{0, 0, 2560, 1400};
    const Rect placed = confineToScreens(saved, {kPrimary});
    REQUIRE(placed.width == kPrimary.width);
    REQUIRE(placed.height == kPrimary.height);
    REQUIRE(isReachable(placed, kPrimary));
}

TEST_CASE("A window stored as a sliver is opened at a size that can be used", "[ui][settings]") {
    const Rect placed = confineToScreens(Rect{100, 100, 3, 2}, {kPrimary});
    REQUIRE(placed.width == kMinimumWindowWidth);
    REQUIRE(placed.height == kMinimumWindowHeight);
}

TEST_CASE("A screen smaller than the smallest window does not produce a smaller one",
          "[ui][settings]") {
    // The minimum and the screen disagree here, and the minimum wins. Worth a
    // test of its own because getting the order wrong is not merely ugly:
    // clamping to a low bound above the high bound is undefined behaviour.
    const Rect tiny{0, 0, 320, 240};
    const Rect placed = confineToScreens(Rect{0, 0, 1280, 760}, {tiny});
    REQUIRE(placed.width == kMinimumWindowWidth);
    REQUIRE(placed.height == kMinimumWindowHeight);
}

TEST_CASE("A stored rectangle with no screens to check against is left alone", "[ui][settings]") {
    // Cannot happen with a window up. If it ever does, guessing a position
    // would be a guess dressed as a fix.
    REQUIRE(confineToScreens(kOnScreen, {}) == kOnScreen);
}

TEST_CASE("Screens that do not start at the origin are handled too", "[ui][settings]") {
    // A second monitor above and to the left of the primary, which is what a
    // laptop under an external display looks like on Windows.
    const Rect above{-1920, -1080, 1920, 1080};
    const Rect saved{-1800, -1000, 1280, 760};
    REQUIRE(confineToScreens(saved, {kPrimary, above}) == saved);

    const Rect lost{-9000, -9000, 1280, 760};
    const Rect placed = confineToScreens(lost, {kPrimary, above});
    REQUIRE(isReachable(placed, kPrimary));
}

// --------------------------------------------------------------------------
// Splitters
// --------------------------------------------------------------------------

TEST_CASE("Splitter sizes are kept only when there is one for every pane", "[ui][settings]") {
    REQUIRE(splitterSizesUsable({420, 230, 140}, 3));
    REQUIRE_FALSE(splitterSizesUsable({420, 230}, 3));
    REQUIRE_FALSE(splitterSizesUsable({420, 230, 140, 90}, 3));
    REQUIRE_FALSE(splitterSizesUsable({}, 3));
    REQUIRE_FALSE(splitterSizesUsable({}, 0));
}

TEST_CASE("A splitter layout with nothing in it anywhere is refused", "[ui][settings]") {
    // Every pane at zero is a window with no visible content and no handle to
    // drag, which is not a layout to restore somebody into.
    REQUIRE_FALSE(splitterSizesUsable({0, 0}, 2));
    REQUIRE(splitterSizesUsable({0, 700}, 2));
}

TEST_CASE("A splitter pane size out of all proportion is refused", "[ui][settings]") {
    REQUIRE_FALSE(splitterSizesUsable({-10, 700}, 2));
    REQUIRE_FALSE(splitterSizesUsable({2000000000, 700}, 2));
    REQUIRE_FALSE(splitterSizesUsable({kLargestPane + 1, 700}, 2));
}

TEST_CASE("A splitter list that is not integers is not a list of sizes", "[ui][settings]") {
    REQUIRE(parseIntList("420,230,140").value() == std::vector<int>{420, 230, 140});
    REQUIRE(parseIntList("420").value() == std::vector<int>{420});
    REQUIRE_FALSE(parseIntList("420,").has_value());
    REQUIRE_FALSE(parseIntList(",420").has_value());
    REQUIRE_FALSE(parseIntList("420,,230").has_value());
    REQUIRE_FALSE(parseIntList("420,two-thirty").has_value());
    REQUIRE_FALSE(parseIntList("").has_value());
}

TEST_CASE("A damaged splitter list leaves the window's own layout alone", "[ui][settings]") {
    const SavedSettings saved =
        readSettings(mapOf({{"window/mainsplit", "300,"}, {"window/sidesplit", "420,230,140"}}));
    REQUIRE(saved.mainSplit.empty());
    REQUIRE(saved.sideSplit == std::vector<int>{420, 230, 140});
}

// --------------------------------------------------------------------------
// The recent list
// --------------------------------------------------------------------------

TEST_CASE("The most recently opened file is at the top of the list", "[ui][settings]") {
    RecentFiles recent;
    recent.remember("/music/first.wav");
    recent.remember("/music/second.wav");
    recent.remember("/music/third.wav");

    REQUIRE(recent.paths().size() == 3);
    REQUIRE(recent.paths()[0] == std::filesystem::path{"/music/third.wav"});
    REQUIRE(recent.paths()[2] == std::filesystem::path{"/music/first.wav"});
}

TEST_CASE("Opening a file again moves it up rather than listing it twice", "[ui][settings]") {
    RecentFiles recent;
    recent.remember("/music/a.wav");
    recent.remember("/music/b.wav");
    recent.remember("/music/a.wav");

    REQUIRE(recent.paths().size() == 2);
    REQUIRE(recent.paths()[0] == std::filesystem::path{"/music/a.wav"});
    REQUIRE(recent.paths()[1] == std::filesystem::path{"/music/b.wav"});
}

TEST_CASE("The recent list is bounded, and drops the oldest entry", "[ui][settings]") {
    RecentFiles recent;
    for (int i = 0; i < 15; ++i) {
        recent.remember("/music/" + std::to_string(i) + ".wav");
    }
    REQUIRE(recent.size() == RecentFiles::kMost);
    REQUIRE(recent.paths().front() == std::filesystem::path{"/music/14.wav"});
    REQUIRE(recent.paths().back() == std::filesystem::path{"/music/5.wav"});
}

TEST_CASE("Two spellings of one path are one entry", "[ui][settings]") {
    RecentFiles recent;
    recent.remember("/music/a.wav");
    recent.remember("/music/./a.wav");
    recent.remember("/music/take/../a.wav");
    REQUIRE(recent.size() == 1);
}

TEST_CASE("An empty path is not an entry", "[ui][settings]") {
    RecentFiles recent;
    recent.remember({});
    REQUIRE(recent.empty());
}

TEST_CASE("A file that has actually failed to open is forgotten", "[ui][settings]") {
    RecentFiles recent;
    recent.remember("/music/a.wav");
    recent.remember("/music/b.wav");
    recent.forget("/music/./a.wav");

    REQUIRE(recent.size() == 1);
    REQUIRE(recent.paths().front() == std::filesystem::path{"/music/b.wav"});
}

TEST_CASE("A stored list full of duplicates becomes a list the menu can show", "[ui][settings]") {
    RecentFiles recent;
    recent.assign({"/music/a.wav", "/music/b.wav", "/music/a.wav", "/music/c.wav"});

    // The order it arrived in, minus the second appearance of a.wav: the first
    // one is the most recent, which is what the file's order means.
    REQUIRE(recent.size() == 3);
    REQUIRE(recent.paths()[0] == std::filesystem::path{"/music/a.wav"});
    REQUIRE(recent.paths()[1] == std::filesystem::path{"/music/b.wav"});
    REQUIRE(recent.paths()[2] == std::filesystem::path{"/music/c.wav"});
}

TEST_CASE("A recent list with a hole in it is read as the entries around it", "[ui][settings]") {
    const SavedSettings saved = readSettings(mapOf({
        {"recent/file1", "/music/a.wav"},
        {"recent/file3", "/music/c.wav"},
        {"recent/file4", ""},
        {"recent/file5", "/music/e.wav"},
    }));
    REQUIRE(saved.recent.size() == 3);
    REQUIRE(saved.recent.paths()[0] == std::filesystem::path{"/music/a.wav"});
    REQUIRE(saved.recent.paths()[1] == std::filesystem::path{"/music/c.wav"});
    REQUIRE(saved.recent.paths()[2] == std::filesystem::path{"/music/e.wav"});
}

TEST_CASE("A file more than ten deep in a hand-edited list is not read", "[ui][settings]") {
    std::vector<std::pair<std::string, std::string>> entries;
    for (int i = 1; i <= 20; ++i) {
        entries.emplace_back("recent/file" + std::to_string(i),
                             "/music/" + std::to_string(i) + ".wav");
    }
    const SavedSettings saved = readSettings(mapOf(entries));
    REQUIRE(saved.recent.size() == RecentFiles::kMost);
    REQUIRE(saved.recent.paths().front() == std::filesystem::path{"/music/1.wav"});
}

TEST_CASE("A path outside ASCII survives the round trip", "[ui][settings]") {
    // The settings file is UTF-8 whatever the platform's own 8-bit encoding
    // is, so a recording whose name is not in the active code page is still
    // the same file next launch.
    const std::filesystem::path awkward{std::u8string{u8"/musique/prise numéro 2 — château.wav"}};
    REQUIRE(pathFromUtf8(toUtf8(awkward)) == awkward);

    SavedSettings wanted;
    wanted.recent.remember(awkward);
    const SavedSettings back = readSettings(writeSettings(wanted));
    REQUIRE(back.recent.paths().size() == 1);
    REQUIRE(back.recent.paths().front() == awkward);
}

// --------------------------------------------------------------------------
// Preferences that have to make sense together
// --------------------------------------------------------------------------

TEST_CASE("An FFT size that is not a power of two is refused", "[ui][settings]") {
    Preferences preferences;
    preferences.fftSize = 3000;
    REQUIRE(validated(preferences).fftSize == 4096);

    preferences.fftSize = 128;
    REQUIRE(validated(preferences).fftSize == 4096);

    preferences.fftSize = 65536;
    REQUIRE(validated(preferences).fftSize == 4096);

    preferences.fftSize = 1024;
    REQUIRE(validated(preferences).fftSize == 1024);
}

TEST_CASE("A hop that does not fit its window falls back to a quarter of it", "[ui][settings]") {
    Preferences preferences;
    preferences.fftSize = 2048;

    preferences.hopSize = 4096; // Bigger than the window: gaps in the analysis.
    REQUIRE(validated(preferences).hopSize == 512);

    preferences.hopSize = 64; // A thirty-second: thirty-two transforms a frame.
    REQUIRE(validated(preferences).hopSize == 512);

    preferences.hopSize = 300; // Not a power of two.
    REQUIRE(validated(preferences).hopSize == 512);

    preferences.hopSize = 256;
    REQUIRE(validated(preferences).hopSize == 256);
}

TEST_CASE("A dynamic range the menu does not offer is refused", "[ui][settings]") {
    Preferences preferences;
    preferences.spectrogramFloorDb = -73.0;
    REQUIRE(validated(preferences).spectrogramFloorDb == -96.0);

    preferences.spectrogramFloorDb = -120.0;
    REQUIRE(validated(preferences).spectrogramFloorDb == -120.0);
}

TEST_CASE("The key bound cannot ask for more audio than is read", "[ui][settings]") {
    Preferences preferences;
    preferences.analysisSeconds = 30.0;
    preferences.keySeconds = 600.0;
    REQUIRE(validated(preferences).keySeconds == 30.0);
}

TEST_CASE("The analysis bounds are held inside what the panel can do", "[ui][settings]") {
    Preferences preferences;
    preferences.analysisSeconds = 0.0;
    REQUIRE(validated(preferences).analysisSeconds == 5.0);

    preferences.analysisSeconds = 100000.0;
    REQUIRE(validated(preferences).analysisSeconds == 3600.0);

    preferences.analysisSeconds = 120.0;
    preferences.keySeconds = 0.5;
    REQUIRE(validated(preferences).keySeconds == 5.0);
}

TEST_CASE("An enumeration that is not one of its own values falls back", "[ui][settings]") {
    Preferences preferences;
    // What a cast from a number produces, which is what a caller building this
    // struct by hand could hand over.
    preferences.colourmap = static_cast<Colourmap>(99);
    preferences.exportFormat = io::SampleFormat::Float64;
    preferences.dither = static_cast<dsp::DitherType>(-1);

    const Preferences fixed = validated(preferences);
    REQUIRE(fixed.colourmap == Colourmap::Magma);
    REQUIRE(fixed.exportFormat == io::SampleFormat::PcmInt24);
    REQUIRE(fixed.dither == dsp::DitherType::Tpdf);
}

TEST_CASE("Every stored name is one the reader knows", "[ui][settings]") {
    // The two directions have to agree, or a setting writes itself out and
    // never comes back. Checked for every value of every enumeration rather
    // than for the ones that happened to be typed into a test.
    for (const io::SampleFormat format :
         {io::SampleFormat::PcmInt16, io::SampleFormat::PcmInt24, io::SampleFormat::Float32}) {
        REQUIRE(sampleFormatFromName(toName(format)) == format);
    }
    for (const dsp::DitherType dither :
         {dsp::DitherType::None, dsp::DitherType::Tpdf, dsp::DitherType::TpdfNoiseShaped}) {
        REQUIRE(ditherFromName(toName(dither)) == dither);
    }
    for (const engine::FadeShape shape :
         {engine::FadeShape::Linear, engine::FadeShape::EqualPower, engine::FadeShape::Logarithmic,
          engine::FadeShape::Exponential, engine::FadeShape::SCurve}) {
        REQUIRE(fadeShapeFromName(toName(shape)) == shape);
    }
    for (const FrequencyScale scale : {FrequencyScale::Logarithmic, FrequencyScale::Linear}) {
        REQUIRE(frequencyScaleFromName(toName(scale)) == scale);
    }
    for (const Colourmap map : {Colourmap::Magma, Colourmap::Viridis, Colourmap::Grey}) {
        REQUIRE(colourmapFromName(toName(map)) == map);
    }
    for (std::size_t i = 0; i < analysis::targetCount(); ++i) {
        const analysis::LoudnessPlatform platform = analysis::allTargets()[i].platform;
        REQUIRE_FALSE(toName(platform).empty());
        REQUIRE(loudnessPlatformFromName(toName(platform)) == platform);
    }
}

// --------------------------------------------------------------------------
// Where the file goes
// --------------------------------------------------------------------------

TEST_CASE("Settings go to the per-user file by default", "[ui][settings]") {
    const SettingsFile chosen =
        chooseSettingsFile("/opt/auscultate/Auscultate.ini", false, "/home/me/.config/x.ini");
    REQUIRE(chosen.home == SettingsHome::PerUser);
    REQUIRE(chosen.path == std::filesystem::path{"/home/me/.config/x.ini"});
}

TEST_CASE("A settings file already beside the executable is the portable case", "[ui][settings]") {
    // Opted into by being there: no switch to discover, and creating an empty
    // file is something a person can do from a file manager on a stick.
    const SettingsFile chosen = chooseSettingsFile("/media/stick/auscultate/Auscultate.ini", true,
                                                   "/home/me/.config/x.ini");
    REQUIRE(chosen.home == SettingsHome::Portable);
    REQUIRE(chosen.path == std::filesystem::path{"/media/stick/auscultate/Auscultate.ini"});
}
