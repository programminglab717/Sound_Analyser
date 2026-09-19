#include <sa/ui/PreferencesDialog.h>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <algorithm>

namespace sa::ui {

namespace {

/// Put `value` in the combo, adding it as the item to select.
template <typename Enum>
void addChoice(QComboBox* box, const QString& label, Enum value, Enum selected) {
    box->addItem(label, static_cast<int>(value));
    if (value == selected) {
        box->setCurrentIndex(box->count() - 1);
    }
}

template <typename Enum>
[[nodiscard]] Enum chosen(const QComboBox* box, Enum fallback) {
    if (box == nullptr || box->currentIndex() < 0) {
        return fallback;
    }
    return static_cast<Enum>(box->currentData().toInt());
}

} // namespace

PreferencesDialog::PreferencesDialog(QWidget* parent, const Preferences& preferences,
                                     const std::filesystem::path& file, SettingsHome home)
    : QDialog{parent}, untouched_{preferences} {
    setWindowTitle(tr("Preferences"));

    auto* layout = new QVBoxLayout{this};
    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    loudnessTarget_ = new QComboBox{this};
    for (std::size_t i = 0; i < analysis::targetCount(); ++i) {
        const analysis::ComplianceTarget& target = analysis::allTargets()[i];
        addChoice(loudnessTarget_,
                  tr("%1  (%2 LUFS)")
                      .arg(QString::fromUtf8(target.name.data(),
                                             static_cast<qsizetype>(target.name.size())))
                      .arg(target.integratedLufs, 0, 'f', 1),
                  target.platform, preferences.loudnessTarget);
    }
    form->addRow(tr("&Loudness target"), loudnessTarget_);

    exportFormat_ = new QComboBox{this};
    addChoice(exportFormat_, tr("16-bit"), io::SampleFormat::PcmInt16, preferences.exportFormat);
    addChoice(exportFormat_, tr("24-bit"), io::SampleFormat::PcmInt24, preferences.exportFormat);
    addChoice(exportFormat_, tr("32-bit float"), io::SampleFormat::Float32,
              preferences.exportFormat);
    form->addRow(tr("&Export format"), exportFormat_);

    dither_ = new QComboBox{this};
    addChoice(dither_, tr("None"), dsp::DitherType::None, preferences.dither);
    addChoice(dither_, tr("Triangular"), dsp::DitherType::Tpdf, preferences.dither);
    addChoice(dither_, tr("Triangular, noise-shaped"), dsp::DitherType::TpdfNoiseShaped,
              preferences.dither);
    form->addRow(tr("&Dither"), dither_);

    fadeShape_ = new QComboBox{this};
    addChoice(fadeShape_, tr("Linear"), engine::FadeShape::Linear, preferences.fadeShape);
    addChoice(fadeShape_, tr("Equal power"), engine::FadeShape::EqualPower, preferences.fadeShape);
    addChoice(fadeShape_, tr("Logarithmic"), engine::FadeShape::Logarithmic, preferences.fadeShape);
    addChoice(fadeShape_, tr("Exponential"), engine::FadeShape::Exponential, preferences.fadeShape);
    addChoice(fadeShape_, tr("S-curve"), engine::FadeShape::SCurve, preferences.fadeShape);
    form->addRow(tr("&Fade shape"), fadeShape_);

    fftSize_ = new QComboBox{this};
    for (int size = kSmallestFftSize; size <= kLargestFftSize; size *= 2) {
        fftSize_->addItem(tr("%1 samples").arg(size), size);
        if (size == preferences.fftSize) {
            fftSize_->setCurrentIndex(fftSize_->count() - 1);
        }
    }
    form->addRow(tr("Spectrogram &window"), fftSize_);

    // Overlap rather than a hop in samples: the hop only means anything as a
    // fraction of the window, and the pair has to stay consistent -- which is
    // a rule the file's validator already holds, so the dialog cannot be the
    // one place it is possible to break it.
    overlap_ = new QComboBox{this};
    const int overlapNow = preferences.hopSize > 0 ? preferences.fftSize / preferences.hopSize : 4;
    for (const int factor : {2, 4, 8, 16}) {
        overlap_->addItem(tr("%1x  (hop %2)").arg(factor).arg(preferences.fftSize / factor),
                          factor);
        if (factor == overlapNow) {
            overlap_->setCurrentIndex(overlap_->count() - 1);
        }
    }
    form->addRow(tr("Spectrogram &overlap"), overlap_);

    floorDb_ = new QComboBox{this};
    for (const double floorDb : kFloorChoicesDb) {
        floorDb_->addItem(tr("%1 dB").arg(static_cast<int>(floorDb)), floorDb);
        if (floorDb == preferences.spectrogramFloorDb) {
            floorDb_->setCurrentIndex(floorDb_->count() - 1);
        }
    }
    form->addRow(tr("Dynamic &range"), floorDb_);

    frequencyScale_ = new QComboBox{this};
    addChoice(frequencyScale_, tr("Logarithmic"), FrequencyScale::Logarithmic,
              preferences.frequencyScale);
    addChoice(frequencyScale_, tr("Linear"), FrequencyScale::Linear, preferences.frequencyScale);
    form->addRow(tr("Frequency &scale"), frequencyScale_);

    colourmap_ = new QComboBox{this};
    addChoice(colourmap_, tr("Magma"), Colourmap::Magma, preferences.colourmap);
    addChoice(colourmap_, tr("Viridis"), Colourmap::Viridis, preferences.colourmap);
    addChoice(colourmap_, tr("Greyscale"), Colourmap::Grey, preferences.colourmap);
    form->addRow(tr("&Colour map"), colourmap_);

    analysisSeconds_ = new QDoubleSpinBox{this};
    analysisSeconds_->setDecimals(0);
    analysisSeconds_->setRange(5.0, 3600.0);
    analysisSeconds_->setSingleStep(30.0);
    analysisSeconds_->setSuffix(tr(" s"));
    analysisSeconds_->setValue(preferences.analysisSeconds);
    form->addRow(tr("&Analyse at most"), analysisSeconds_);

    keySeconds_ = new QDoubleSpinBox{this};
    keySeconds_->setDecimals(0);
    keySeconds_->setRange(5.0, 3600.0);
    keySeconds_->setSingleStep(15.0);
    keySeconds_->setSuffix(tr(" s"));
    keySeconds_->setValue(preferences.keySeconds);
    form->addRow(tr("&Key from the first"), keySeconds_);

    layout->addLayout(form);

    // Where the settings are, spelled out. A user who wants to back them up,
    // copy them to another machine, or delete them and start again should not
    // have to be told by support which folder this build chose.
    auto* where =
        new QLabel{home == SettingsHome::Portable
                       ? tr("Portable: settings are kept in %1, beside the application.")
                             .arg(QString::fromStdString(toUtf8(file)))
                       : tr("Settings are kept in %1.").arg(QString::fromStdString(toUtf8(file))),
                   this};
    where->setWordWrap(true);
    layout->addWidget(where);

    auto* buttons =
        new QDialogButtonBox{QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, this};
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

Preferences PreferencesDialog::preferences() const {
    Preferences chosenPreferences = untouched_;
    chosenPreferences.loudnessTarget = chosen(loudnessTarget_, untouched_.loudnessTarget);
    chosenPreferences.exportFormat = chosen(exportFormat_, untouched_.exportFormat);
    chosenPreferences.dither = chosen(dither_, untouched_.dither);
    chosenPreferences.fadeShape = chosen(fadeShape_, untouched_.fadeShape);
    chosenPreferences.frequencyScale = chosen(frequencyScale_, untouched_.frequencyScale);
    chosenPreferences.colourmap = chosen(colourmap_, untouched_.colourmap);

    chosenPreferences.fftSize = fftSize_->currentData().toInt();
    const int factor = std::max(1, overlap_->currentData().toInt());
    chosenPreferences.hopSize = chosenPreferences.fftSize / factor;
    chosenPreferences.spectrogramFloorDb = floorDb_->currentData().toDouble();
    chosenPreferences.analysisSeconds = analysisSeconds_->value();
    chosenPreferences.keySeconds = keySeconds_->value();

    // The same validator the file goes through. A key bound above the analysis
    // bound is reachable from these two spin boxes, and the dialog is not
    // where that gets its own second opinion.
    return validated(chosenPreferences);
}

std::optional<Preferences> PreferencesDialog::ask(QWidget* parent, const Preferences& preferences,
                                                  const std::filesystem::path& file,
                                                  SettingsHome home) {
    PreferencesDialog dialog{parent, preferences, file, home};
    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    return dialog.preferences();
}

} // namespace sa::ui
