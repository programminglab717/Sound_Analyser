#include <sa/ui/SettingsStore.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMetaType>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <utility>

namespace sa::ui {

namespace {

[[nodiscard]] QString toQString(const std::filesystem::path& path) {
    return QString::fromStdString(toUtf8(path));
}

[[nodiscard]] std::filesystem::path toPath(const QString& text) {
    return pathFromUtf8(text.toStdString());
}

/// The name a portable copy's settings file goes by.
///
/// The product's own name rather than something generic, because it sits in a
/// folder the user unzipped and has to be recognisable as ours among whatever
/// else is in there.
constexpr QLatin1String kPortableFileName{"Auscultate.ini"};

} // namespace

SettingsFile settingsFileForThisBuild() {
    const QString beside =
        QCoreApplication::applicationDirPath() + QLatin1String{"/"} + kPortableFileName;

    // Asked for rather than constructed: this is exactly the file QSettings
    // would keep an INI in for this user, this organisation and this
    // application, on whichever platform it is running.
    const QSettings perUser{QSettings::IniFormat, QSettings::UserScope,
                            QCoreApplication::organizationName(),
                            QCoreApplication::applicationName()};

    return chooseSettingsFile(toPath(beside), QFileInfo::exists(beside),
                              toPath(perUser.fileName()));
}

SettingsStore::SettingsStore(std::filesystem::path file) : file_{std::move(file)} {}

SettingsMap SettingsStore::read() const {
    SettingsMap values;
    if (file_.empty()) {
        return values;
    }

    QSettings settings{toQString(file_), QSettings::IniFormat};
    // Not checked for FormatError: QSettings hands back whatever it managed to
    // parse, and a half-readable file yielding half its settings is better than
    // one that yields none. Everything below validates what it gets.
    const QStringList keys = settings.allKeys();
    for (const QString& key : keys) {
        const QVariant value = settings.value(key);
        // An INI value with an unquoted comma in it arrives as a QStringList,
        // which is QSettings' own convention and not something this layer gets
        // to opt out of. Joining it back is what makes a hand-edited
        // "mainsplit=420,230,140" mean what the person who typed it meant; the
        // same key written by this build is quoted and arrives as one string.
        const QString text = value.typeId() == QMetaType::QStringList
                                 ? value.toStringList().join(QLatin1Char{','})
                                 : value.toString();
        values.set(key.toLower().toStdString(), text.toStdString());
    }
    return values;
}

bool SettingsStore::write(const SettingsMap& values) const {
    if (file_.empty()) {
        return false;
    }

    const QString name = toQString(file_);
    // QSettings creates the file but not always the tree above it, and the
    // per-user location is a folder that has never existed until the first
    // time anything is saved.
    const QFileInfo info{name};
    if (!info.dir().exists() && !QDir{}.mkpath(info.absolutePath())) {
        return false;
    }

    QSettings settings{name, QSettings::IniFormat};
    // Cleared rather than merged, so a key this build no longer writes -- a
    // preference that has been removed, a recent entry that has fallen off the
    // end of the list -- does not linger in the file forever.
    settings.clear();
    for (const auto& [key, value] : values.entries()) {
        settings.setValue(QString::fromStdString(key), QString::fromStdString(value));
    }
    settings.sync();
    return settings.status() == QSettings::NoError;
}

} // namespace sa::ui
