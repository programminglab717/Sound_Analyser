#include <sa/analysis/LoudnessMeter.h>
#include <sa/analysis/SignalStatistics.h>
#include <sa/analysis/TruePeakMeter.h>
#include <sa/ui/LoudnessPanel.h>

#include <QComboBox>
#include <QGridLayout>
#include <QLabel>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <thread>

namespace sa::ui {

namespace {

constexpr QColor kOnTarget{0x6b, 0xcf, 0x7f};
constexpr QColor kOver{0xff, 0x6b, 0x6b};
constexpr QColor kUnder{0xff, 0x9f, 0x43};

/// Blocks big enough that the per-call overhead of three meters disappears,
/// small enough that a two-hour programme never costs more than this.
constexpr SampleCount kBlockFrames = 65536;

[[nodiscard]] QString decibels(double value, const char* unit, int places = 1) {
    if (value <= analysis::kDecibelFloor + 1.0) {
        return QStringLiteral("--");
    }
    return QStringLiteral("%1 %2").arg(value, 0, 'f', places).arg(QString::fromUtf8(unit));
}

/// Stream `length` frames through all three meters.
///
/// Deliberately not analyseProgramme(): that takes the whole buffer at once,
/// and a metering panel that needs the programme resident to measure it is a
/// metering panel that falls over on the files this product exists for.
[[nodiscard]] bool measureStreaming(const io::AudioSource& source, SampleIndex start,
                                    SampleCount length, analysis::ProgrammeAnalysis& out) {
    const io::AudioFileInfo& info = source.info();
    auto loudness = analysis::LoudnessMeter::create(info.sampleRate, info.layout);
    auto peaks = analysis::TruePeakMeter::create(info.channelCount());
    auto statistics = analysis::SignalStatisticsMeter::create(info.channelCount());
    if (!loudness || !peaks || !statistics) {
        return false;
    }

    AudioBuffer block{info.layout, kBlockFrames};
    SampleCount done = 0;
    while (done < length) {
        const SampleCount want = std::min<SampleCount>(kBlockFrames, length - done);
        AudioBufferView view = block.view().subRange(0, want);
        const auto read = source.read(start + done, view);
        if (!read || read.value() <= 0) {
            break;
        }
        ConstAudioBufferView filled = view.subRange(0, read.value());
        loudness.value().process(filled);
        peaks.value().process(filled);
        statistics.value().process(filled);
        done += read.value();
    }

    out.loudness = loudness.value().measurement();
    out.truePeakDbtp = peaks.value().truePeakDbtp();
    out.statistics = statistics.value().statistics(out.truePeakDbtp, out.loudness.integratedLufs);
    return done > 0;
}

} // namespace

LoudnessPanel::LoudnessPanel(QWidget* parent)
    : QWidget(parent), generation_(std::make_shared<std::atomic<std::uint64_t>>(0)) {
    buildLayout();
    clear();
}

LoudnessPanel::~LoudnessPanel() {
    // Bumping the generation is what makes a running worker's result be
    // discarded. The worker holds the counter by shared_ptr, so it stays alive
    // to be read even after this panel is gone.
    generation_->fetch_add(1);
}

void LoudnessPanel::buildLayout() {
    setMinimumWidth(226);
    setMaximumWidth(300);

    auto* grid = new QGridLayout{this};
    grid->setContentsMargins(12, 10, 12, 10);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(3);

    int row = 0;
    heading_ = new QLabel{this};
    heading_->setStyleSheet("color: #6d7382;");
    grid->addWidget(heading_, row++, 0, 1, 2);

    integrated_ = new QLabel{this};
    QFont big = font();
    big.setPointSizeF(big.pointSizeF() * 2.1);
    big.setBold(true);
    integrated_->setFont(big);
    integrated_->setStyleSheet("color: #dfe4f0;");
    grid->addWidget(integrated_, row, 0, 1, 2);
    ++row;

    auto* units = new QLabel{tr("LUFS integrated"), this};
    units->setStyleSheet("color: #6d7382;");
    grid->addWidget(units, row++, 0, 1, 2);

    const auto addRow = [&](const QString& name, QLabel*& value) {
        auto* label = new QLabel{name, this};
        label->setStyleSheet("color: #8a8fa0;");
        value = new QLabel{this};
        value->setStyleSheet("color: #c8ccd8;");
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(label, row, 0);
        grid->addWidget(value, row, 1);
        ++row;
    };

    const auto addSeparator = [&](const QString& title) {
        auto* label = new QLabel{title, this};
        label->setStyleSheet("color: #5a6070; margin-top: 8px;");
        grid->addWidget(label, row++, 0, 1, 2);
    };

    addSeparator(tr("LOUDNESS"));
    addRow(tr("Range"), range_);
    addRow(tr("Short term"), shortTerm_);
    addRow(tr("Max short term"), maximumShortTerm_);
    addRow(tr("Momentary"), momentary_);

    addSeparator(tr("PEAK"));
    addRow(tr("True peak"), truePeak_);
    addRow(tr("Sample peak"), samplePeak_);
    addRow(tr("Peak to loudness"), peakToLoudness_);

    addSeparator(tr("SIGNAL"));
    addRow(tr("RMS"), rms_);
    addRow(tr("Crest factor"), crest_);
    addRow(tr("DC offset"), dcOffset_);

    addSeparator(tr("TARGET"));
    target_ = new QComboBox{this};
    for (std::size_t i = 0; i < analysis::targetCount(); ++i) {
        const analysis::ComplianceTarget& preset = analysis::allTargets()[i];
        target_->addItem(
            QString::fromUtf8(preset.name.data(), static_cast<qsizetype>(preset.name.size())),
            QVariant::fromValue(static_cast<int>(preset.platform)));
    }
    const int ebu = target_->findData(static_cast<int>(analysis::LoudnessPlatform::EbuR128));
    // EBU R128 rather than list position zero: it is the broadcast baseline the
    // others are described against, and a meter whose default target depends on
    // the order of an array is a meter nobody can quote.
    target_->setCurrentIndex(ebu >= 0 ? ebu : 0);
    target_->setStyleSheet("QComboBox { background: #1c1e26; color: #c8ccd8; border: 1px solid "
                           "#2a2c38; padding: 3px; }"
                           "QComboBox QAbstractItemView { background: #16171d; color: #c8ccd8; "
                           "selection-background-color: #2a2c38; }");
    grid->addWidget(target_, row++, 0, 1, 2);

    verdict_ = new QLabel{this};
    verdict_->setWordWrap(true);
    grid->addWidget(verdict_, row++, 0, 1, 2);

    connect(target_, &QComboBox::currentIndexChanged, this, [this] {
        if (hasLatest_) {
            showCompliance(latest_);
        }
    });

    grid->setRowStretch(row, 1);
    grid->setColumnStretch(1, 1);
}

void LoudnessPanel::clear() {
    hasLatest_ = false;
    busy_ = false;
    heading_->setText(tr("nothing loaded"));
    integrated_->setText(QStringLiteral("--"));
    for (QLabel* label : {range_, shortTerm_, maximumShortTerm_, momentary_, truePeak_, samplePeak_,
                          peakToLoudness_, rms_, crest_, dcOffset_}) {
        label->setText(QStringLiteral("--"));
    }
    verdict_->clear();
}

void LoudnessPanel::setPending(const QString& what) {
    busy_ = true;
    heading_->setText(tr("measuring %1…").arg(what));
}

void LoudnessPanel::measure(std::shared_ptr<const io::AudioSource> source, SampleIndex start,
                            SampleCount length, const QString& what) {
    const std::uint64_t mine = generation_->fetch_add(1) + 1;

    if (!source || length <= 0) {
        clear();
        emit measurementFinished();
        return;
    }
    setPending(what);

    // Detached rather than joined: the panel outlives no worker it cares about,
    // because the generation check makes a late result harmless, and joining in
    // the destructor would stall the window's close on a long measurement.
    std::thread{[source = std::move(source), start, length, what, mine, generation = generation_,
                 this] {
        analysis::ProgrammeAnalysis result;
        const bool ok = measureStreaming(*source, start, length, result);

        if (generation->load() != mine) {
            return;
        }
        QMetaObject::invokeMethod(
            this,
            [this, result, what, ok, mine, generation] {
                if (generation->load() != mine) {
                    return;
                }
                busy_ = false;
                if (ok) {
                    show(result, what);
                } else {
                    clear();
                    heading_->setText(tr("could not measure %1").arg(what));
                }
                emit measurementFinished();
            },
            Qt::QueuedConnection);
    }}.detach();
}

void LoudnessPanel::show(const analysis::ProgrammeAnalysis& result, const QString& what) {
    latest_ = result;
    hasLatest_ = true;

    heading_->setText(what);
    const analysis::LoudnessMeasurement& loudness = result.loudness;

    // Zero gated blocks means nothing cleared the absolute gate -- material
    // shorter than one 400 ms block, or silence. Printing the floor as if it
    // were a measurement is how a meter lies.
    integrated_->setText(loudness.gatedBlockCount > 0
                             ? QStringLiteral("%1").arg(loudness.integratedLufs, 0, 'f', 1)
                             : QStringLiteral("--"));

    range_->setText(loudness.shortTermBlockCount > 0
                        ? QStringLiteral("%1 LU").arg(loudness.loudnessRangeLu, 0, 'f', 1)
                        : QStringLiteral("--"));
    shortTerm_->setText(decibels(loudness.shortTermLufs, "LUFS"));
    maximumShortTerm_->setText(decibels(loudness.maximumShortTermLufs, "LUFS"));
    momentary_->setText(decibels(loudness.maximumMomentaryLufs, "LUFS"));

    truePeak_->setText(decibels(result.truePeakDbtp, "dBTP"));
    samplePeak_->setText(decibels(result.statistics.samplePeakDbfs, "dBFS"));
    peakToLoudness_->setText(decibels(result.statistics.peakToLoudnessRatioDb, "LU"));

    rms_->setText(decibels(result.statistics.rmsDbfs, "dBFS"));
    crest_->setText(QStringLiteral("%1 dB").arg(result.statistics.crestFactorDb, 0, 'f', 1));
    dcOffset_->setText(QStringLiteral("%1").arg(result.statistics.dcOffset, 0, 'f', 5));

    showCompliance(result);
}

void LoudnessPanel::showCompliance(const analysis::ProgrammeAnalysis& result) {
    if (result.loudness.gatedBlockCount <= 0) {
        verdict_->clear();
        return;
    }

    const auto platform = static_cast<analysis::LoudnessPlatform>(target_->currentData().toInt());
    const analysis::ComplianceResult check =
        analysis::check(platform, result.loudness.integratedLufs, result.truePeakDbtp);

    QStringList notes;
    QColor colour = kOnTarget;

    // The target's own tolerance decides "on target", not a number chosen here:
    // EBU R128's +/-0.5 LU and ATSC A/85's +/-2 dB come from those standards.
    if (check.loudnessOnTarget) {
        notes << tr("On target (%1 LU)").arg(check.loudnessDeviationLu, 0, 'f', 1);
    } else if (check.loudnessDeviationLu > 0.0) {
        notes << tr("%1 LU too loud").arg(check.loudnessDeviationLu, 0, 'f', 1);
        colour = kOver;
    } else {
        notes << tr("%1 LU too quiet").arg(-check.loudnessDeviationLu, 0, 'f', 1);
        colour = kUnder;
    }

    if (!check.truePeakWithinCeiling) {
        notes << tr("true peak %1 dB over the %2 dBTP ceiling")
                     .arg(-check.truePeakMarginDb, 0, 'f', 1)
                     .arg(check.target.truePeakCeilingDbtp, 0, 'f', 1);
        colour = kOver;
    }

    if (!check.passed()) {
        // conformGainDb, not gainToTargetDb: the gain that gets closest to the
        // target without pushing the peaks through the ceiling. Where the two
        // differ, the difference is the amount that would have to be limited
        // rather than merely turned up, and saying so is the honest advice.
        notes << tr("apply %1 dB").arg(check.conformGainDb, 0, 'f', 1);
        if (std::abs(check.conformGainDb - check.gainToTargetDb) > 0.05) {
            notes << tr("(%1 dB would hit the target but breach the ceiling)")
                         .arg(check.gainToTargetDb, 0, 'f', 1);
        }
    }

    verdict_->setText(notes.join(QStringLiteral("\n")));
    verdict_->setStyleSheet(QStringLiteral("color: %1;").arg(colour.name()));
}

} // namespace sa::ui
