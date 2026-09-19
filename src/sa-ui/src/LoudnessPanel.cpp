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
                                    SampleCount length, const CancellationToken& cancellation,
                                    analysis::ProgrammeAnalysis& out) {
    const io::AudioFileInfo& info = source.info();
    auto loudness = analysis::LoudnessMeter::create(info.sampleRate, info.layout);
    auto peaks = analysis::TruePeakMeter::create(info.channelCount());
    auto statistics = analysis::SignalStatisticsMeter::create(info.channelCount());
    if (!loudness || !peaks || !statistics) {
        return false;
    }
    // Optional, because only a stereo pair has a stereo field and mono is
    // ordinary material, not a failure.
    auto stereo = analysis::StereoFieldMeter::create(info.channelCount());

    AudioBuffer block{info.layout, kBlockFrames};
    SampleCount done = 0;
    while (done < length) {
        if (cancellation.isCancelled()) {
            return false;
        }
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
        if (stereo) {
            stereo.value().process(filled);
        }
        done += read.value();
    }

    out.loudness = loudness.value().measurement();
    out.truePeakDbtp = peaks.value().truePeakDbtp();
    if (stereo) {
        out.stereo = stereo.value().field();
    }

    // The streaming meter is an interpolator, and an interpolator droops: ours
    // reads up to 0.44 dB low on bright transients. Where the range is small
    // enough to hold, measure it exactly instead and quote that, because a
    // true-peak number that reads low is the one that ships a master over the
    // ceiling.
    //
    // The bound is on the range being measured, not the file. A selection is
    // usually short even when the programme is not.
    constexpr std::size_t kExactBudgetBytes = 400'000'000;
    const auto bytes = static_cast<std::size_t>(length) *
                       static_cast<std::size_t>(info.channelCount()) * sizeof(float);
    if (done > 0 && bytes <= kExactBudgetBytes) {
        AudioBuffer whole{info.layout, done};
        SampleCount filled = 0;
        while (filled < done) {
            AudioBufferView view = whole.view().subRange(filled, done - filled);
            const auto read = source.read(start + filled, view);
            if (!read || read.value() <= 0) {
                break;
            }
            filled += read.value();
        }
        if (filled == done) {
            if (auto exact = analysis::exactTruePeakDbtp(whole.constView()); exact) {
                out.truePeakDbtp = exact.value();
            }
        }
    }

    out.statistics = statistics.value().statistics(out.truePeakDbtp, out.loudness.integratedLufs);
    return done > 0;
}

} // namespace

LoudnessPanel::LoudnessPanel(QWidget* parent)
    : QWidget(parent), session_(std::make_shared<Session>()) {
    session_->panel = this;
    buildLayout();
    resetLabels();
}

void LoudnessPanel::stopWorker() {
    cancellation_.cancel();
    if (worker_.joinable()) {
        worker_.join();
    }
    cancellation_.reset();
}

LoudnessPanel::~LoudnessPanel() {
    // Bumping the generation makes a running worker's result be discarded; the
    // worker holds the session by shared_ptr, so it stays alive to be read
    // after this panel is gone. Clearing the pointer under the mutex is what
    // makes that safe rather than merely likely -- see Session.
    session_->generation.fetch_add(1);
    stopWorker();
    const std::lock_guard<std::mutex> lock{session_->mutex};
    session_->panel = nullptr;
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

    // Both return the widgets they made, so a group of rows can be hidden
    // together later. Only the stereo section needs that so far.
    const auto addRow = [&](const QString& name, QLabel*& value) {
        auto* label = new QLabel{name, this};
        label->setStyleSheet("color: #8a8fa0;");
        value = new QLabel{this};
        value->setStyleSheet("color: #c8ccd8;");
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(label, row, 0);
        grid->addWidget(value, row, 1);
        ++row;
        return label;
    };

    const auto addSeparator = [&](const QString& title) {
        auto* label = new QLabel{title, this};
        label->setStyleSheet("color: #5a6070; margin-top: 8px;");
        grid->addWidget(label, row++, 0, 1, 2);
        return label;
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

    stereoWidgets_ = {addSeparator(tr("STEREO")),
                      addRow(tr("Correlation"), correlation_),
                      correlation_,
                      addRow(tr("Width"), width_),
                      width_,
                      addRow(tr("Balance"), balance_),
                      balance_,
                      addRow(tr("Mono sum"), monoLoss_),
                      monoLoss_};

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
        emit targetChanged();
    });

    grid->setRowStretch(row, 1);
    grid->setColumnStretch(1, 1);
}

void LoudnessPanel::clear() {
    // Bump the generation and stop the worker, not merely blank the labels. A
    // measurement in flight passes its generation check otherwise and delivers
    // into the emptied panel a moment later, so closing a document while its
    // meters were still running left figures on screen for a document that is
    // no longer open. The three callers below that already hold the worker
    // still -- the constructor, a finished delivery, and measure() past its own
    // stopWorker -- want only the labels, and call resetLabels directly.
    session_->generation.fetch_add(1);
    stopWorker();
    resetLabels();
}

void LoudnessPanel::resetLabels() {
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

void LoudnessPanel::deliver(const analysis::ProgrammeAnalysis& result, const QString& what,
                            bool ok) {
    busy_ = false;
    if (ok) {
        show(result, what);
    } else {
        resetLabels();
        heading_->setText(tr("could not measure %1").arg(what));
    }
    emit measurementFinished();
}

void LoudnessPanel::setPending(const QString& what) {
    busy_ = true;
    heading_->setText(tr("measuring %1…").arg(what));
}

void LoudnessPanel::measure(std::shared_ptr<const io::AudioSource> source, SampleIndex start,
                            SampleCount length, const QString& what) {
    const std::uint64_t mine = session_->generation.fetch_add(1) + 1;
    // Stop the previous one before starting another. Without this a batch run
    // of five operations had five measurements of the same document running at
    // once, four of them already superseded.
    stopWorker();

    if (!source || length <= 0) {
        resetLabels();
        emit measurementFinished();
        return;
    }
    setPending(what);

    worker_ = std::thread{[source = std::move(source), start, length, what, mine,
                           session = session_, cancellation = &cancellation_] {
        analysis::ProgrammeAnalysis result;
        const bool ok = measureStreaming(*source, start, length, *cancellation, result);

        if (session->generation.load() != mine) {
            return;
        }
        const std::lock_guard<std::mutex> lock{session->mutex};
        if (session->panel == nullptr || session->generation.load() != mine) {
            return;
        }
        LoudnessPanel* panel = session->panel;
        QMetaObject::invokeMethod(
            panel,
            [panel, result, what, ok, mine, session] {
                // Back on the main thread. The generation is checked again
                // because a newer request may have been made while this one
                // was queued.
                if (session->generation.load() != mine) {
                    return;
                }
                panel->deliver(result, what, ok);
            },
            Qt::QueuedConnection);
    }};
}

std::optional<double> LoudnessPanel::conformGainDb() const {
    if (!hasLatest_ || latest_.loudness.gatedBlockCount <= 0) {
        return std::nullopt;
    }
    const auto platform = static_cast<analysis::LoudnessPlatform>(target_->currentData().toInt());
    return analysis::check(platform, latest_.loudness.integratedLufs, latest_.truePeakDbtp)
        .conformGainDb;
}

QString LoudnessPanel::targetName() const {
    return target_->currentText();
}

analysis::LoudnessPlatform LoudnessPanel::target() const {
    return static_cast<analysis::LoudnessPlatform>(target_->currentData().toInt());
}

void LoudnessPanel::setTarget(analysis::LoudnessPlatform platform) {
    // A platform the list does not hold leaves the selection alone, rather than
    // falling back to whatever is first. The list is every target this build
    // knows, so the only way here is a value from somewhere else entirely.
    if (const int index = target_->findData(static_cast<int>(platform)); index >= 0) {
        target_->setCurrentIndex(index);
    }
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

    showStereo(result.stereo);

    rms_->setText(decibels(result.statistics.rmsDbfs, "dBFS"));
    crest_->setText(QStringLiteral("%1 dB").arg(result.statistics.crestFactorDb, 0, 'f', 1));
    dcOffset_->setText(QStringLiteral("%1").arg(result.statistics.dcOffset, 0, 'f', 5));

    showCompliance(result);
}

void LoudnessPanel::showStereo(const analysis::StereoField& field) {
    for (QWidget* widget : stereoWidgets_) {
        if (widget != nullptr) {
            widget->setVisible(field.valid);
        }
    }
    if (!field.valid) {
        return;
    }

    correlation_->setText(QStringLiteral("%1").arg(field.correlation, 0, 'f', 2));
    width_->setText(decibels(field.widthDb, "dB"));
    // Signed and named, because "-6.0 dB" alone does not say which side.
    balance_->setText(std::abs(field.balanceDb) < 0.05
                          ? tr("centred")
                          : tr("%1 dB %2")
                                .arg(std::abs(field.balanceDb), 0, 'f', 1)
                                .arg(field.balanceDb > 0.0 ? tr("right") : tr("left")));
    monoLoss_->setText(decibels(field.monoLossDb, "dB"));
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
