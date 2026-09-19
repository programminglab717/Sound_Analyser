#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/io/AudioSource.h>

#include <QApplication>
#include <QElapsedTimer>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <numbers>
#include <thread>

/// A source that answers slowly and says how often it has been asked.
///
/// Both halves matter for the panels. The delay is what keeps a worker alive
/// long enough for the main thread to supersede it on purpose rather than by
/// luck; the count is how a test knows the worker really was inside the
/// measurement, instead of asserting against a sleep and hoping.
namespace sa::ui::test {

class PacedSource final : public io::AudioSource {
public:
    PacedSource(SampleCount frames, SampleRate rate, std::chrono::milliseconds perRead)
        : perRead_(perRead) {
        info_.sampleRate = rate;
        info_.layout = ChannelLayout::mono();
        info_.frameCount = frames;
        info_.format = io::SampleFormat::Float32;
    }

    [[nodiscard]] const io::AudioFileInfo& info() const noexcept override { return info_; }

    [[nodiscard]] Result<SampleCount> read(SampleIndex startFrame,
                                           AudioBufferView destination) const override {
        if (startFrame < 0 || destination.channelCount() != 1) {
            return Error{ErrorCode::InvalidArgument, "the paced source is one channel from zero"};
        }
        if (startFrame >= info_.frameCount || destination.isEmpty()) {
            return SampleCount{0};
        }
        const SampleCount count =
            std::min<SampleCount>(destination.frames(), info_.frameCount - startFrame);
        // A 1 kHz tone, synthesised rather than stored: it costs nothing to
        // hold a minute of it, and it gives the meters a level to report
        // instead of the silence that reads as a failed measurement.
        constexpr double kTwoPi = 2.0 * std::numbers::pi;
        float* out = destination.channel(0);
        for (SampleCount i = 0; i < count; ++i) {
            const double t = static_cast<double>(startFrame + i) / info_.sampleRate.hz();
            out[i] = static_cast<float>(0.5 * std::sin(kTwoPi * 1000.0 * t));
        }
        framesRead_.fetch_add(count);
        reads_.fetch_add(1);
        if (perRead_.count() > 0) {
            std::this_thread::sleep_for(perRead_);
        }
        return count;
    }

    [[nodiscard]] int reads() const noexcept { return reads_.load(); }

    [[nodiscard]] long long framesRead() const noexcept { return framesRead_.load(); }

private:
    io::AudioFileInfo info_;
    std::chrono::milliseconds perRead_;
    mutable std::atomic<int> reads_{0};
    mutable std::atomic<long long> framesRead_{0};
};

/// Wait for `ready` without letting Qt deliver anything.
///
/// The distinction is the whole point of the tests that use it: a result a
/// worker has posted is an event sitting in the queue, and the moment the main
/// thread pumps the queue it is delivered. So the tests that are about what
/// happens *before* delivery have to wait without pumping.
template <class Ready>
[[nodiscard]] bool waitWithoutEvents(Ready ready, int millis = 5000) {
    QElapsedTimer clock;
    clock.start();
    while (!ready()) {
        if (clock.elapsed() > millis) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return true;
}

/// Pump Qt's event queue until `ready`, which is how a posted result arrives.
template <class Ready>
[[nodiscard]] bool pumpUntil(Ready ready, int millis = 5000) {
    QElapsedTimer clock;
    clock.start();
    while (!ready()) {
        if (clock.elapsed() > millis) {
            return false;
        }
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    QApplication::processEvents();
    return true;
}

/// Pump everything the queue currently holds, several times over.
///
/// Once is not enough for these tests: a worker posts to the panel, and the
/// panel's own delivery can post again. Draining repeatedly is what makes "and
/// then nothing arrived" a statement about the queue rather than about how many
/// times it happened to be looked at.
inline void drainEvents(int rounds = 8) {
    for (int i = 0; i < rounds; ++i) {
        QApplication::processEvents(QEventLoop::AllEvents, 5);
        QApplication::sendPostedEvents();
    }
}

} // namespace sa::ui::test
