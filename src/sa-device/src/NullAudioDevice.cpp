#include <sa/core/ChannelLayout.h>
#include <sa/core/RealtimeGuard.h>
#include <sa/device/NullAudioDevice.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace sa::device {

namespace {

/// The device whose render loop is running on *this* thread, if any. It lets
/// stop() recognise a call made from inside the callback -- where joining would
/// mean joining ourselves -- with no shared state and so nothing to race on.
thread_local const NullAudioDevice* tlsRenderingDevice = nullptr;

std::chrono::nanoseconds blockPeriod(SampleRate rate, int frames) noexcept {
    if (!rate.isValid() || frames <= 0) {
        return std::chrono::nanoseconds::zero();
    }
    const double nanoseconds = 1.0e9 * static_cast<double>(frames) / rate.hz();
    return std::chrono::nanoseconds{static_cast<std::int64_t>(nanoseconds)};
}

} // namespace

NullAudioDevice::NullAudioDevice(AudioDeviceDescription description, AudioDeviceConfig config,
                                 Pacing pacing)
    : description_(std::move(description)), config_(config), pacing_(pacing) {
    // Sized here, on the calling thread, because the render loop must not
    // allocate -- see the real-time contract on AudioCallback. A direction with
    // no channels still keeps the block's frame count, so both views handed to
    // the callback agree on how long the block is.
    const auto frames =
        config_.bufferFrames > 0 ? static_cast<SampleCount>(config_.bufferFrames) : SampleCount{0};
    input_.resize(ChannelLayout::discrete(config_.inputChannels), frames);
    output_.resize(ChannelLayout::discrete(config_.outputChannels), frames);
}

NullAudioDevice::~NullAudioDevice() {
    stop();

    // A callback that stopped its own device leaves the thread running but
    // unwinding; it has to be joined before the members it touches die.
    const std::lock_guard<std::mutex> lock{lifecycle_};
    joinRenderThread();
}

Status NullAudioDevice::start(AudioCallback callback) {
    if (!config_.isValid()) {
        return Error{ErrorCode::InvalidArgument, "audio device configuration is not usable"};
    }
    if (!callback) {
        return Error{ErrorCode::InvalidArgument, "audio callback is empty"};
    }

    const std::lock_guard<std::mutex> lock{lifecycle_};
    if (running_.load(std::memory_order_acquire)) {
        // The caller's callback cannot be honoured -- swapping a std::function
        // under a live audio thread is a data race -- so say so rather than
        // returning success and silently keeping the old one. A caller whose
        // new callback never fires, against a success return, has a bug that
        // surfaces months later and points nowhere near here.
        return Error{ErrorCode::InvalidArgument,
                     "device is already running; stop() before starting with a new callback"};
    }

    // A previous run may have ended without being joined: a callback that
    // stopped its own device does exactly that.
    joinRenderThread();

    // Silence is written once rather than per block. The callback only ever
    // sees a const view of it, so nothing can dirty it between blocks, and
    // re-memsetting a large buffer every block would be pure waste.
    input_.clear();

    callback_ = std::move(callback);
    running_.store(true, std::memory_order_release);

    try {
        thread_ = std::thread{[this] { renderLoop(); }};
    } catch (...) {
        // Thread creation is the one genuinely throwing call in this file and
        // it does fail under resource exhaustion. Nothing escapes our API.
        running_.store(false, std::memory_order_release);
        callback_ = nullptr;
        return Error{ErrorCode::IoFailure, "could not start the audio render thread"};
    }

    return Status{};
}

void NullAudioDevice::stop() noexcept {
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
    joinRenderThread();
}

void NullAudioDevice::joinRenderThread() noexcept {
    if (thread_.joinable()) {
        thread_.join();
    }
}

void NullAudioDevice::renderLoop() {
    tlsRenderingDevice = this;

    const std::chrono::nanoseconds period = blockPeriod(config_.sampleRate, config_.bufferFrames);
    const bool paced = pacing_ == Pacing::RealTime && period > std::chrono::nanoseconds::zero();
    auto deadline = std::chrono::steady_clock::now();

    while (running_.load(std::memory_order_acquire)) {
        if (paced) {
            // An absolute deadline advanced by one period, rather than sleeping
            // for a period: scheduling overshoot then stays a one-block error
            // instead of accumulating into drift over a long run.
            deadline += period;
            std::this_thread::sleep_until(deadline);
        }

        {
            // Marks the thread so that an allocation inside the callback is
            // counted, which is what makes real-time violations visible in
            // tests rather than in the field.
            const rt::ScopedAudioThread guard;
            callback_(input_.constView(), output_.view());
        }

        // Whatever the callback produced goes nowhere: this device models a
        // sink that is always ready and never heard.
        blocksRendered_.fetch_add(1, std::memory_order_release);
    }

    tlsRenderingDevice = nullptr;
}

std::vector<AudioDeviceDescription> NullAudioBackend::enumerate() const {
    AudioDeviceDescription description;
    description.id = std::string{kDeviceId};
    description.name = "Null device (silence)";
    description.maxInputChannels = 2;
    description.maxOutputChannels = 2;
    description.sampleRates = {kSampleRate44100, kSampleRate48000, kSampleRate96000};
    description.defaultBufferFrames = kDefaultBufferFrames;

    // Never flagged default: if a real device exists it should win, and the
    // manager falls back to this one anyway when nothing else is there.
    description.isDefault = false;

    return {std::move(description)};
}

Result<std::unique_ptr<AudioDevice>>
NullAudioBackend::open(const AudioDeviceDescription& description, const AudioDeviceConfig& config) {
    if (description.id != kDeviceId) {
        return Error{ErrorCode::NotFound, "unknown null-backend device id: " + description.id};
    }
    if (!config.isValid()) {
        return Error{ErrorCode::InvalidArgument, "audio device configuration is not usable"};
    }

    return std::unique_ptr<AudioDevice>{
        std::make_unique<NullAudioDevice>(description, config, pacing_)};
}

} // namespace sa::device
