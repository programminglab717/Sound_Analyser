#include <sa/core/AudioBuffer.h>
#include <sa/transport/Player.h>

#include <algorithm>
#include <chrono>
#include <vector>

namespace sa::transport {

namespace {

/// Frames the worker renders per pass. Small enough to react to a stop
/// promptly, large enough that the per-render overhead is not the bottleneck.
constexpr SampleCount kRenderBlock = 4096;

} // namespace

Player::Player(std::unique_ptr<device::AudioDevice> device) : device_(std::move(device)) {
    const auto capacity = static_cast<std::size_t>(kBufferSeconds * device_->sampleRate().hz()) *
                          static_cast<std::size_t>(std::max(1, device_->outputChannels()));
    ring_ = std::make_unique<device::SpscRingBuffer>(capacity);
}

Result<Player> Player::create(std::unique_ptr<device::AudioDevice> device) {
    if (!device) {
        return Error{ErrorCode::InvalidArgument, "no device"};
    }
    if (device->isRunning()) {
        return Error{ErrorCode::InvalidArgument, "device is already running"};
    }
    if (device->outputChannels() <= 0) {
        return Error{ErrorCode::InvalidArgument, "device has no output channels"};
    }
    return Player{std::move(device)};
}

Player::Player(Player&& other) noexcept
    : device_(std::move(other.device_)), ring_(std::move(other.ring_)),
      scratch_(std::move(other.scratch_)) {
    // Moving a running player would leave the audio callback pointing at the
    // moved-from object, so the only safe move is of a stopped one. Stopping
    // here rather than asserting keeps the move usable in a Result.
    other.stop();
}

Player& Player::operator=(Player&& other) noexcept {
    if (this != &other) {
        stop();
        other.stop();
        device_ = std::move(other.device_);
        ring_ = std::move(other.ring_);
        scratch_ = std::move(other.scratch_);
    }
    return *this;
}

Player::~Player() {
    stop();
}

bool Player::isPlaying() const noexcept {
    return running_.load(std::memory_order_acquire);
}

SampleIndex Player::position() const noexcept {
    return rangeStart_.load(std::memory_order_relaxed) +
           framesPlayed_.load(std::memory_order_relaxed);
}

std::uint64_t Player::underruns() const noexcept {
    return underruns_.load(std::memory_order_relaxed);
}

Status Player::play(std::shared_ptr<const io::AudioSource> source, SampleIndex from,
                    SampleIndex to) {
    stop();

    if (!source) {
        return Error{ErrorCode::InvalidArgument, "no source"};
    }
    if (to <= from) {
        return Error{ErrorCode::InvalidArgument, "range is empty"};
    }
    if (source->info().channelCount() != device_->outputChannels()) {
        return Error{ErrorCode::InvalidArgument, "source channel count does not match the device"};
    }

    framesPlayed_.store(0, std::memory_order_relaxed);
    rangeStart_.store(from, std::memory_order_relaxed);
    rangeLength_.store(to - from, std::memory_order_relaxed);
    underruns_.store(0, std::memory_order_relaxed);
    sourceExhausted_.store(false, std::memory_order_relaxed);

    // Twice the advertised block, so a backend that occasionally delivers more
    // than it promised is serviced rather than clipped.
    const auto outputChannels = static_cast<std::size_t>(device_->outputChannels());
    scratch_.assign(
        static_cast<std::size_t>(std::max(1, device_->bufferFrames())) * 2 * outputChannels, 0.0f);
    ring_->reset();

    running_.store(true, std::memory_order_release);
    worker_ = std::thread{[this, source, from, to] { runWorker(source, from, to); }};

    // Everything the callback touches is either an atomic or the ring, both of
    // which are allocation-free and lock-free. Nothing here is captured by
    // value that needs destroying on the audio thread.
    auto status = device_->start([this](ConstAudioBufferView, AudioBufferView output) {
        const auto frames = static_cast<std::size_t>(output.frames());
        const auto channels = static_cast<std::size_t>(output.channelCount());
        if (frames == 0 || channels == 0) {
            return;
        }

        // Never read more than the scratch holds. A backend that hands over a
        // longer block than it advertised would otherwise overrun it, and the
        // callback cannot grow it.
        const std::size_t capacity = scratch_.size() / channels;
        const std::size_t serviceable = std::min(frames, capacity);
        const std::size_t got = ring_->read(scratch_.data(), serviceable * channels);
        const std::size_t framesGot = got / channels;

        // The ring holds interleaved samples; AudioBufferView is planar.
        for (std::size_t channel = 0; channel < channels; ++channel) {
            float* out = output.channel(static_cast<int>(channel));
            for (std::size_t frame = 0; frame < framesGot; ++frame) {
                out[frame] = scratch_[frame * channels + channel];
            }
            // Anything not filled is silence, including the tail of an
            // over-long block. output arrives uninitialised, so leaving it
            // would play whatever was there before.
            std::fill_n(out + framesGot, frames - framesGot, 0.0f);
        }

        framesPlayed_.fetch_add(static_cast<std::int64_t>(framesGot), std::memory_order_relaxed);

        // A short block once the source has been fully queued is the end of the
        // material, not a failure to keep up. Counting it would report an
        // underrun on every successful playback.
        if (framesGot < frames && !sourceExhausted_.load(std::memory_order_acquire)) {
            underruns_.fetch_add(1, std::memory_order_relaxed);
        }
    });

    if (!status) {
        running_.store(false, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
        return status;
    }
    return Status{};
}

void Player::stop() {
    running_.store(false, std::memory_order_release);
    if (device_) {
        device_->stop();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

void Player::runWorker(std::shared_ptr<const io::AudioSource> source, SampleIndex from,
                       SampleIndex to) {
    const int channels = source->info().channelCount();
    AudioBuffer block{source->info().layout, kRenderBlock};
    std::vector<float> interleaved(static_cast<std::size_t>(kRenderBlock) *
                                   static_cast<std::size_t>(channels));

    SampleIndex cursor = from;
    std::size_t pending = 0;
    std::size_t offset = 0;

    while (running_.load(std::memory_order_acquire)) {
        if (pending == 0) {
            if (cursor >= to) {
                sourceExhausted_.store(true, std::memory_order_release);
                break;
            }
            const SampleCount want = std::min<SampleCount>(kRenderBlock, to - cursor);
            AudioBufferView view = block.view().subRange(0, want);
            const auto read = source->read(cursor, view);
            if (!read || read.value() <= 0) {
                sourceExhausted_.store(true, std::memory_order_release);
                break;
            }
            const auto frames = static_cast<std::size_t>(read.value());
            for (std::size_t frame = 0; frame < frames; ++frame) {
                for (int channel = 0; channel < channels; ++channel) {
                    interleaved[frame * static_cast<std::size_t>(channels) +
                                static_cast<std::size_t>(channel)] = view.channel(channel)[frame];
                }
            }
            pending = frames * static_cast<std::size_t>(channels);
            offset = 0;
            cursor += read.value();
        }

        const std::size_t written = ring_->write(interleaved.data() + offset, pending);
        offset += written;
        pending -= written;

        if (written == 0) {
            // The ring is full, which is the normal steady state: the callback
            // drains it at real time and the worker has nothing to do until it
            // does. Sleeping a fraction of the buffer is far cheaper than
            // spinning and responsive enough to refill before it empties.
            std::this_thread::sleep_for(std::chrono::duration<double>(kBufferSeconds * 0.1));
        }
    }
}

} // namespace sa::transport
