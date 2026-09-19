#pragma once

#include <sa/analysis/ComplianceTarget.h>
#include <sa/dsp/Dither.h>
#include <sa/engine/Clip.h>
#include <sa/io/AudioFileInfo.h>
#include <sa/ui/ViewGeometry.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sa::ui {

/// What the settings file is, and what it is deliberately not.
///
/// Everything in this header is arithmetic and string handling over a flat
/// key/value map. There is no QSettings in it, because a stored geometry that
/// would open the window on a monitor nobody has any more is a *decision*, and
/// a decision that can only be exercised by opening a window is a decision
/// nobody checks. SettingsStore.h is the twenty lines that move this map in and
/// out of a QSettings file; everything worth being wrong about is here, where a
/// test can call it.

/// The schema this build writes and understands.
///
/// Bumped only when a key changes meaning, not when one is added: an unknown
/// key is ignored and a missing key falls back to its default, so adding a
/// setting is already backward and forward compatible. See readSettings for
/// what happens when the file says a *higher* number than this.
inline constexpr int kSettingsVersion = 1;

/// A flat key/value store: the shape QSettings has, without QSettings in it.
///
/// Values are strings because that is what an INI file holds. Every conversion
/// out of one is fallible and every fallible conversion here answers with the
/// default rather than with an exception, which is the whole behaviour being
/// asked for -- a settings file is untrusted input that the user may have
/// edited, that half a disk write may have truncated, and that a newer build
/// may have written.
class SettingsMap {
public:
    using Entries = std::map<std::string, std::string, std::less<>>;

    void set(std::string key, std::string value);
    void setInt(std::string key, int value);
    void setBool(std::string key, bool value);
    /// Written with enough digits to read back as the same double.
    void setDouble(std::string key, double value);

    [[nodiscard]] bool has(std::string_view key) const;

    /// The raw text, or nothing when the key is absent.
    [[nodiscard]] std::optional<std::string_view> text(std::string_view key) const;

    /// Typed reads. Each one answers `fallback` when the key is absent, when
    /// the text is not a number of that kind, or when it has trailing rubbish
    /// after one -- "1280px" is not 1280.
    [[nodiscard]] int intOr(std::string_view key, int fallback) const;
    [[nodiscard]] bool boolOr(std::string_view key, bool fallback) const;
    [[nodiscard]] double doubleOr(std::string_view key, double fallback) const;

    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

    [[nodiscard]] const Entries& entries() const noexcept { return entries_; }

private:
    Entries entries_;
};

/// A rectangle in the virtual desktop's coordinates, in pixels.
struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    [[nodiscard]] int right() const noexcept { return x + width; }

    [[nodiscard]] int bottom() const noexcept { return y + height; }

    [[nodiscard]] bool isEmpty() const noexcept { return width <= 0 || height <= 0; }

    [[nodiscard]] friend bool operator==(const Rect& a, const Rect& b) noexcept {
        return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    }
};

/// The overlap of two rectangles, or an empty one where they do not meet.
[[nodiscard]] Rect intersection(const Rect& a, const Rect& b) noexcept;

/// Area in pixels, as a 64-bit count: two 32-bit edges multiply out of an int.
[[nodiscard]] std::int64_t area(const Rect& rect) noexcept;

/// The window as it opens the first time, before anything has been saved.
inline constexpr int kDefaultWindowWidth = 1280;
inline constexpr int kDefaultWindowHeight = 760;

/// The smallest window a restore will produce.
///
/// Not a minimum the user is held to while dragging -- the layout's own
/// minimums do that -- but a floor on what a *stored* number may ask for, so a
/// truncated or hand-edited file cannot open the application as a 3-pixel
/// sliver that has to be found with the mouse before it can be resized.
inline constexpr int kMinimumWindowWidth = 480;
inline constexpr int kMinimumWindowHeight = 320;

/// How much of the window has to be on a screen for the window to count as
/// reachable: enough to see and enough to grab.
///
/// Height is about the title bar specifically. A window whose title bar is
/// above the top of the screen cannot be dragged back down on Windows, which
/// is the shape this rule exists to rule out.
inline constexpr int kGrabbableWidth = 160;
inline constexpr int kGrabbableHeight = 48;

/// Where the window was and how it was shown.
///
/// `frame` is the *normal* geometry even when `maximised` is set, which is what
/// makes un-maximising land the window back where it was rather than on some
/// remembered full-screen rectangle.
struct WindowPlacement {
    Rect frame{0, 0, kDefaultWindowWidth, kDefaultWindowHeight};
    bool maximised = false;
};

/// Bring a saved window rectangle back onto the screens that are attached now.
///
/// This is the bug that ships in a surprising number of applications: a window
/// saved on the second monitor of a docked laptop, reopened on the train,
/// arrives at x = 2560 and is never seen again. The user's only remedy is to
/// know where the settings live and edit them, which is not a remedy.
///
/// The rules, in order:
///   * No screens at all -- which cannot happen with a GUI up, but this is
///     called with whatever the platform reports -- leaves the rectangle
///     alone. There is nothing here to be off the edge of, and inventing a
///     position would be a guess dressed up as a fix.
///   * The screen holding the largest part of the window is the one it belongs
///     to. Nothing overlapping any screen falls back to the first, which is
///     the primary.
///   * The size is clamped into [kMinimumWindow..., that screen's size], so a
///     window saved on a 4K monitor does not open larger than the laptop panel
///     it is reopened on. On a screen smaller than the minimum the two
///     disagree, and the minimum wins.
///   * A window with a grabbable amount of itself on that screen, title bar
///     included, keeps the position it was saved with -- deliberately hanging
///     a window over an edge is a thing people do and is not an error to
///     correct.
///   * Anything else is centred on that screen.
[[nodiscard]] Rect confineToScreens(const Rect& saved, const std::vector<Rect>& screens) noexcept;

/// Whether enough of `window` sits on `screen` to see it and drag it.
[[nodiscard]] bool isReachable(const Rect& window, const Rect& screen) noexcept;

/// Largest pane size a stored splitter may ask for.
///
/// A splitter hands its widgets whatever it is told, so a corrupt "2000000000"
/// is a layout with one visible pane and no way back. Far above any real
/// screen, far below the range where the arithmetic stops being sensible.
inline constexpr int kLargestPane = 100000;

/// Whether a stored list of splitter sizes can be used for a splitter with
/// `panes` widgets in it.
///
/// All or nothing rather than per-pane repair: a splitter's sizes only mean
/// anything together, and a list that has lost one of them is not a layout
/// anybody chose. The defaults in MainWindow are good; falling back to them is
/// not a loss worth a rescue attempt.
[[nodiscard]] bool splitterSizesUsable(const std::vector<int>& sizes, std::size_t panes) noexcept;

/// Parse "420,230,140". Nothing on anything else, including an empty string,
/// a trailing comma or a value that is not an integer.
[[nodiscard]] std::optional<std::vector<int>> parseIntList(std::string_view text);

[[nodiscard]] std::string formatIntList(const std::vector<int>& values);

/// The File menu's recent list: most recent first, deduplicated, bounded.
class RecentFiles {
public:
    /// Ten, which is what every other editor offers. Long enough to cover a
    /// session's worth of files, short enough that the menu is still a list
    /// somebody can read rather than a history to search.
    static constexpr std::size_t kMost = 10;

    /// Put `path` at the top, removing any earlier appearance of it.
    ///
    /// Paths are compared after lexical normalisation, so "a/./b" and "a/b" are
    /// one entry. They are *not* compared case-insensitively on Windows, where
    /// two spellings of one file would appear twice: deciding that properly
    /// means asking the file system whether two paths are the same file, which
    /// is a disk hit per entry and fails outright for a file that is not there
    /// -- and a file that is not there is precisely what this list is full of.
    void remember(const std::filesystem::path& path);

    /// Drop an entry. What happens when opening one actually fails, which is
    /// the only evidence worth acting on.
    void forget(const std::filesystem::path& path);

    /// Replace the list, applying the same normalisation, deduplication and
    /// bound as remember(). This is how a stored list arrives, so a file that
    /// has been hand-edited into twenty duplicate entries becomes a list the
    /// menu can show.
    void assign(const std::vector<std::filesystem::path>& paths);

    void clear() noexcept { paths_.clear(); }

    [[nodiscard]] const std::vector<std::filesystem::path>& paths() const noexcept {
        return paths_;
    }

    [[nodiscard]] bool empty() const noexcept { return paths_.empty(); }

    [[nodiscard]] std::size_t size() const noexcept { return paths_.size(); }

private:
    std::vector<std::filesystem::path> paths_;
};

/// The dynamic-range floors the View menu offers, in dB.
///
/// A stored floor has to be one of these rather than merely be in range: the
/// menu is a group of four radio entries, and a restored -73 would tick none of
/// them and leave the menu saying something the display is not doing.
inline constexpr double kFloorChoicesDb[] = {-60.0, -80.0, -96.0, -120.0};

/// Settings the window used to hard-code, with the values it hard-coded.
///
/// Every default here is the value the code had before this struct existed, so
/// a first run with no file is the application exactly as it was.
struct Preferences {
    /// What an export is written as, and what is done about the bits it drops.
    io::SampleFormat exportFormat = io::SampleFormat::PcmInt24;
    dsp::DitherType dither = dsp::DitherType::Tpdf;

    /// Which curve Fade in and Fade out use.
    engine::FadeShape fadeShape = engine::FadeShape::Linear;

    /// The delivery specification the meters judge against.
    analysis::LoudnessPlatform loudnessTarget = analysis::LoudnessPlatform::EbuR128;

    /// Display analysis settings. 4096 at 48 kHz is an 11.7 Hz bin and a 21 ms
    /// hop; see displayConfig() in MainWindow.cpp for why that pair.
    int fftSize = 4096;
    int hopSize = 1024;

    /// Bottom of the spectrogram's displayed range.
    double spectrogramFloorDb = -96.0;

    FrequencyScale frequencyScale = FrequencyScale::Logarithmic;
    Colourmap colourmap = Colourmap::Magma;

    /// The four analysis overlays, and whether each is wanted.
    bool showBeatGrid = true;
    bool showPitchContour = false;
    bool showOctaveBands = false;
    bool measureRoom = false;

    /// How much audio one run of the musical analysis reads, at most, and how
    /// much of that the key is taken from. AnalysisPanel's bounds, which were
    /// constants until this struct existed.
    double analysisSeconds = 120.0;
    double keySeconds = 60.0;
};

/// The FFT sizes a stored setting may name.
///
/// Powers of two because every transform in the project is one, bounded below
/// where the frequency resolution stops being useful for the bottom two
/// octaves and above where one frame covers a third of a second and transients
/// are gone.
inline constexpr int kSmallestFftSize = 512;
inline constexpr int kLargestFftSize = 16384;

/// Clamp every field into the range it is allowed, replacing anything outside
/// it with the default.
///
/// Separate from reading so that the dialog can be held to the same rules as
/// the file: a preference that only the parser validates is a preference the
/// user interface can still put out of range.
[[nodiscard]] Preferences validated(Preferences preferences) noexcept;

/// Everything one run remembers from the last.
struct SavedSettings {
    /// Nothing when no window geometry has been stored -- a first run, or a
    /// file that has lost it. The window then opens at its default size, which
    /// is not the same thing as opening at a stored 1280x760.
    std::optional<WindowPlacement> window;

    /// Empty when unusable or absent, which the window reads as "keep the
    /// layout I was built with".
    std::vector<int> mainSplit;
    std::vector<int> sideSplit;

    RecentFiles recent;
    Preferences preferences;

    /// False when the file was written by a build that knows more than this
    /// one does.
    ///
    /// A user running an older build once -- from a second folder, from a
    /// colleague's stick -- should not lose the settings of the newer one they
    /// use every day, so this build reads what it recognises, and then does
    /// not write the file at all for the rest of the session. The alternative
    /// is silent destruction of settings we cannot even name.
    bool writeBack = true;
};

/// Read a stored map, validating every value.
///
/// Never fails and never throws. An absent file is an empty map and produces
/// the defaults; so does a map full of rubbish, one key at a time.
[[nodiscard]] SavedSettings readSettings(const SettingsMap& stored);

/// The map to write for a given state. Always writes every key, including the
/// version.
[[nodiscard]] SettingsMap writeSettings(const SavedSettings& settings);

/// Where the settings file goes.
enum class SettingsHome {
    /// Beside the executable: the portable case, opted into by the file
    /// already being there.
    Portable,
    /// The per-user configuration directory the platform names.
    PerUser,
};

struct SettingsFile {
    std::filesystem::path path;
    SettingsHome home = SettingsHome::PerUser;
};

/// Decide which of the two files to use.
///
/// Auscultate ships as a zip that is unpacked anywhere, which is the argument
/// for keeping settings beside the executable: the whole application is then
/// one folder to copy, and uninstalling is deleting it.
///
/// It is still the wrong default, for three reasons that all bite the same
/// day. A folder unpacked into Program Files is not writable by the user who
/// runs it, and an application that cannot save its settings *and does not say
/// so* is worse than one that never offered to. Two accounts on one machine
/// sharing one unpacked folder would share one window position and one recent
/// file list, with the second user's paths appearing in the first user's menu.
/// And a shared folder makes the recent list a leak rather than a convenience:
/// the paths one person opened would be in the other's File menu, on a machine
/// where they had each been given their own account precisely so that would not
/// happen.
///
/// So the default is the per-user location, and portable is opt-in: an
/// `Auscultate.ini` that is already beside the executable is used. Creating an
/// empty file is a thing a person can do from a file manager on a stick, needs
/// no switch to discover, and is the convention several portable applications
/// already use.
///
/// The format is INI in both cases and never the registry, which is what
/// QSettings would use on Windows if asked for its native format. An INI file
/// can be read, corrected, copied to another machine and deleted by the person
/// whose settings they are. Registry values under HKCU can be none of those
/// without regedit, and they survive deleting the unpacked folder, which for a
/// zip-shipped application is the thing a user believes is uninstalling it.
[[nodiscard]] SettingsFile chooseSettingsFile(const std::filesystem::path& besideExecutable,
                                              bool besideExecutableExists,
                                              const std::filesystem::path& perUser);

/// Names for the enumerations, for the file and for the batch verbs.
///
/// Names rather than the numbers behind them, so that a file stays readable and
/// stays right when an enumeration gains a member in the middle. These are the
/// same words the --apply verbs already use, and they are defined here once so
/// the file and the command line cannot drift apart.
[[nodiscard]] std::string_view toName(io::SampleFormat format) noexcept;
[[nodiscard]] std::optional<io::SampleFormat> sampleFormatFromName(std::string_view name) noexcept;

[[nodiscard]] std::string_view toName(dsp::DitherType dither) noexcept;
[[nodiscard]] std::optional<dsp::DitherType> ditherFromName(std::string_view name) noexcept;

[[nodiscard]] std::string_view toName(engine::FadeShape shape) noexcept;
[[nodiscard]] std::optional<engine::FadeShape> fadeShapeFromName(std::string_view name) noexcept;

[[nodiscard]] std::string_view toName(FrequencyScale scale) noexcept;
[[nodiscard]] std::optional<FrequencyScale> frequencyScaleFromName(std::string_view name) noexcept;

[[nodiscard]] std::string_view toName(Colourmap map) noexcept;
[[nodiscard]] std::optional<Colourmap> colourmapFromName(std::string_view name) noexcept;

[[nodiscard]] std::string_view toName(analysis::LoudnessPlatform platform) noexcept;
[[nodiscard]] std::optional<analysis::LoudnessPlatform>
loudnessPlatformFromName(std::string_view name) noexcept;

/// UTF-8 for a path, whatever the platform's native encoding is.
///
/// std::filesystem::path::string() is the local 8-bit encoding on Windows,
/// which turns a file under a name outside the active code page into a
/// different name or throws. The settings file is UTF-8, which is also what
/// QString::fromStdString reads, so paths cross that boundary as UTF-8 and
/// come back as themselves.
[[nodiscard]] std::string toUtf8(const std::filesystem::path& path);

[[nodiscard]] std::filesystem::path pathFromUtf8(std::string_view text);

} // namespace sa::ui
