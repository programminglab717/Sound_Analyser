#include <sa/ui/Settings.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string>
#include <system_error>
#include <utility>

namespace sa::ui {

namespace {

/// Keys. All lower case, because QSettings is case-insensitive about keys on
/// Windows and case-sensitive elsewhere, and a file written on one and read on
/// the other has to be the same file.
constexpr std::string_view kVersionKey = "version";
constexpr std::string_view kWindowX = "window/x";
constexpr std::string_view kWindowY = "window/y";
constexpr std::string_view kWindowWidth = "window/width";
constexpr std::string_view kWindowHeight = "window/height";
constexpr std::string_view kWindowMaximised = "window/maximised";
constexpr std::string_view kMainSplit = "window/mainsplit";
constexpr std::string_view kSideSplit = "window/sidesplit";
constexpr std::string_view kRecentPrefix = "recent/file";

/// A whole-string parse: "1280px" is not a number, and neither is "".
template <typename T>
[[nodiscard]] std::optional<T> parseNumber(std::string_view text) {
    // Leading and trailing spaces are the one liberty taken, because a person
    // editing the file by hand leaves them and means nothing by it.
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }

    T value{};
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto [stopped, code] = std::from_chars(first, last, value);
    if (code != std::errc{} || stopped != last) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] bool isPowerOfTwo(int value) noexcept {
    return value > 0 && (value & (value - 1)) == 0;
}

/// A name that matches whatever case it was stored in.
///
/// Case-insensitive because the file is meant to be editable by hand, and
/// somebody typing "Magma" has said which colour map they want.
[[nodiscard]] bool sameName(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char lowerA = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] + 32) : a[i];
        const char lowerB = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] + 32) : b[i];
        if (lowerA != lowerB) {
            return false;
        }
    }
    return true;
}

/// One entry of a name table.
template <typename Enum>
struct Named {
    Enum value;
    std::string_view name;
};

template <typename Enum, std::size_t N>
[[nodiscard]] std::string_view nameOf(const Named<Enum> (&table)[N], Enum value) noexcept {
    for (const Named<Enum>& entry : table) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return {};
}

template <typename Enum, std::size_t N>
[[nodiscard]] std::optional<Enum> valueOf(const Named<Enum> (&table)[N],
                                          std::string_view name) noexcept {
    for (const Named<Enum>& entry : table) {
        if (sameName(entry.name, name)) {
            return entry.value;
        }
    }
    return std::nullopt;
}

// The same words the --apply verbs use: format:16, dither:shaped, fadein:scurve.
constexpr Named<io::SampleFormat> kFormats[] = {
    {io::SampleFormat::PcmInt16, "16"},
    {io::SampleFormat::PcmInt24, "24"},
    {io::SampleFormat::Float32, "float"},
};

constexpr Named<dsp::DitherType> kDithers[] = {
    {dsp::DitherType::None, "none"},
    {dsp::DitherType::Tpdf, "tpdf"},
    {dsp::DitherType::TpdfNoiseShaped, "shaped"},
};

constexpr Named<engine::FadeShape> kFades[] = {
    {engine::FadeShape::Linear, "linear"},
    {engine::FadeShape::EqualPower, "equalpower"},
    {engine::FadeShape::Logarithmic, "logarithmic"},
    {engine::FadeShape::Exponential, "exponential"},
    {engine::FadeShape::SCurve, "scurve"},
};

constexpr Named<FrequencyScale> kScales[] = {
    {FrequencyScale::Logarithmic, "logarithmic"},
    {FrequencyScale::Linear, "linear"},
};

constexpr Named<Colourmap> kColourmaps[] = {
    {Colourmap::Magma, "magma"},
    {Colourmap::Viridis, "viridis"},
    {Colourmap::Grey, "grey"},
};

/// Short, file-safe names rather than analysis::toString's display names, which
/// have spaces in them ("EBU R128", "Apple Music"). A settings value is read by
/// a parser before it is read by a person.
constexpr Named<analysis::LoudnessPlatform> kPlatforms[] = {
    {analysis::LoudnessPlatform::Spotify, "spotify"},
    {analysis::LoudnessPlatform::AppleMusic, "applemusic"},
    {analysis::LoudnessPlatform::YouTube, "youtube"},
    {analysis::LoudnessPlatform::AmazonMusic, "amazonmusic"},
    {analysis::LoudnessPlatform::Tidal, "tidal"},
    {analysis::LoudnessPlatform::Podcast, "podcast"},
    {analysis::LoudnessPlatform::EbuR128, "ebur128"},
    {analysis::LoudnessPlatform::AtscA85, "atsca85"},
};

/// Read a named enumeration, falling back to what the caller already has.
template <typename Enum, std::size_t N>
[[nodiscard]] Enum readName(const SettingsMap& stored, std::string_view key,
                            const Named<Enum> (&table)[N], Enum fallback) {
    const std::optional<std::string_view> text = stored.text(key);
    if (!text) {
        return fallback;
    }
    return valueOf(table, *text).value_or(fallback);
}

} // namespace

void SettingsMap::set(std::string key, std::string value) {
    entries_[std::move(key)] = std::move(value);
}

void SettingsMap::setInt(std::string key, int value) {
    set(std::move(key), std::to_string(value));
}

void SettingsMap::setBool(std::string key, bool value) {
    set(std::move(key), value ? "true" : "false");
}

void SettingsMap::setDouble(std::string key, double value) {
    // Shortest representation that reads back as the same double, so a stored
    // -96 is "-96" and not "-96.000000".
    char buffer[40]{};
    const auto [stopped, code] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (code != std::errc{}) {
        set(std::move(key), "0");
        return;
    }
    set(std::move(key), std::string{buffer, static_cast<std::size_t>(stopped - buffer)});
}

bool SettingsMap::has(std::string_view key) const {
    return entries_.find(key) != entries_.end();
}

std::optional<std::string_view> SettingsMap::text(std::string_view key) const {
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return std::nullopt;
    }
    return std::string_view{found->second};
}

int SettingsMap::intOr(std::string_view key, int fallback) const {
    const std::optional<std::string_view> raw = text(key);
    if (!raw) {
        return fallback;
    }
    return parseNumber<int>(*raw).value_or(fallback);
}

bool SettingsMap::boolOr(std::string_view key, bool fallback) const {
    const std::optional<std::string_view> raw = text(key);
    if (!raw) {
        return fallback;
    }
    // "1" and "0" as well as the words, because a hand-edited file has them and
    // the writer is not the only thing that has to be understood here.
    if (sameName(*raw, "true") || sameName(*raw, "1")) {
        return true;
    }
    if (sameName(*raw, "false") || sameName(*raw, "0")) {
        return false;
    }
    return fallback;
}

double SettingsMap::doubleOr(std::string_view key, double fallback) const {
    const std::optional<std::string_view> raw = text(key);
    if (!raw) {
        return fallback;
    }
    const std::optional<double> value = parseNumber<double>(*raw);
    // A stored infinity or NaN is not a setting, whatever the parser made of
    // the text: every number here goes on to be arithmetic in a layout.
    if (!value || !std::isfinite(*value)) {
        return fallback;
    }
    return *value;
}

Rect intersection(const Rect& a, const Rect& b) noexcept {
    const int left = std::max(a.x, b.x);
    const int top = std::max(a.y, b.y);
    const int right = std::min(a.right(), b.right());
    const int bottom = std::min(a.bottom(), b.bottom());
    if (right <= left || bottom <= top) {
        return Rect{left, top, 0, 0};
    }
    return Rect{left, top, right - left, bottom - top};
}

std::int64_t area(const Rect& rect) noexcept {
    if (rect.isEmpty()) {
        return 0;
    }
    return static_cast<std::int64_t>(rect.width) * static_cast<std::int64_t>(rect.height);
}

bool isReachable(const Rect& window, const Rect& screen) noexcept {
    const Rect shown = intersection(window, screen);
    if (shown.width < kGrabbableWidth || shown.height < kGrabbableHeight) {
        return false;
    }
    // The top edge specifically: a window whose title bar is above the top of
    // the screen has plenty of itself visible and still cannot be dragged.
    return window.y >= screen.y;
}

Rect confineToScreens(const Rect& saved, const std::vector<Rect>& screens) noexcept {
    if (screens.empty()) {
        return saved;
    }

    const Rect* best = &screens.front();
    std::int64_t most = 0;
    for (const Rect& screen : screens) {
        const std::int64_t shown = area(intersection(saved, screen));
        if (shown > most) {
            most = shown;
            best = &screen;
        }
    }

    // The minimum wins over the screen when the two disagree, which they do on
    // a screen smaller than the minimum window. Not merely a taste: std::clamp
    // with a low bound above its high bound is undefined behaviour, so the
    // order here is load-bearing.
    Rect placed = saved;
    placed.width =
        std::clamp(placed.width, kMinimumWindowWidth, std::max(best->width, kMinimumWindowWidth));
    placed.height = std::clamp(placed.height, kMinimumWindowHeight,
                               std::max(best->height, kMinimumWindowHeight));

    if (isReachable(placed, *best)) {
        return placed;
    }

    placed.x = best->x + (best->width - placed.width) / 2;
    placed.y = best->y + (best->height - placed.height) / 2;
    return placed;
}

bool splitterSizesUsable(const std::vector<int>& sizes, std::size_t panes) noexcept {
    if (sizes.size() != panes || panes == 0) {
        return false;
    }
    std::int64_t total = 0;
    for (const int size : sizes) {
        if (size < 0 || size > kLargestPane) {
            return false;
        }
        total += size;
    }
    // All zeroes is a splitter with nothing in it, which is a layout nobody
    // can recover from with a mouse.
    return total > 0;
}

std::optional<std::vector<int>> parseIntList(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::vector<int> values;
    std::size_t at = 0;
    while (true) {
        const std::size_t comma = text.find(',', at);
        const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
        // A leading, trailing or doubled comma leaves an empty field here,
        // which parseNumber refuses along with everything else that is not a
        // whole number.
        const std::optional<int> value = parseNumber<int>(text.substr(at, end - at));
        if (!value) {
            return std::nullopt;
        }
        values.push_back(*value);
        if (comma == std::string_view::npos) {
            return values;
        }
        at = comma + 1;
    }
}

std::string formatIntList(const std::vector<int>& values) {
    std::string text;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            text += ',';
        }
        text += std::to_string(values[i]);
    }
    return text;
}

void RecentFiles::remember(const std::filesystem::path& path) {
    if (path.empty()) {
        return;
    }
    const std::filesystem::path tidied = path.lexically_normal();
    forget(tidied);
    paths_.insert(paths_.begin(), tidied);
    if (paths_.size() > kMost) {
        paths_.resize(kMost);
    }
}

void RecentFiles::forget(const std::filesystem::path& path) {
    const std::string wanted = toUtf8(path.lexically_normal());
    std::erase_if(paths_, [&wanted](const std::filesystem::path& held) {
        return toUtf8(held.lexically_normal()) == wanted;
    });
}

void RecentFiles::assign(const std::vector<std::filesystem::path>& paths) {
    paths_.clear();
    // Back to front, so remember()'s "most recent first" leaves the list in the
    // order it arrived rather than reversed.
    for (auto entry = paths.rbegin(); entry != paths.rend(); ++entry) {
        remember(*entry);
    }
}

Preferences validated(Preferences preferences) noexcept {
    const Preferences fallback;

    if (!isPowerOfTwo(preferences.fftSize) || preferences.fftSize < kSmallestFftSize ||
        preferences.fftSize > kLargestFftSize) {
        preferences.fftSize = fallback.fftSize;
    }
    // A hop bigger than the window leaves gaps in the analysis, and one below
    // a sixteenth of it costs sixteen transforms per frame of picture.
    if (!isPowerOfTwo(preferences.hopSize) || preferences.hopSize > preferences.fftSize ||
        preferences.hopSize < preferences.fftSize / 16) {
        preferences.hopSize = std::max(1, preferences.fftSize / 4);
    }

    bool offered = false;
    for (const double floorDb : kFloorChoicesDb) {
        offered = offered || floorDb == preferences.spectrogramFloorDb;
    }
    if (!offered) {
        preferences.spectrogramFloorDb = fallback.spectrogramFloorDb;
    }

    if (!std::isfinite(preferences.analysisSeconds)) {
        preferences.analysisSeconds = fallback.analysisSeconds;
    }
    preferences.analysisSeconds = std::clamp(preferences.analysisSeconds, 5.0, 3600.0);
    if (!std::isfinite(preferences.keySeconds)) {
        preferences.keySeconds = fallback.keySeconds;
    }
    // The key is taken from the front of the same read, so it cannot ask for
    // more audio than the read itself covers.
    preferences.keySeconds = std::clamp(preferences.keySeconds, 5.0, preferences.analysisSeconds);

    // An enumeration that is not one of its own values -- a cast from a number,
    // which is what a caller building this struct by hand could produce --
    // falls back rather than being passed on to a switch that has no case for
    // it.
    if (nameOf(kFormats, preferences.exportFormat).empty()) {
        preferences.exportFormat = fallback.exportFormat;
    }
    if (nameOf(kDithers, preferences.dither).empty()) {
        preferences.dither = fallback.dither;
    }
    if (nameOf(kFades, preferences.fadeShape).empty()) {
        preferences.fadeShape = fallback.fadeShape;
    }
    if (nameOf(kPlatforms, preferences.loudnessTarget).empty()) {
        preferences.loudnessTarget = fallback.loudnessTarget;
    }
    if (nameOf(kScales, preferences.frequencyScale).empty()) {
        preferences.frequencyScale = fallback.frequencyScale;
    }
    if (nameOf(kColourmaps, preferences.colourmap).empty()) {
        preferences.colourmap = fallback.colourmap;
    }
    return preferences;
}

SavedSettings readSettings(const SettingsMap& stored) {
    SavedSettings settings;
    settings.writeBack = stored.intOr(kVersionKey, kSettingsVersion) <= kSettingsVersion;

    // All four or none: half a rectangle is not a position, and defaulting the
    // missing half would put the window somewhere nobody left it.
    if (stored.has(kWindowX) && stored.has(kWindowY) && stored.has(kWindowWidth) &&
        stored.has(kWindowHeight)) {
        WindowPlacement placement;
        placement.frame.x = stored.intOr(kWindowX, 0);
        placement.frame.y = stored.intOr(kWindowY, 0);
        placement.frame.width = stored.intOr(kWindowWidth, kDefaultWindowWidth);
        placement.frame.height = stored.intOr(kWindowHeight, kDefaultWindowHeight);
        placement.maximised = stored.boolOr(kWindowMaximised, false);
        settings.window = placement;
    }

    if (const std::optional<std::string_view> text = stored.text(kMainSplit); text) {
        settings.mainSplit = parseIntList(*text).value_or(std::vector<int>{});
    }
    if (const std::optional<std::string_view> text = stored.text(kSideSplit); text) {
        settings.sideSplit = parseIntList(*text).value_or(std::vector<int>{});
    }

    std::vector<std::filesystem::path> recent;
    for (std::size_t i = 1; i <= RecentFiles::kMost; ++i) {
        const std::string key = std::string{kRecentPrefix} + std::to_string(i);
        const std::optional<std::string_view> text = stored.text(key);
        if (!text || text->empty()) {
            // A gap rather than a stop: a file edited by hand can be missing
            // file3 and still mean the seven entries around it.
            continue;
        }
        recent.push_back(pathFromUtf8(*text));
    }
    settings.recent.assign(recent);

    Preferences preferences;
    preferences.exportFormat =
        readName(stored, "preferences/exportformat", kFormats, preferences.exportFormat);
    preferences.dither = readName(stored, "preferences/dither", kDithers, preferences.dither);
    preferences.fadeShape =
        readName(stored, "preferences/fadeshape", kFades, preferences.fadeShape);
    preferences.loudnessTarget =
        readName(stored, "preferences/loudnesstarget", kPlatforms, preferences.loudnessTarget);
    preferences.frequencyScale =
        readName(stored, "preferences/frequencyscale", kScales, preferences.frequencyScale);
    preferences.colourmap =
        readName(stored, "preferences/colourmap", kColourmaps, preferences.colourmap);

    preferences.fftSize = stored.intOr("preferences/fftsize", preferences.fftSize);
    preferences.hopSize = stored.intOr("preferences/hopsize", preferences.hopSize);
    preferences.spectrogramFloorDb =
        stored.doubleOr("preferences/spectrogramfloordb", preferences.spectrogramFloorDb);
    preferences.showBeatGrid = stored.boolOr("preferences/beatgrid", preferences.showBeatGrid);
    preferences.showPitchContour =
        stored.boolOr("preferences/pitchcontour", preferences.showPitchContour);
    preferences.showOctaveBands =
        stored.boolOr("preferences/octavebands", preferences.showOctaveBands);
    preferences.measureRoom = stored.boolOr("preferences/room", preferences.measureRoom);
    preferences.analysisSeconds =
        stored.doubleOr("preferences/analysisseconds", preferences.analysisSeconds);
    preferences.keySeconds = stored.doubleOr("preferences/keyseconds", preferences.keySeconds);

    settings.preferences = validated(preferences);
    return settings;
}

SettingsMap writeSettings(const SavedSettings& settings) {
    SettingsMap stored;
    stored.setInt(std::string{kVersionKey}, kSettingsVersion);

    if (settings.window) {
        stored.setInt(std::string{kWindowX}, settings.window->frame.x);
        stored.setInt(std::string{kWindowY}, settings.window->frame.y);
        stored.setInt(std::string{kWindowWidth}, settings.window->frame.width);
        stored.setInt(std::string{kWindowHeight}, settings.window->frame.height);
        stored.setBool(std::string{kWindowMaximised}, settings.window->maximised);
    }
    if (!settings.mainSplit.empty()) {
        stored.set(std::string{kMainSplit}, formatIntList(settings.mainSplit));
    }
    if (!settings.sideSplit.empty()) {
        stored.set(std::string{kSideSplit}, formatIntList(settings.sideSplit));
    }

    std::size_t index = 1;
    for (const std::filesystem::path& path : settings.recent.paths()) {
        stored.set(std::string{kRecentPrefix} + std::to_string(index), toUtf8(path));
        ++index;
    }

    const Preferences preferences = validated(settings.preferences);
    stored.set("preferences/exportformat", std::string{toName(preferences.exportFormat)});
    stored.set("preferences/dither", std::string{toName(preferences.dither)});
    stored.set("preferences/fadeshape", std::string{toName(preferences.fadeShape)});
    stored.set("preferences/loudnesstarget", std::string{toName(preferences.loudnessTarget)});
    stored.set("preferences/frequencyscale", std::string{toName(preferences.frequencyScale)});
    stored.set("preferences/colourmap", std::string{toName(preferences.colourmap)});
    stored.setInt("preferences/fftsize", preferences.fftSize);
    stored.setInt("preferences/hopsize", preferences.hopSize);
    stored.setDouble("preferences/spectrogramfloordb", preferences.spectrogramFloorDb);
    stored.setBool("preferences/beatgrid", preferences.showBeatGrid);
    stored.setBool("preferences/pitchcontour", preferences.showPitchContour);
    stored.setBool("preferences/octavebands", preferences.showOctaveBands);
    stored.setBool("preferences/room", preferences.measureRoom);
    stored.setDouble("preferences/analysisseconds", preferences.analysisSeconds);
    stored.setDouble("preferences/keyseconds", preferences.keySeconds);
    return stored;
}

SettingsFile chooseSettingsFile(const std::filesystem::path& besideExecutable,
                                bool besideExecutableExists, const std::filesystem::path& perUser) {
    if (besideExecutableExists && !besideExecutable.empty()) {
        return SettingsFile{besideExecutable, SettingsHome::Portable};
    }
    return SettingsFile{perUser, SettingsHome::PerUser};
}

std::string_view toName(io::SampleFormat format) noexcept {
    return nameOf(kFormats, format);
}

std::optional<io::SampleFormat> sampleFormatFromName(std::string_view name) noexcept {
    return valueOf(kFormats, name);
}

std::string_view toName(dsp::DitherType dither) noexcept {
    return nameOf(kDithers, dither);
}

std::optional<dsp::DitherType> ditherFromName(std::string_view name) noexcept {
    return valueOf(kDithers, name);
}

std::string_view toName(engine::FadeShape shape) noexcept {
    return nameOf(kFades, shape);
}

std::optional<engine::FadeShape> fadeShapeFromName(std::string_view name) noexcept {
    return valueOf(kFades, name);
}

std::string_view toName(FrequencyScale scale) noexcept {
    return nameOf(kScales, scale);
}

std::optional<FrequencyScale> frequencyScaleFromName(std::string_view name) noexcept {
    return valueOf(kScales, name);
}

std::string_view toName(Colourmap map) noexcept {
    return nameOf(kColourmaps, map);
}

std::optional<Colourmap> colourmapFromName(std::string_view name) noexcept {
    return valueOf(kColourmaps, name);
}

std::string_view toName(analysis::LoudnessPlatform platform) noexcept {
    return nameOf(kPlatforms, platform);
}

std::optional<analysis::LoudnessPlatform> loudnessPlatformFromName(std::string_view name) noexcept {
    return valueOf(kPlatforms, name);
}

std::string toUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    std::string out;
    out.reserve(text.size());
    for (const char8_t unit : text) {
        out.push_back(static_cast<char>(unit));
    }
    return out;
}

std::filesystem::path pathFromUtf8(std::string_view text) {
    std::u8string units;
    units.reserve(text.size());
    for (const char unit : text) {
        units.push_back(static_cast<char8_t>(unit));
    }
    return std::filesystem::path{units};
}

} // namespace sa::ui
