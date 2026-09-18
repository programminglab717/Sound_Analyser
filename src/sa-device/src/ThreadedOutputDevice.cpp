#include <sa/device/ThreadedOutputDevice.h>

#include <chrono>
#include <utility>

namespace sa::device {

namespace {

/// The device whose render loop is running on *this* thread, if any. It lets
/// stop() recognise a call made from inside the callback -- where joining would
/// mean joining ourselves -- with no shared state and so nothing to race on.
/// The same trick NullAudioDevice uses, for the same reason.
thread_local const ThreadedOutputDevice* tlsRenderingDevice = nullptr;

} // namespace

ThreadedOutputDevice::ThreadedOutputDevice(AudioDeviceDescription description,
                                           AudioDeviceConfig config)
    : description_(std::move(description)), config_(config) {
    // Seeded from the negotiated rate so that sampleRate() answers sensibly
    // before the stream has had a chance to say what it really got.
    actualSampleRateHz_.store(config_.sampleRate.hz(), std::memory_order_release);
}

ThreadedOutputDevice::~ThreadedOutputDevice() {
    // Deliberately not stop(): by the time a base destructor runs, the derived
    // part of the object is gone, and stop() dispatches to wakeStream(), which
    // would be a pure virtual call. Every concrete backend calls stop() from
    // its own destructor -- that is what actually ends the thread -- so all
    // this has to do is make certain nothing is left running if one forgets.
    running_.store(false, std::memory_order_release);
    {
        const std::lock_guard<std::mutex> lock{handshake_};
        handshakeSignal_.notify_all();
    }

    const std::lock_guard<std::mutex> lock{lifecycle_};
    joinRenderThread();
}

Status ThreadedOutputDevice::start(AudioCallback callback) {
    if (!config_.isValid()) {
        return Error{ErrorCode::InvalidArgument, "audio device configuration is not usable"};
    }
    if (config_.outputChannels <= 0) {
        return Error{ErrorCode::InvalidArgument, "a playback device needs at least one output"};
    }
    if (!callback) {
        return Error{ErrorCode::InvalidArgument, "audio callback is empty"};
    }

    const std::lock_guard<std::mutex> lock{lifecycle_};
    if (running_.load(std::memory_order_acquire)) {
        // Same refusal, and the same reason, as NullAudioDevice: swapping a
        // std::function under a live audio thread is a data race, so saying no
        // beats returning success and quietly keeping the old callback.
        return Error{ErrorCode::InvalidArgument,
                     "device is already running; stop() before starting with a new callback"};
    }

    // A previous run may have ended without being joined: a callback that
    // stopped its own device, or a start() that timed out, leaves exactly that.
    joinRenderThread();

    // Allocated here, on the calling thread, so the render loop never does.
    blocker_.prepare(config_.outputChannels, config_.bufferFrames);
    callback_ = std::move(callback);

    {
        const std::lock_guard<std::mutex> handshakeLock{handshake_};
        startupSettled_ = false;
        startupStatus_ = Status{};
    }

    running_.store(true, std::memory_order_release);

    try {
        thread_ = std::thread{[this] { renderThreadMain(); }};
    } catch (...) {
        // Thread creation is the one genuinely throwing call in this file and
        // it does fail under resource exhaustion. Nothing escapes our API.
        running_.store(false, std::memory_order_release);
        callback_ = nullptr;
        return Error{ErrorCode::IoFailure, "could not start the audio render thread"};
    }

    std::unique_lock<std::mutex> handshakeLock{handshake_};
    const bool settled =
        handshakeSignal_.wait_for(handshakeLock, std::chrono::milliseconds{kStreamStartTimeoutMs},
                                  [this] { return startupSettled_; });
    if (!settled) {
        handshakeLock.unlock();
        running_.store(false, std::memory_order_release);
        wakeStream();

        // Deliberately not joined. The thread is inside a driver call that will
        // return in its own time, and waiting for it here would undo the
        // timeout we just spent. The next stop(), start() or destructor joins
        // it, exactly as for a callback that stopped its own device.
        return Error{ErrorCode::IoFailure, "the audio device did not start within the timeout"};
    }

    const Status startup = startupStatus_;
    handshakeLock.unlock();

    if (!startup.ok()) {
        running_.store(false, std::memory_order_release);
        joinRenderThread();
        callback_ = nullptr;
    }
    return startup;
}

void ThreadedOutputDevice::stop() noexcept {
    // Requested before anything else, so the loop is already unwinding by the
    // time we get as far as joining it.
    running_.store(false, std::memory_order_release);

    if (tlsRenderingDevice == this) {
        // Called from inside our own callback. Joining here would deadlock
        // against ourselves, and blocking on the lifecycle mutex could deadlock
        // against a thread that holds it while joining us. The loop exits after
        // the current block; whoever calls stop(), start() or the destructor
        // next does the join.
        return;
    }

    const std::lock_guard<std::mutex> lock{lifecycle_};
    {
        // Cuts short a restart that is waiting out its backoff.
        const std::lock_guard<std::mutex> handshakeLock{handshake_};
        handshakeSignal_.notify_all();
    }
    wakeStream();
    joinRenderThread();
}

void ThreadedOutputDevice::joinRenderThread() noexcept {
    if (thread_.joinable()) {
        thread_.join();
    }
}

void ThreadedOutputDevice::publishStartup(const Status& status) noexcept {
    const std::lock_guard<std::mutex> lock{handshake_};
    startupStatus_ = status;
    startupSettled_ = true;
    handshakeSignal_.notify_all();
}

void ThreadedOutputDevice::waitBeforeRestart() noexcept {
    std::unique_lock<std::mutex> lock{handshake_};
    handshakeSignal_.wait_for(lock, std::chrono::milliseconds{kStreamRestartDelayMs},
                              [this] { return !keepRunning(); });
}

void ThreadedOutputDevice::renderThreadMain() {
    tlsRenderingDevice = this;
    configureRenderThread();

    const Status opened = openStream();
    publishStartup(opened);

    // A first open that fails is start() failing. Retrying here would leave the
    // caller holding an error while a stream it was told it would not get
    // quietly appears behind it.
    while (opened.ok() && keepRunning()) {
        runStream();
        closeStream();
        if (!keepRunning()) {
            break;
        }

        // runStream() came back with the device still meant to be playing, so
        // the stream broke rather than ended: the endpoint was invalidated, or
        // the default device moved. Rebuild it.
        blocker_.reset();
        waitBeforeRestart();
        if (!keepRunning()) {
            break;
        }

        if (openStream().ok()) {
            restarts_.fetch_add(1, std::memory_order_release);
        } else {
            // Usually a device mid-reconnect. Keep trying rather than give up:
            // an application that has gone permanently silent until it is
            // restarted is a worse outcome than a few seconds of nothing, and
            // waitBeforeRestart() bounds how hard we try.
            closeStream();
        }
    }

    // Unconditional, and after a loop that may not have run at all: a stop()
    // landing between the stream coming up and the first pass would otherwise
    // leave it open for good. closeStream() is required to tolerate being
    // called on an already-closed stream precisely so that this can be
    // unconditional -- and it has to happen before the teardown hook, because
    // on Windows that hook drops the apartment the stream's objects live in.
    closeStream();
    releaseRenderThread();
    tlsRenderingDevice = nullptr;
}

} // namespace sa::device
