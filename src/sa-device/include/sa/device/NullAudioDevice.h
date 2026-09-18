#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/device/AudioDevice.h>
#include <sa/device/AudioDeviceBackend.h>
#include <sa/device/AudioDeviceDescription.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace sa::device {

/// A device with no hardware behind it: input is silence, output is discarded.
///
/// It exists for three reasons, in order of importance. It makes the whole
/// device layer testable without a sound card, which is the only way the
/// real-time rules above get checked in CI. It gives headless runs -- batch
/// analysis, sa-cli, a build machine with no audio service -- a device to open
/// instead of a special "no device" code path threaded through the engine. And
/// it is the fallback when every real device fails to open, so the application
/// starts rather than dies.
///
/// The callback runs on an ordinary std::thread, not a real-time one. Timing is
/// therefore approximate and it will not catch priority-inversion bugs -- but
/// it is marked as the audio thread (sa/core/RealtimeGuard.h), so allocation
/// inside the callback is still detected.
class NullAudioDevice final : public AudioDevice {
public:
    enum class Pacing {
        /// Sleep between blocks so callbacks arrive at the simulated sample
        /// rate. What you want when testing behaviour over time.
        RealTime,

        /// Render flat out. Offline work and fast tests; a free-running loop
        /// would be a spinning core in a real application.
        FreeRun,
    };

    NullAudioDevice(AudioDeviceDescription description, AudioDeviceConfig config,
                    Pacing pacing = Pacing::RealTime);

    ~NullAudioDevice() override;

    [[nodiscard]] const AudioDeviceDescription& description() const noexcept override {
        return description_;
    }

    [[nodiscard]] Status start(AudioCallback callback) override;

    void stop() noexcept override;

    [[nodiscard]] bool isRunning() const noexcept override {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] SampleRate sampleRate() const noexcept override { return config_.sampleRate; }

    [[nodiscard]] int bufferFrames() const noexcept override { return config_.bufferFrames; }

    [[nodiscard]] int inputChannels() const noexcept override { return config_.inputChannels; }

    [[nodiscard]] int outputChannels() const noexcept override { return config_.outputChannels; }

    /// Callbacks completed since construction. Lets a headless caller or a test
    /// wait for progress without the callback having to count for itself.
    [[nodiscard]] std::size_t blocksRendered() const noexcept {
        return blocksRendered_.load(std::memory_order_acquire);
    }

private:
    void renderLoop();

    void joinRenderThread() noexcept;

    AudioDeviceDescription description_;
    AudioDeviceConfig config_;
    Pacing pacing_;

    /// Allocated once, here, so that the render loop itself never does.
    AudioBuffer input_;
    AudioBuffer output_;

    AudioCallback callback_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<std::size_t> blocksRendered_{0};

    /// Serialises start/stop against each other. Never taken by the render
    /// thread, so a callback calling stop() cannot deadlock against a join.
    mutable std::mutex lifecycle_;
};

/// Backend that offers exactly one NullAudioDevice.
///
/// Always registered last, so there is always something to open.
class NullAudioBackend final : public AudioDeviceBackend {
public:
    static constexpr std::string_view kBackendName = "null";

    /// Id of the single device this backend offers.
    static constexpr std::string_view kDeviceId = "null:silence";

    explicit NullAudioBackend(NullAudioDevice::Pacing pacing = NullAudioDevice::Pacing::RealTime)
        : pacing_(pacing) {}

    [[nodiscard]] std::string_view name() const noexcept override { return kBackendName; }

    [[nodiscard]] std::vector<AudioDeviceDescription> enumerate() const override;

    [[nodiscard]] Result<std::unique_ptr<AudioDevice>>
    open(const AudioDeviceDescription& description, const AudioDeviceConfig& config) override;

private:
    NullAudioDevice::Pacing pacing_;
};

} // namespace sa::device
