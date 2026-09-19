#pragma once

#include <sa/ui/Settings.h>

#include <QDialog>
#include <filesystem>
#include <optional>

class QComboBox;
class QDoubleSpinBox;

namespace sa::ui {

/// The settings the window used to hard-code, in one form.
///
/// A form rather than another menu, for what FieldDialog's comment says about
/// six consecutive prompts: these are settings that are read against each
/// other. An FFT size means nothing without the hop beside it, and the key
/// bound only makes sense next to the analysis bound it is taken from.
///
/// What is deliberately *not* here: the four analysis overlays, the frequency
/// scale, the dynamic range and the colour map already have menu entries that
/// tick, and the export format and dither already have theirs. They are all
/// persisted, and several of them appear here as well where the dialog is the
/// better place to see them together -- but a setting with two controls has to
/// keep them in step, so everything here writes through the same Preferences
/// struct the menus do, and the window re-ticks its menus when the dialog
/// closes.
class PreferencesDialog : public QDialog {
    Q_OBJECT

public:
    PreferencesDialog(QWidget* parent, const Preferences& preferences,
                      const std::filesystem::path& file, SettingsHome home);

    /// The preferences as left by the user, validated.
    [[nodiscard]] Preferences preferences() const;

    /// Show the dialog and return what it was left at, or nothing if it was
    /// cancelled.
    [[nodiscard]] static std::optional<Preferences> ask(QWidget* parent,
                                                        const Preferences& preferences,
                                                        const std::filesystem::path& file,
                                                        SettingsHome home);

private:
    /// Every combo holds the enumeration value in its item data, so reading one
    /// back is a cast rather than a comparison against a translated label.
    QComboBox* loudnessTarget_ = nullptr;
    QComboBox* exportFormat_ = nullptr;
    QComboBox* dither_ = nullptr;
    QComboBox* fadeShape_ = nullptr;
    QComboBox* fftSize_ = nullptr;
    QComboBox* overlap_ = nullptr;
    QComboBox* floorDb_ = nullptr;
    QComboBox* frequencyScale_ = nullptr;
    QComboBox* colourmap_ = nullptr;
    QDoubleSpinBox* analysisSeconds_ = nullptr;
    QDoubleSpinBox* keySeconds_ = nullptr;

    /// The four overlays are not here; see the class comment. This keeps
    /// whatever they were so that preferences() answers with the whole struct
    /// rather than with the part the dialog happens to show.
    Preferences untouched_;
};

} // namespace sa::ui
