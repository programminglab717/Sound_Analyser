#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/device/AudioDeviceDescription.h>

#include <functional>

namespace sa::device {

/// Block-size bounds the whole layer agrees on. The floor keeps a caller from
/// asking for a per-sample callback -- the per-block overhead would dominate
/// and no driver would honour it anyway; the ceiling is about three seconds at
/// 96 kHz, well past any sane latency, and stops a bad config from demanding a
/// huge allocation.
inline constexpr int kMinBufferFrames = 16;
inline constexpr int kMaxBufferFrames = 1 << 18;

/// Block size used when neither caller nor device expresses a preference.
/// ~10.7 ms at 48 kHz: comfortably safe on a shared-mode endpoint, low enough
/// for monitoring.
inline constexpr int kDefaultBufferFrames = 512;

/// What we ask a device for when opening it. A backend may not be able to
/// honour it exactly; the opened AudioDevice reports what it actually got.
struct AudioDeviceConfig {
    SampleRate sampleRate = kSampleRate48000;
    int inputChannels = 0;
    int outputChannels = 2;

    /// Frames per callback. The single biggest latency knob we expose, and the
    /// one users will get wrong, so backends clamp rather than fail.
    int bufferFrames = kDefaultBufferFrames;

    [[nodiscard]] bool isValid() const noexcept {
        return sampleRate.isValid() && inputChannels >= 0 && outputChannels >= 0 &&
               inputChannels <= kMaxChannels && outputChannels <= kMaxChannels &&
               (inputChannels > 0 || outputChannels > 0) && bufferFrames > 0;
    }
};

/// The audio callback. Invoked once per block, on the real-time thread.
///
/// `input` holds the frames just captured and `output` must be filled with the
/// frames to play. A direction the device does not have arrives as a view with
/// zero channels; both views always report the same frame count, so either can
/// be used to size the block. `output` arrives uninitialised, exactly as a real
/// driver hands it over -- a callback that does not fill every frame will play
/// whatever was there before.
///
/// REAL-TIME CONTRACT. This runs on a thread the OS scheduler will not wait
/// for: miss the deadline and the user hears it. Inside this callback you must
/// not allocate or free, take a lock, touch a file or socket, throw, log, wait
/// on another thread, or call anything that might do those things behind your
/// back (std::string, std::function assignment, std::shared_ptr destruction,
/// any container that can grow). Move audio across the boundary with
/// SpscRingBuffer; hand anything that needs destroying to the deferred deleter
/// (docs/03-architecture.md §3). Debug builds trap the allocation case via
/// sa/core/RealtimeGuard.h -- the rest is on the author.
///
/// The callable itself is copied into the device by start(), on the calling
/// thread, so capturing state is fine; only its *invocation* is real-time.
using AudioCallback = std::function<void(ConstAudioBufferView input, AudioBufferView output)>;

/// An open audio device.
///
/// Obtained from AudioDeviceManager, never constructed directly by callers:
/// that is what lets a real WASAPI device replace the null one without a single
/// call site changing.
class AudioDevice {
public:
    virtual ~AudioDevice() = default;

    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    [[nodiscard]] virtual const AudioDeviceDescription& description() const noexcept = 0;

    /// Begins calling `callback` on the real-time thread. Idempotent: starting
    /// an already-running device succeeds and changes nothing, including the
    /// installed callback -- swapping a callback under a live audio thread
    /// cannot be done safely, so stop() first.
    [[nodiscard]] virtual Status start(AudioCallback callback) = 0;

    /// Stops the stream. Idempotent, and safe to call on a device that never
    /// started. Once it returns, the callback is not running and will not run
    /// again, so state the callback captured can be destroyed -- that guarantee
    /// is the whole reason this is not just a flag.
    ///
    /// The one exception is calling stop() from inside the callback: that
    /// requests the stop and returns immediately, because a thread cannot join
    /// itself. The thread unwinds after the current block.
    virtual void stop() noexcept = 0;

    [[nodiscard]] virtual bool isRunning() const noexcept = 0;

    /// The rate actually in use, which may differ from the one requested.
    [[nodiscard]] virtual SampleRate sampleRate() const noexcept = 0;

    /// Frames per callback actually in use. A backend may deliver a shorter
    /// final block only when stopping; callers should size from the view.
    [[nodiscard]] virtual int bufferFrames() const noexcept = 0;

    [[nodiscard]] virtual int inputChannels() const noexcept = 0;

    [[nodiscard]] virtual int outputChannels() const noexcept = 0;

protected:
    AudioDevice() = default;
};

} // namespace sa::device
