#pragma once

#include <sa/ui/Settings.h>

#include <filesystem>

namespace sa::ui {

/// The settings file this build will use, decided by chooseSettingsFile().
///
/// The per-user candidate is whatever QSettings itself would use for an INI in
/// the user scope, asked for rather than spelled out: %APPDATA% and
/// ~/.config are Qt's business, and a second copy of that rule here would be
/// one more thing to get wrong on a platform nobody is testing on today.
///
/// Requires the application and organisation names to be set, which main()
/// does before the window exists.
[[nodiscard]] SettingsFile settingsFileForThisBuild();

/// A settings file, as a flat map, through QSettings.
///
/// The only Qt in the settings layer, and deliberately the only thing it does:
/// read a file into a map and write a map into a file. Everything that decides
/// anything is in Settings.h, where it can be tested without a window -- and
/// what this class does instead is the part that cannot be, because it is an
/// INI parser somebody else wrote.
class SettingsStore {
public:
    explicit SettingsStore(std::filesystem::path file);

    /// Whatever is in the file. An absent file, an unreadable one and one full
    /// of rubbish all come back as an empty or partial map rather than as a
    /// failure: every value is validated on the way out anyway, and refusing
    /// to start because a settings file is damaged would be a worse failure
    /// than starting with the defaults.
    [[nodiscard]] SettingsMap read() const;

    /// Replace the file's contents. False when it could not be written --
    /// a read-only folder, a full disk, a portable copy on a locked stick.
    bool write(const SettingsMap& values) const;

    [[nodiscard]] const std::filesystem::path& file() const noexcept { return file_; }

private:
    std::filesystem::path file_;
};

} // namespace sa::ui
