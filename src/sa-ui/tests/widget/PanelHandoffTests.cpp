#include "PacedSource.h"

#include <sa/ui/AnalysisPanel.h>
#include <sa/ui/LoudnessPanel.h>

#include <QElapsedTimer>
#include <QEvent>
#include <QObject>
#include <QString>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using namespace sa;
using namespace sa::ui;
using namespace sa::ui::test;
using namespace std::chrono_literals;

namespace {

constexpr SampleRate kRate{48000.0};

/// Counts the queued calls that reach a panel.
///
/// A worker delivers by posting one of these to the panel, so the count is how
/// a test can tell the two ways of dropping a superseded result apart: the
/// worker's own check, which stops the post ever being made, and the panel's,
/// which refuses one that was already in the queue. Both are correct, and only
/// one of them is the case the metering panel was fixed for -- so the test that
/// is about that case asserts it actually happened rather than hoping.
class CallCounter : public QObject {
public:
    [[nodiscard]] int calls() const noexcept { return calls_; }

    void reset() noexcept { calls_ = 0; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::MetaCall) {
            ++calls_;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    int calls_ = 0;
};

/// One second of audio, which is two blocks of the metering panel's streaming
/// pass and one of its exact true-peak pass.
constexpr SampleCount kSecond = 48000;

/// What the metering worker reads before it has anything to deliver: the whole
/// range twice, once streaming and once for the exact true peak.
constexpr long long kFullyRead = 2 * static_cast<long long>(kSecond);

/// A tenth of a second, for the one test that has to wait for a whole
/// measurement to finish before it does anything.
constexpr long long kProbe = kSecond / 10;

} // namespace

TEST_CASE("A measurement nothing supersedes reaches the panel", "[ui][panel][handoff]") {
    // The control for everything below. Without it, a test that asserts
    // "nothing was delivered" would pass on a panel that never delivers at all,
    // which is the one way these could all be green and worthless.
    // The counter outlives the panel it watches, which is the ordering Qt
    // needs from a filter.
    CallCounter counter;
    LoudnessPanel panel;
    panel.installEventFilter(&counter);

    int finished = 0;
    QObject::connect(&panel, &LoudnessPanel::measurementFinished, [&finished] { ++finished; });

    auto source = std::make_shared<PacedSource>(kSecond, kRate, 0ms);
    panel.measure(source, 0, kSecond, QStringLiteral("a tone"));
    REQUIRE(panel.busy());

    REQUIRE(pumpUntil([&panel] { return !panel.busy(); }));
    REQUIRE(finished == 1);
    REQUIRE(panel.latest() != nullptr);
    REQUIRE(panel.latest()->loudness.gatedBlockCount > 0);
    // The delivery really did come through the queue, which is what makes the
    // counter meaningful in the tests below.
    REQUIRE(counter.calls() >= 1);
}

TEST_CASE("Clearing the panel drops a measurement that is still running", "[ui][panel][handoff]") {
    LoudnessPanel panel;
    int finished = 0;
    QObject::connect(&panel, &LoudnessPanel::measurementFinished, [&finished] { ++finished; });

    // Slow enough that the worker is certainly still inside measureStreaming
    // when the panel is emptied under it.
    auto source = std::make_shared<PacedSource>(kSecond * 30, kRate, 20ms);
    panel.measure(source, 0, kSecond * 30, QStringLiteral("a long tone"));

    // Not a sleep: the worker has demonstrably read something, so this is the
    // measurement in flight and not a measurement about to start.
    REQUIRE(waitWithoutEvents([&source] { return source->reads() >= 2; }));
    REQUIRE(source->framesRead() < kFullyRead * 30);

    panel.clear();
    const int readsWhenCleared = source->reads();
    drainEvents();

    REQUIRE(finished == 0);
    REQUIRE(panel.latest() == nullptr);
    REQUIRE_FALSE(panel.busy());

    // And the worker really stopped, rather than being left to finish a
    // measurement whose answer is already unwanted. clear() joins it, so by the
    // time it returned there was nothing left to read another block.
    std::this_thread::sleep_for(100ms);
    REQUIRE(source->reads() == readsWhenCleared);
}

TEST_CASE("Clearing the panel refuses a measurement that was already in the post",
          "[ui][panel][handoff]") {
    // The case the metering panel was fixed for, and the one an argument by
    // construction is least able to settle: the worker has finished, passed its
    // own generation check and posted, and the result is sitting in the queue
    // when the document is closed. Nothing in the code makes that ordering
    // happen, so the test runs the scenario until it has watched the queued
    // call arrive after the clear, and requires that it watched one.

    // First, how long this machine takes to measure the probe from end to end.
    // The scenario needs the worker to have finished before the panel is
    // cleared, and a fixed sleep is a guess that is too short on a loaded
    // machine and wasted time on a quick one.
    qint64 whole = 0;
    {
        LoudnessPanel panel;
        auto source = std::make_shared<PacedSource>(kProbe, kRate, 0ms);
        QElapsedTimer clock;
        clock.start();
        panel.measure(source, 0, kProbe, QStringLiteral("a tone"));
        REQUIRE(pumpUntil([&panel] { return !panel.busy(); }));
        REQUIRE(panel.latest() != nullptr);
        whole = clock.elapsed();
    }
    const std::chrono::milliseconds settle{std::max<qint64>(200, whole * 3)};

    int observed = 0;
    for (int attempt = 0; attempt < 5 && observed == 0; ++attempt) {
        CallCounter counter;
        LoudnessPanel panel;

        int finished = 0;
        QObject::connect(&panel, &LoudnessPanel::measurementFinished, [&finished] { ++finished; });

        auto source = std::make_shared<PacedSource>(kProbe, kRate, 0ms);
        panel.measure(source, 0, kProbe, QStringLiteral("a tone"));

        // Read through twice over is the streaming pass and the exact true-peak
        // pass both done; the settle covers the arithmetic after them and the
        // post itself.
        REQUIRE(waitWithoutEvents([&source] { return source->framesRead() >= 2 * kProbe; }));
        std::this_thread::sleep_for(settle);

        // From here the queue is watched. A filter runs when an event is
        // delivered rather than when it is posted, so anything it counts below
        // is a call that was already in the queue when the panel was emptied.
        panel.installEventFilter(&counter);
        panel.clear();
        drainEvents();
        observed += counter.calls() > 0 ? 1 : 0;

        INFO("attempt " << attempt << ": " << counter.calls() << " queued calls after the clear");
        REQUIRE(finished == 0);
        REQUIRE(panel.latest() == nullptr);
    }
    // Not "no result was delivered", which the test above already establishes,
    // but "a result was delivered to the panel and the panel refused it".
    REQUIRE(observed == 1);
}

TEST_CASE("A burst of measurements leaves the last one on the panel", "[ui][panel][handoff]") {
    LoudnessPanel panel;
    int finished = 0;
    QObject::connect(&panel, &LoudnessPanel::measurementFinished, [&finished] { ++finished; });

    // Five requests in a row, each superseding the one before it, which is what
    // dragging a selection does. Four of them must leave nothing behind.
    std::vector<std::shared_ptr<PacedSource>> sources;
    for (int i = 0; i < 5; ++i) {
        const auto frames = static_cast<SampleCount>(kSecond / 2 + i * 1000);
        sources.push_back(std::make_shared<PacedSource>(frames, kRate, 1ms));
        panel.measure(sources.back(), 0, frames, QStringLiteral("selection %1").arg(i));
    }

    REQUIRE(pumpUntil([&panel] { return !panel.busy(); }));
    REQUIRE(finished == 1);
    REQUIRE(panel.latest() != nullptr);
    // The measurement that survived is the last one, which is the only one
    // whose length it can have been taken over.
    REQUIRE(panel.latest()->statistics.frames == sources.back()->info().frameCount);
}

TEST_CASE("A superseded analysis never reaches the panel", "[ui][panel][handoff]") {
    // The analysis panel's hand-off is not the metering panel's: a superseded
    // worker is cancelled and set aside rather than waited for, because nothing
    // inside detectKey or trackTempo stops halfway. What the two share is the
    // generation, and the claim is the same -- exactly one of a burst of
    // requests reaches the panel, and it is the last.
    AnalysisPanel panel;
    int finished = 0;
    QObject::connect(&panel, &AnalysisPanel::analysisFinished, [&finished] { ++finished; });

    std::vector<std::shared_ptr<PacedSource>> sources;
    QString last;
    for (int i = 0; i < 4; ++i) {
        const auto frames = static_cast<SampleCount>(kSecond + i * kSecond / 4);
        sources.push_back(std::make_shared<PacedSource>(frames, kRate, 2ms));
        last = QStringLiteral("passage %1").arg(i);
        panel.analyse(sources.back(), 0, frames, last, AnalysisPanel::Request{true, false});
        // Long enough for each worker to be somewhere inside a stage when the
        // next one supersedes it, rather than all four starting at once.
        std::this_thread::sleep_for(15ms);
    }

    REQUIRE(pumpUntil([&panel] { return !panel.busy(); }, 60000));
    REQUIRE(finished == 1);
    REQUIRE(panel.latest() != nullptr);
    REQUIRE(panel.latest()->requestedFrames == sources.back()->info().frameCount);

    // And the panel says so in the row a reader looks at, not merely in the
    // result it kept.
    bool sawHeading = false;
    for (const AnalysisPanel::PanelRow& row : panel.shownRows()) {
        if (row.name == QStringLiteral("heading")) {
            REQUIRE(row.text == last);
            sawHeading = true;
        }
    }
    REQUIRE(sawHeading);
}

TEST_CASE("Clearing the analysis panel drops the analysis in flight", "[ui][panel][handoff]") {
    AnalysisPanel panel;
    int finished = 0;
    QObject::connect(&panel, &AnalysisPanel::analysisFinished, [&finished] { ++finished; });

    auto source = std::make_shared<PacedSource>(kSecond * 20, kRate, 20ms);
    panel.analyse(source, 0, kSecond * 20, QStringLiteral("a long passage"),
                  AnalysisPanel::Request{true, false});
    REQUIRE(waitWithoutEvents([&source] { return source->reads() >= 2; }));

    panel.clear();

    // The analysis panel sets a superseded worker aside rather than waiting for
    // it, so the reading stops at the worker's next block boundary and not at
    // the instant clear() returns. It does have to stop, and short of the end:
    // a worker carrying on through an analysis nobody will see is the whole
    // cost the cancellation token exists to avoid.
    //
    // Waiting for that here is also what makes the assertions below mean
    // anything. Draining the queue the moment clear() returned would drain it
    // before the worker had reached the point where it decides whether to post
    // at all, and "nothing arrived" would be a statement about how quickly the
    // test ran.
    bool stopped = false;
    for (int i = 0; i < 40 && !stopped; ++i) {
        const int before = source->reads();
        std::this_thread::sleep_for(50ms);
        stopped = source->reads() == before;
    }
    REQUIRE(stopped);
    REQUIRE(source->framesRead() < kSecond * 20);

    drainEvents();
    REQUIRE(finished == 0);
    REQUIRE(panel.latest() == nullptr);
    REQUIRE_FALSE(panel.busy());
}
