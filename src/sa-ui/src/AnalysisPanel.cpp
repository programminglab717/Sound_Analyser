#include <sa/ui/AnalysisPanel.h>

#include <QGridLayout>
#include <QLabel>
#include <algorithm>
#include <cmath>
#include <utility>

namespace sa::ui {

namespace {

/// The same three colours the metering panel judges with, so that "this is
/// doubtful" looks the same wherever the window says it.
constexpr QColor kFirm{0xc8, 0xcc, 0xd8};
constexpr QColor kDoubtful{0xff, 0x9f, 0x43};
constexpr QColor kAbsent{0x8a, 0x8f, 0xa0};

/// Raises a flag when it goes out of scope, however the scope is left.
///
/// A worker sets one of these as its last effect, and it is the whole of how
/// the panel can ask "has that thread stopped" without blocking on the answer.
class Signal {
public:
    explicit Signal(std::shared_ptr<std::atomic<bool>> flag) : flag_(std::move(flag)) {}

    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;

    ~Signal() { flag_->store(true); }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

/// Read up to `frames` frames into `into`, stopping early if the request has
/// been superseded. Returns how many were read.
///
/// Block by block rather than in one call so that cancellation has somewhere to
/// land. Most of what follows is one call into a library function that runs to
/// completion, so the places a superseded run can give up are few and each one
/// has to be put there on purpose.
[[nodiscard]] SampleCount readBlocks(const io::AudioSource& source, SampleIndex start,
                                     AudioBufferView into, const CancellationToken& cancellation) {
    constexpr SampleCount kBlockFrames = 65536;
    SampleCount done = 0;
    while (done < into.frames()) {
        if (cancellation.isCancelled()) {
            return done;
        }
        const SampleCount want = std::min<SampleCount>(kBlockFrames, into.frames() - done);
        const auto read = source.read(start + done, into.subRange(done, want));
        if (!read || read.value() <= 0) {
            break;
        }
        done += read.value();
    }
    return done;
}

/// A hop that gives a contour a few points per pixel of a wide window without
/// tracking a two-minute passage at the default 5.3 ms.
///
/// The default hop is right for a phrase and wasteful for a passage: at 48 kHz
/// it is 22,500 frames a minute, every one of them a transform, and a display
/// two thousand columns wide cannot show more than a fraction of them. So the
/// hop grows with the length, in powers of two, and stops at eight times the
/// default -- past which the contour starts to miss notes rather than merely
/// draw them with fewer points.
///
/// The hop is reported alongside the contour, because it is the time resolution
/// of everything drawn from it.
[[nodiscard]] SampleCount pitchHopFor(SampleCount frames, SampleCount base) {
    constexpr SampleCount kComfortableFrames = 1 << 21; // About 44 s at 48 kHz.
    SampleCount hop = base;
    while (hop < base * 8 && frames > kComfortableFrames * (hop / base)) {
        hop *= 2;
    }
    return hop;
}

/// The contour, tracked a second of audio at a time so it can be given up on.
///
/// By far the most expensive thing here -- on a two-minute passage it is an
/// order of magnitude more work than everything else put together -- and the
/// one place where a superseded run would otherwise hold the window still for
/// seconds. trackPitch() takes a buffer and runs to the end of it, so the only
/// way to make it interruptible is to hand it less at a time.
///
/// This produces exactly the contour one call would have produced, and that is
/// worth spelling out rather than hoping for. trackPitch places a frame at
/// every multiple of the hop for which window + longestLag samples remain, and
/// each frame's reading depends on nothing outside that span. So a chunk
/// starting at a multiple of the hop and carrying that span past its own end
/// gives its frames the identical samples they would have had, and the frames
/// kept from each chunk tile the whole exactly once. The alternative -- cutting
/// the buffer up and keeping whatever came back -- drops the frames straddling
/// every seam, and a contour with a hole every second is a contour that says
/// "unvoiced" where it means "not looked at".
[[nodiscard]] Result<std::vector<analysis::PitchPoint>>
trackPitchInChunks(ConstAudioBufferView audio, SampleRate rate,
                   const analysis::PitchSettings& settings, const CancellationToken& cancellation) {
    // Settings this cannot divide by are handed on whole, so that the refusal
    // comes from the tracker with its own account of what was wrong rather
    // than from arithmetic here.
    if (!(settings.hop > 0) || !(settings.minHz > 0.0) || !rate.isValid()) {
        return analysis::trackPitch(audio, rate, settings);
    }

    // A second of audio: about a tenth of a second of work at the settings
    // this is used with, which is short enough not to be felt and long enough
    // that rebuilding the tracker's transform tables per chunk is noise.
    const SampleCount chunk = std::max<SampleCount>(
        settings.hop, static_cast<SampleCount>(rate.hz()) / settings.hop * settings.hop);
    // What a frame reads past its own start, plus a hop so that the last frame
    // of a chunk is complete. Erring long costs a few frames that are then
    // discarded; erring short would lose them.
    const SampleCount span = settings.window +
                             static_cast<SampleCount>(std::ceil(rate.hz() / settings.minHz)) +
                             settings.hop;

    std::vector<analysis::PitchPoint> contour;
    for (SampleCount base = 0; base < audio.frames(); base += chunk) {
        if (cancellation.isCancelled()) {
            // What it had, which nothing will look at: a cancelled run is a
            // superseded one, and its generation check drops the result.
            return contour;
        }
        const bool last = base + chunk >= audio.frames();
        const SampleCount take =
            last ? audio.frames() - base : std::min(chunk + span, audio.frames() - base);
        auto part = analysis::trackPitch(audio.subRange(base, take), rate, settings);
        if (!part) {
            // Only the first chunk's refusal is the caller's answer. A later
            // one means the tail was shorter than a frame, which is not a
            // failure of anything.
            if (base == 0) {
                return part.error();
            }
            break;
        }
        // The chunk's own times start at zero; they are times in the passage.
        const double offset = static_cast<double>(base) / rate.hz();
        for (analysis::PitchPoint& point : part.value()) {
            point.timeSeconds += offset;
        }
        // Frames past the chunk's own end belong to the next chunk, which will
        // produce them itself.
        const std::size_t keep =
            last ? part.value().size()
                 : std::min(part.value().size(), static_cast<std::size_t>(chunk / settings.hop));
        contour.insert(contour.end(), part.value().begin(),
                       part.value().begin() + static_cast<std::ptrdiff_t>(keep));
    }
    return contour;
}

/// Run every stage the request asks for over `audio`, filling `out`.
///
/// Each stage's failure is recorded and the rest still run. A file with no key
/// in it still has a tempo, and a panel that threw away four answers because
/// the fifth could not be computed would be worse than the command line it is
/// replacing.
void analyseBuffer(ConstAudioBufferView audio, SampleRate rate, AnalysisPanel::Request request,
                   const CancellationToken& cancellation, MusicalAnalysis& out) {
    const auto keyFrames = std::min<SampleCount>(
        audio.frames(), static_cast<SampleCount>(AnalysisPanel::kKeySeconds * rate.hz()));
    out.keyFrames = keyFrames;

    if (auto estimate = analysis::detectKey(audio.subRange(0, keyFrames), rate); estimate) {
        out.key = std::move(estimate).value();
    } else {
        out.keyError = std::string{estimate.error().what()};
    }
    if (cancellation.isCancelled()) {
        return;
    }

    if (auto grid = analysis::trackTempo(audio, rate); grid) {
        out.tempo = std::move(grid).value();
    } else {
        out.tempoError = std::string{grid.error().what()};
    }
    if (cancellation.isCancelled()) {
        return;
    }

    if (auto bands = analysis::measureBands(audio, rate); bands) {
        out.bands = std::move(bands).value();
    } else {
        out.bandError = std::string{bands.error().what()};
    }
    if (cancellation.isCancelled()) {
        return;
    }

    if (request.pitch) {
        out.pitchRequested = true;
        analysis::PitchSettings settings;
        settings.hop = pitchHopFor(audio.frames(), settings.hop);
        out.pitchHop = settings.hop;
        if (auto contour = trackPitchInChunks(audio, rate, settings, cancellation); contour) {
            out.pitch = std::move(contour).value();
        } else {
            out.pitchError = std::string{contour.error().what()};
        }
    }
    if (cancellation.isCancelled()) {
        return;
    }

    if (request.room) {
        out.roomRequested = true;
        if (auto room = analysis::measureRoomAcoustics(audio, rate); room) {
            out.room = std::move(room).value();
        } else {
            out.roomError = std::string{room.error().what()};
        }
    }
}

} // namespace

AnalysisPanel::AnalysisPanel(QWidget* parent)
    : QWidget(parent), session_(std::make_shared<Session>()) {
    session_->panel = this;
    buildLayout();
    clear();
}

void AnalysisPanel::retireWorker() {
    if (cancellation_) {
        cancellation_->cancel();
    }
    if (worker_.joinable()) {
        if (finished_ && finished_->load()) {
            // Already stopped, so joining costs nothing and keeps the list
            // empty in the case that happens most: a worker that finished
            // while the user was thinking.
            worker_.join();
        } else {
            retired_.push_back(Retired{std::move(worker_), finished_});
        }
    }
    worker_ = std::thread{};
    finished_.reset();
    cancellation_.reset();
}

void AnalysisPanel::sweepRetired() {
    std::vector<Retired> running;
    for (Retired& one : retired_) {
        if (one.finished && one.finished->load()) {
            one.thread.join();
            continue;
        }
        running.push_back(std::move(one));
    }
    retired_ = std::move(running);
}

AnalysisPanel::~AnalysisPanel() {
    // Bumping the generation makes every outstanding result be discarded; the
    // workers hold the session by shared_ptr, so it stays alive to be read
    // after this panel is gone. Clearing the pointer under the mutex is what
    // makes that safe rather than merely likely -- see Session.
    session_->generation.fetch_add(1);
    retireWorker();
    // The one place waiting is right: the window is closing, and a worker
    // still running as the process tears down allocates through this project's
    // instrumented operator new while the runtime dismantles what that
    // instrumentation uses.
    for (Retired& one : retired_) {
        if (one.thread.joinable()) {
            one.thread.join();
        }
    }
    retired_.clear();
    const std::lock_guard<std::mutex> lock{session_->mutex};
    session_->panel = nullptr;
}

void AnalysisPanel::buildLayout() {
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

    coverage_ = new QLabel{this};
    coverage_->setWordWrap(true);
    coverage_->setStyleSheet("color: #6d7382;");
    grid->addWidget(coverage_, row++, 0, 1, 2);

    const auto addRow = [&](const QString& name, QLabel*& value) {
        auto* label = new QLabel{name, this};
        label->setStyleSheet("color: #8a8fa0;");
        value = new QLabel{this};
        value->setStyleSheet(QStringLiteral("color: %1;").arg(kFirm.name()));
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(label, row, 0);
        grid->addWidget(value, row, 1);
        ++row;
        return label;
    };

    // A caveat is a sentence, not a figure, so it takes the width of the panel
    // under the row it belongs to rather than being squeezed into the value
    // column beside it.
    const auto addCaveatRow = [&](QLabel*& caveat) {
        caveat = new QLabel{this};
        caveat->setWordWrap(true);
        caveat->setStyleSheet(QStringLiteral("color: %1;").arg(kDoubtful.name()));
        grid->addWidget(caveat, row++, 0, 1, 2);
        return caveat;
    };

    const auto addSeparator = [&](const QString& title) {
        auto* label = new QLabel{title, this};
        label->setStyleSheet("color: #5a6070; margin-top: 8px;");
        grid->addWidget(label, row++, 0, 1, 2);
        return label;
    };

    addSeparator(tr("KEY"));
    addRow(tr("Key"), key_);
    addCaveatRow(keyCaveat_);
    addRow(tr("Runner-up"), runnerUp_);
    addRow(tr("Tuning"), tuning_);

    addSeparator(tr("TEMPO"));
    addRow(tr("Tempo"), tempo_);
    addCaveatRow(tempoCaveat_);
    addRow(tr("Confidence"), tempoConfidence_);
    addRow(tr("First beat"), firstBeat_);
    addRow(tr("Beats"), beatCount_);

    pitchWidgets_ = {addSeparator(tr("PITCH")),  addRow(tr("Median"), pitch_),  pitch_,
                     addCaveatRow(pitchCaveat_), addRow(tr("Voiced"), voiced_), voiced_};
    // The standing limit rather than a caveat about this result: it is true of
    // every contour this tracker draws, and PitchTrack.h says it plainly.
    auto* monophonic = new QLabel{tr("one note at a time; on polyphony this follows one of them "
                                     "and does not say which"),
                                  this};
    monophonic->setWordWrap(true);
    monophonic->setStyleSheet("color: #6d7382;");
    grid->addWidget(monophonic, row++, 0, 1, 2);
    pitchWidgets_.push_back(monophonic);

    roomWidgets_ = {addSeparator(tr("ROOM")),
                    addRow(tr("EDT"), earlyDecay_),
                    earlyDecay_,
                    addRow(tr("T20"), t20_),
                    t20_,
                    addRow(tr("T30"), t30_),
                    t30_,
                    addRow(tr("C50"), clarity50_),
                    clarity50_,
                    addRow(tr("C80"), clarity80_),
                    clarity80_,
                    addRow(tr("D50"), definition50_),
                    definition50_,
                    addRow(tr("Centre time"), centreTime_),
                    centreTime_,
                    addRow(tr("Usable decay"), usableRange_),
                    usableRange_,
                    addCaveatRow(roomCaveat_)};

    grid->setRowStretch(row, 1);
    grid->setColumnStretch(1, 1);
}

void AnalysisPanel::clear() {
    // An analysis in flight belongs to whatever was open before. Bumping the
    // generation is what stops its result landing in a panel that has been
    // emptied, and retiring it stops it doing the rest of the work for an
    // answer nobody will see.
    session_->generation.fetch_add(1);
    retireWorker();
    sweepRetired();

    hasLatest_ = false;
    busy_ = false;
    latest_ = MusicalAnalysis{};
    heading_->setText(tr("nothing loaded"));
    coverage_->clear();
    for (QLabel* label : {key_, runnerUp_, tuning_, tempo_, tempoConfidence_, firstBeat_,
                          beatCount_, pitch_, voiced_}) {
        label->setText(QStringLiteral("--"));
        label->setStyleSheet(QStringLiteral("color: %1;").arg(kAbsent.name()));
    }
    for (QLabel* label : {keyCaveat_, tempoCaveat_, pitchCaveat_, roomCaveat_}) {
        label->clear();
        label->setVisible(false);
    }
    for (QWidget* widget : pitchWidgets_) {
        widget->setVisible(false);
    }
    for (QWidget* widget : roomWidgets_) {
        widget->setVisible(false);
    }
}

void AnalysisPanel::setRow(QLabel* value, QLabel* caveat, const Reading& reading) {
    const QColor colour = reading.certainty == Certainty::Firm       ? kFirm
                          : reading.certainty == Certainty::Doubtful ? kDoubtful
                                                                     : kAbsent;
    value->setText(QString::fromStdString(reading.value));
    value->setStyleSheet(QStringLiteral("color: %1;").arg(colour.name()));

    if (caveat == nullptr) {
        return;
    }
    caveat->setText(QString::fromStdString(reading.caveat));
    // An absent answer's caveat is the only thing there is to read, so it is
    // shown in the neutral colour -- the amber is for a number that is on
    // screen and should not be trusted, which is a different warning.
    caveat->setStyleSheet(
        QStringLiteral("color: %1;")
            .arg(reading.certainty == Certainty::Doubtful ? kDoubtful.name() : kAbsent.name()));
    caveat->setVisible(!reading.caveat.empty());
}

std::vector<AnalysisPanel::PanelRow> AnalysisPanel::shownRows() const {
    const auto row = [](const char* name, const QLabel* label) {
        return PanelRow{QString::fromUtf8(name), label->text(),
                        label->isVisibleTo(label->window())};
    };
    return {row("heading", heading_),
            row("coverage", coverage_),
            row("key", key_),
            row("key_caveat", keyCaveat_),
            row("runner_up", runnerUp_),
            row("tuning", tuning_),
            row("tempo", tempo_),
            row("tempo_caveat", tempoCaveat_),
            row("confidence", tempoConfidence_),
            row("first_beat", firstBeat_),
            row("beats", beatCount_),
            row("pitch", pitch_),
            row("pitch_caveat", pitchCaveat_),
            row("voiced", voiced_),
            row("edt", earlyDecay_),
            row("t20", t20_),
            row("t30", t30_),
            row("c50", clarity50_),
            row("c80", clarity80_),
            row("d50", definition50_),
            row("centre_time", centreTime_),
            row("usable_decay", usableRange_),
            row("room_caveat", roomCaveat_)};
}

void AnalysisPanel::deliver(const MusicalAnalysis& result, const QString& what) {
    busy_ = false;
    show(result, what);
    emit analysisFinished();
}

void AnalysisPanel::analyse(std::shared_ptr<const io::AudioSource> source, SampleIndex start,
                            SampleCount length, const QString& what, Request request) {
    // Before the generation is taken, because clear() takes one of its own.
    if (!source || length <= 0) {
        clear();
        emit analysisFinished();
        return;
    }

    const std::uint64_t mine = session_->generation.fetch_add(1) + 1;
    retireWorker();
    sweepRetired();

    busy_ = true;
    heading_->setText(tr("analysing %1…").arg(what));

    cancellation_ = std::make_shared<CancellationToken>();
    finished_ = std::make_shared<std::atomic<bool>>(false);

    worker_ = std::thread{[source = std::move(source), start, length, what, request, mine,
                           session = session_, cancellation = cancellation_, finished = finished_] {
        // First, so that it covers every way out of the lambda. The flag is
        // what tells the panel this thread can be joined without waiting, and
        // one set only on the successful path would leave a cancelled worker
        // in the set-aside list for ever.
        const Signal stopped{finished};

        const io::AudioFileInfo& info = source->info();
        MusicalAnalysis result;
        result.rate = info.sampleRate;
        result.start = start;
        result.requestedFrames = length;

        const auto wanted = std::min<SampleCount>(
            length, static_cast<SampleCount>(kMostSeconds * info.sampleRate.hz()));
        if (wanted > 0) {
            AudioBuffer audio{info.layout, wanted};
            const SampleCount read = readBlocks(*source, start, audio.view(), *cancellation);
            result.analysedFrames = read;
            if (read > 0 && !cancellation->isCancelled()) {
                // Narrowed to what was actually read. Every analysis below
                // takes the view's own frame count, so a short read left at
                // full width would have them all analysing the silence past
                // the end of it.
                analyseBuffer(audio.constView().subRange(0, read), info.sampleRate, request,
                              *cancellation, result);
            }
        }

        if (session->generation.load() != mine) {
            return;
        }
        const std::lock_guard<std::mutex> lock{session->mutex};
        if (session->panel == nullptr || session->generation.load() != mine) {
            return;
        }
        AnalysisPanel* panel = session->panel;
        QMetaObject::invokeMethod(
            panel,
            [panel, result, what, mine, session] {
                // Back on the main thread, and checked again because a newer
                // request may have been made while this one was queued.
                if (session->generation.load() != mine) {
                    return;
                }
                panel->deliver(result, what);
            },
            Qt::QueuedConnection);
    }};
}

void AnalysisPanel::show(const MusicalAnalysis& result, const QString& what) {
    latest_ = result;
    hasLatest_ = true;

    heading_->setText(what);
    const std::string coverage = coverageNote(result);
    coverage_->setText(coverage.empty() ? QString{}
                                        : tr("read the %1").arg(QString::fromStdString(coverage)));
    coverage_->setVisible(!coverage.empty());

    showKey(result);
    showTempo(result);
    showPitch(result);
    showRoom(result);
}

void AnalysisPanel::showKey(const MusicalAnalysis& result) {
    const Reading reading = keyReading(result.key, result.keyError);
    setRow(key_, keyCaveat_, reading);

    // The runner-up and the tuning are shown only when a key was named. Beside
    // "no key" they would be two more figures about an answer that does not
    // exist, which is the opposite of what the refusal is for.
    const bool named = reading.certainty != Certainty::None;
    runnerUp_->setText(named ? QString::fromStdString(runnerUpReading(result.key))
                             : QStringLiteral("--"));
    tuning_->setText(named ? QString::fromStdString(tuningReading(result.key))
                           : QStringLiteral("--"));
    for (QLabel* label : {runnerUp_, tuning_}) {
        label->setStyleSheet(
            QStringLiteral("color: %1;").arg(named ? kFirm.name() : kAbsent.name()));
    }
}

void AnalysisPanel::showTempo(const MusicalAnalysis& result) {
    const Reading reading = tempoReading(result.tempo, result.tempoError);
    setRow(tempo_, tempoCaveat_, reading);

    const bool found = result.tempo.valid;
    // The confidence is shown either way. Where a tempo was refused it is the
    // evidence for the refusal, which TempoTrack.h is explicit about -- except
    // where it is a zero that was never measured, and then there is nothing to
    // report.
    tempoConfidence_->setText(found || result.tempo.confidence > 0.0
                                  ? QStringLiteral("%1").arg(result.tempo.confidence, 0, 'f', 2)
                                  : QStringLiteral("--"));
    firstBeat_->setText(found ? QStringLiteral("%1 s").arg(result.tempo.firstBeatSeconds, 0, 'f', 3)
                              : QStringLiteral("--"));
    beatCount_->setText(found ? QString::number(result.tempo.beatSeconds.size())
                              : QStringLiteral("--"));
    for (QLabel* label : {tempoConfidence_, firstBeat_, beatCount_}) {
        label->setStyleSheet(
            QStringLiteral("color: %1;").arg(found ? kFirm.name() : kAbsent.name()));
    }
}

void AnalysisPanel::showPitch(const MusicalAnalysis& result) {
    for (QWidget* widget : pitchWidgets_) {
        widget->setVisible(result.pitchRequested);
    }
    if (!result.pitchRequested) {
        return;
    }

    const Reading reading = pitchReading(result.pitch, result.pitchRequested, result.pitchError);
    setRow(pitch_, pitchCaveat_, reading);

    std::size_t voiced = 0;
    for (const analysis::PitchPoint& point : result.pitch) {
        if (point.voiced) {
            ++voiced;
        }
    }
    voiced_->setText(result.pitch.empty()
                         ? QStringLiteral("--")
                         : tr("%1 of %2 frames").arg(voiced).arg(result.pitch.size()));
    voiced_->setStyleSheet(
        QStringLiteral("color: %1;").arg(result.pitch.empty() ? kAbsent.name() : kFirm.name()));
}

void AnalysisPanel::showRoom(const MusicalAnalysis& result) {
    for (QWidget* widget : roomWidgets_) {
        widget->setVisible(result.roomRequested);
    }
    if (!result.roomRequested) {
        return;
    }

    const Reading reading = roomReading(result.room, result.roomRequested, result.roomError);
    setRow(usableRange_, roomCaveat_, reading);

    const analysis::RoomAcoustics& room = result.room;
    // Every figure goes through roomSeconds, which prints "--" for one the
    // decay had no range for. A zero here would read as a very dead room.
    earlyDecay_->setText(QString::fromStdString(
        roomSeconds(room.valid && room.hasEarlyDecay, room.earlyDecaySeconds)));
    t20_->setText(QString::fromStdString(roomSeconds(room.valid && room.hasT20, room.t20Seconds)));
    t30_->setText(QString::fromStdString(roomSeconds(room.valid && room.hasT30, room.t30Seconds)));
    clarity50_->setText(room.valid ? QStringLiteral("%1 dB").arg(room.clarity50Db, 0, 'f', 2)
                                   : QStringLiteral("--"));
    clarity80_->setText(room.valid ? QStringLiteral("%1 dB").arg(room.clarity80Db, 0, 'f', 2)
                                   : QStringLiteral("--"));
    definition50_->setText(room.valid ? QStringLiteral("%1").arg(room.definition50, 0, 'f', 3)
                                      : QStringLiteral("--"));
    centreTime_->setText(room.valid ? QStringLiteral("%1 s").arg(room.centreTimeSeconds, 0, 'f', 4)
                                    : QStringLiteral("--"));

    for (QLabel* label :
         {earlyDecay_, t20_, t30_, clarity50_, clarity80_, definition50_, centreTime_}) {
        const bool measured = label->text() != QStringLiteral("--");
        label->setStyleSheet(
            QStringLiteral("color: %1;").arg(measured ? kFirm.name() : kAbsent.name()));
    }
}

} // namespace sa::ui
