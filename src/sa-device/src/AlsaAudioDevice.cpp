#include <sa/device/AlsaAudioDevice.h>

#if defined(SA_DEVICE_HAVE_ALSA)

#include <algorithm>
#include <alsa/asoundlib.h>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <utility>
#include <vector>

namespace sa::device {

namespace {

/// Rates worth probing during enumeration. ALSA will happily report a
/// continuous range through its rate plugin, so a list of the rates anyone
/// actually records at is more use to a device-picker than min and max.
constexpr unsigned int kProbedRates[] = {44100u, 48000u, 88200u, 96000u, 176400u, 192000u};

/// Periods per buffer. Two is the theoretical minimum and leaves no slack at
/// all for a scheduling hiccup; four is the usual compromise, and at the block
/// sizes this layer negotiates it is still only a few milliseconds of latency.
constexpr unsigned int kPeriodsPerBuffer = 4;

/// SCHED_FIFO priority for the render thread.
///
/// Not the maximum: priority 99 outranks the kernel's own watchdog and RCU
/// threads, and an audio thread that spins there can wedge the machine rather
/// than glitch. This sits above every ordinary desktop task and below the
/// kernel's, which is where PipeWire and JACK put theirs for the same reason.
constexpr int kRenderThreadPriority = 70;

/// Owns a string ALSA allocated with malloc. snd_device_name_get_hint returns
/// one per field per device, and the enumeration loop below has half a dozen
/// early exits that would otherwise each need their own free().
class HintString {
public:
    HintString() = default;

    explicit HintString(char* text) noexcept : text_(text) {}

    ~HintString() { std::free(text_); }

    HintString(const HintString&) = delete;
    HintString& operator=(const HintString&) = delete;

    HintString(HintString&& other) noexcept : text_(other.text_) { other.text_ = nullptr; }

    HintString& operator=(HintString&& other) noexcept {
        if (this != &other) {
            std::free(text_);
            text_ = other.text_;
            other.text_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] const char* get() const noexcept { return text_; }

    [[nodiscard]] bool empty() const noexcept { return text_ == nullptr || text_[0] == '\0'; }

    [[nodiscard]] std::string toString() const {
        return text_ != nullptr ? std::string{text_} : std::string{};
    }

private:
    char* text_ = nullptr;
};

/// Owns the hint array itself, which has its own free function.
class HintList {
public:
    ~HintList() {
        if (hints_ != nullptr) {
            snd_device_name_free_hint(hints_);
        }
    }

    HintList() = default;
    HintList(const HintList&) = delete;
    HintList& operator=(const HintList&) = delete;

    [[nodiscard]] void*** put() noexcept { return &hints_; }

    [[nodiscard]] void** get() const noexcept { return hints_; }

private:
    void** hints_ = nullptr;
};

/// The formats we are willing to play, best first. Float32 first because it is
/// what the rest of the codebase already holds, so it costs a copy and no
/// conversion at all.
struct FormatChoice {
    snd_pcm_format_t alsa;
    InterleavedFormat ours;
};

constexpr FormatChoice kFormatChoices[] = {
    {SND_PCM_FORMAT_FLOAT_LE, InterleavedFormat::Float32},
    {SND_PCM_FORMAT_S32_LE, InterleavedFormat::Int32},
    {SND_PCM_FORMAT_S16_LE, InterleavedFormat::Int16},
};

/// ALSA's description field is a two-line affair -- card name, then a longer
/// blurb -- and only the first line belongs in a device list.
[[nodiscard]] std::string firstLine(const std::string& text) {
    const std::size_t newline = text.find('\n');
    return newline == std::string::npos ? text : text.substr(0, newline);
}

/// Everything after the "alsa:" prefix is the PCM name, passed to
/// snd_pcm_open unchanged.
[[nodiscard]] std::string pcmNameOf(const std::string& id) {
    return id.substr(AlsaAudioBackend::kIdPrefix.size());
}

/// True when the named PCM can be opened right now.
///
/// AudioDeviceManager::openOrFallback walks the device list opening candidates
/// until one works, and that loop is only worth anything if open() actually
/// touches the device: a PCM that enumerated a moment ago may since have been
/// unplugged or taken exclusively by something else. Without this the first
/// well-formed ALSA id would always "open", the loop would stop there, and the
/// failure would surface from start() with the fallback chain already spent.
///
/// Non-blocking, so a PCM another application is holding answers straight away
/// instead of parking the caller on it.
[[nodiscard]] bool pcmCanOpen(const char* pcmName) {
    snd_pcm_t* pcm = nullptr;
    if (snd_pcm_open(&pcm, pcmName, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0) {
        return false;
    }
    (void)snd_pcm_close(pcm);
    return true;
}

/// Reads what a PCM will accept, for enumeration. Opened non-blocking so that a
/// device another application is holding fails fast instead of parking the
/// enumeration thread on it.
void probeDevice(const char* pcmName, AudioDeviceDescription& description) {
    snd_pcm_t* pcm = nullptr;
    if (snd_pcm_open(&pcm, pcmName, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0) {
        // Busy or asleep, not necessarily unusable. Leave the conservative
        // defaults in place and let open() find out for certain later --
        // dropping the device here would hide an interface that works.
        return;
    }

    snd_pcm_hw_params_t* params = nullptr;
    if (snd_pcm_hw_params_malloc(&params) == 0) {
        if (snd_pcm_hw_params_any(pcm, params) >= 0) {
            unsigned int maxChannels = 0;
            if (snd_pcm_hw_params_get_channels_max(params, &maxChannels) == 0) {
                // A software plugin will claim thousands. Clamp to what a
                // buffer in this codebase can hold rather than believe it.
                description.maxOutputChannels =
                    static_cast<int>(std::min<unsigned int>(maxChannels, kMaxChannels));
            }
            for (const unsigned int rate : kProbedRates) {
                if (snd_pcm_hw_params_test_rate(pcm, params, rate, 0) == 0) {
                    description.sampleRates.push_back(SampleRate{static_cast<double>(rate)});
                }
            }
        }
        snd_pcm_hw_params_free(params);
    }

    snd_pcm_close(pcm);
}

} // namespace

struct AlsaStream {
    snd_pcm_t* pcm = nullptr;

    /// Interleaved staging for one write. Sized when the stream opens, which is
    /// on the render thread but outside the callback -- the callback itself
    /// only ever writes into memory that already exists.
    std::vector<std::byte> scratch;

    snd_pcm_uframes_t chunkFrames = 0;
    std::size_t frameBytes = 0;
    InterleavedFormat format = InterleavedFormat::Float32;
    int channels = 0;
};

AlsaAudioDevice::AlsaAudioDevice(AudioDeviceDescription description, AudioDeviceConfig config,
                                 std::string pcmName)
    : ThreadedOutputDevice(std::move(description), config), pcmName_(std::move(pcmName)),
      stream_(std::make_unique<AlsaStream>()) {}

AlsaAudioDevice::~AlsaAudioDevice() {
    // Before the base class runs: stop() dispatches virtually, and the render
    // thread is still calling openStream() and friends on this object.
    stop();
}

void AlsaAudioDevice::configureRenderThread() noexcept {
    sched_param parameters{};
    const int lowest = sched_get_priority_min(SCHED_FIFO);
    const int highest = sched_get_priority_max(SCHED_FIFO);
    parameters.sched_priority = std::clamp(kRenderThreadPriority, lowest, highest);

    // Expected to fail on a stock desktop, where an unprivileged process has an
    // RLIMIT_RTPRIO of zero. That is not an error worth reporting: the stream
    // still runs, it is simply at the mercy of the ordinary scheduler, and
    // telling the user about a limit only root can lift helps nobody.
    (void)pthread_setschedparam(pthread_self(), SCHED_FIFO, &parameters);
}

Status AlsaAudioDevice::openStream() {
    AlsaStream& stream = *stream_;

    int error = snd_pcm_open(&stream.pcm, pcmName_.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (error < 0 || stream.pcm == nullptr) {
        stream.pcm = nullptr;
        return Error{ErrorCode::IoFailure,
                     "could not open ALSA device " + pcmName_ + ": " + snd_strerror(error)};
    }

    snd_pcm_hw_params_t* params = nullptr;
    if (snd_pcm_hw_params_malloc(&params) < 0) {
        return Error{ErrorCode::OutOfMemory, "could not allocate ALSA hardware parameters"};
    }

    // One exit point from here on, so the parameter block is freed exactly once
    // however the negotiation goes.
    Status outcome;
    snd_pcm_uframes_t period = static_cast<snd_pcm_uframes_t>(config().bufferFrames);
    unsigned int channels = static_cast<unsigned int>(config().outputChannels);
    unsigned int rate = static_cast<unsigned int>(std::lround(config().sampleRate.hz()));
    InterleavedFormat chosenFormat = InterleavedFormat::Float32;

    if ((error = snd_pcm_hw_params_any(stream.pcm, params)) < 0) {
        outcome = Error{ErrorCode::IoFailure,
                        std::string{"ALSA device offers no usable configuration: "} +
                            snd_strerror(error)};
    } else if ((error = snd_pcm_hw_params_set_access(stream.pcm, params,
                                                     SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        // Every PCM worth using supports this. A device that does not is either
        // mmap-only or broken, and neither is worth a second code path.
        outcome = Error{ErrorCode::UnsupportedFormat,
                        std::string{"ALSA device will not take interleaved writes: "} +
                            snd_strerror(error)};
    } else {
        bool formatSet = false;
        for (const FormatChoice& candidate : kFormatChoices) {
            if (snd_pcm_hw_params_set_format(stream.pcm, params, candidate.alsa) == 0) {
                chosenFormat = candidate.ours;
                formatSet = true;
                break;
            }
        }
        if (!formatSet) {
            outcome = Error{ErrorCode::UnsupportedFormat,
                            "ALSA device accepts neither float32 nor 16- or 32-bit PCM"};
        }
    }

    if (outcome.ok()) {
        // "_near" throughout: a device that cannot give us exactly what was
        // asked for should give us the closest thing it has, not an error. What
        // it settled on is read back below and reported through sampleRate().
        if ((error = snd_pcm_hw_params_set_channels_near(stream.pcm, params, &channels)) < 0 ||
            (error = snd_pcm_hw_params_set_rate_near(stream.pcm, params, &rate, nullptr)) < 0 ||
            (error = snd_pcm_hw_params_set_period_size_near(stream.pcm, params, &period, nullptr)) <
                0) {
            outcome = Error{ErrorCode::IoFailure,
                            std::string{"ALSA would not accept the stream geometry: "} +
                                snd_strerror(error)};
        } else {
            unsigned int periods = kPeriodsPerBuffer;
            // Advisory: some plugins fix the buffer size and refuse this, which
            // is not a reason to give up on an otherwise workable stream.
            (void)snd_pcm_hw_params_set_periods_near(stream.pcm, params, &periods, nullptr);

            if ((error = snd_pcm_hw_params(stream.pcm, params)) < 0) {
                outcome = Error{ErrorCode::IoFailure,
                                std::string{"ALSA rejected the negotiated parameters: "} +
                                    snd_strerror(error)};
            } else {
                (void)snd_pcm_hw_params_get_period_size(params, &period, nullptr);
            }
        }
    }

    snd_pcm_hw_params_free(params);
    if (!outcome.ok()) {
        return outcome;
    }

    snd_pcm_sw_params_t* software = nullptr;
    if (snd_pcm_sw_params_malloc(&software) == 0) {
        if (snd_pcm_sw_params_current(stream.pcm, software) == 0) {
            // Start once a period is queued rather than on the first write, so
            // playback begins with the buffer already ahead of the hardware
            // instead of one underrun behind it.
            (void)snd_pcm_sw_params_set_start_threshold(stream.pcm, software, period);
            (void)snd_pcm_sw_params_set_avail_min(stream.pcm, software, period);
            (void)snd_pcm_sw_params(stream.pcm, software);
        }
        snd_pcm_sw_params_free(software);
    }

    if ((error = snd_pcm_prepare(stream.pcm)) < 0) {
        return Error{ErrorCode::IoFailure,
                     std::string{"ALSA device would not prepare: "} + snd_strerror(error)};
    }

    stream.format = chosenFormat;
    stream.channels = static_cast<int>(channels);
    stream.chunkFrames =
        period > 0 ? period : static_cast<snd_pcm_uframes_t>(config().bufferFrames);
    stream.frameBytes = static_cast<std::size_t>(stream.channels) *
                        static_cast<std::size_t>(bytesPerSample(stream.format));
    stream.scratch.assign(static_cast<std::size_t>(stream.chunkFrames) * stream.frameBytes,
                          std::byte{0});

    setActualSampleRate(SampleRate{static_cast<double>(rate)});
    return Status{};
}

bool AlsaAudioDevice::recover(int error) noexcept {
    AlsaStream& stream = *stream_;
    if (stream.pcm == nullptr) {
        return false;
    }

    // snd_pcm_recover handles the three failures that are part of normal
    // operation -- an underrun, a suspend/resume cycle, an interrupted call --
    // and hands back anything else unchanged. Silent, because this is the
    // render thread and logging here is exactly what the real-time contract
    // forbids; the restart count is how a caller learns something happened.
    return snd_pcm_recover(stream.pcm, error, /*silent=*/1) == 0;
}

void AlsaAudioDevice::runStream() noexcept {
    AlsaStream& stream = *stream_;
    if (stream.pcm == nullptr || stream.scratch.empty()) {
        return;
    }

    const auto chunk = static_cast<SampleCount>(stream.chunkFrames);

    while (keepRunning()) {
        blocker().render(callback(), stream.scratch.data(), chunk, stream.format, stream.channels);

        const std::byte* cursor = stream.scratch.data();
        snd_pcm_uframes_t remaining = stream.chunkFrames;
        while (remaining > 0) {
            // Blocking: this is what paces the loop. The wait is bounded by the
            // buffer, a few tens of milliseconds, which is also the worst-case
            // latency of stop() -- short enough not to need waking.
            const snd_pcm_sframes_t written = snd_pcm_writei(stream.pcm, cursor, remaining);
            if (written < 0) {
                if (!recover(static_cast<int>(written))) {
                    // The device has gone, or failed in a way ALSA has no
                    // recipe for. Hand back to the base class, which rebuilds.
                    return;
                }
                // The timeline has already broken, so the rest of this block is
                // stale. Dropping it costs one block; writing it late would put
                // every later block late too.
                break;
            }

            const auto taken = static_cast<snd_pcm_uframes_t>(written);
            remaining -= taken;
            cursor +=
                static_cast<std::ptrdiff_t>(static_cast<std::size_t>(taken) * stream.frameBytes);
        }
    }
}

void AlsaAudioDevice::closeStream() noexcept {
    AlsaStream& stream = *stream_;
    if (stream.pcm == nullptr) {
        return;
    }

    // drop, not drain: we have been told to stop, and draining would play out
    // whatever is queued first -- up to a bufferful of audio the user has
    // already asked to be rid of.
    (void)snd_pcm_drop(stream.pcm);
    (void)snd_pcm_close(stream.pcm);
    stream.pcm = nullptr;
}

std::vector<AudioDeviceDescription> AlsaAudioBackend::enumerate() const {
    std::vector<AudioDeviceDescription> devices;

    HintList hints;
    if (snd_device_name_hint(-1, "pcm", hints.put()) != 0 || hints.get() == nullptr) {
        return devices;
    }

    for (void** hint = hints.get(); *hint != nullptr; ++hint) {
        const HintString name{snd_device_name_get_hint(*hint, "NAME")};
        if (name.empty()) {
            continue;
        }

        const HintString direction{snd_device_name_get_hint(*hint, "IOID")};
        if (direction.get() != nullptr && std::strcmp(direction.get(), "Output") != 0) {
            continue;
        }

        // ALSA's own "null" PCM swallows everything written to it. Offering it
        // as a playback device would mean a user selecting it, seeing a healthy
        // stream, and hearing nothing with no indication why. This layer
        // already has a device for "no hardware" -- NullAudioDevice -- and it
        // is honest about being one. open() will still take "alsa:null" if
        // something asks for it by name, which is how the tests get a PCM to
        // drive on a machine with no sound card.
        if (std::strcmp(name.get(), "null") == 0) {
            continue;
        }

        AudioDeviceDescription description;
        description.id = std::string{kIdPrefix} + name.toString();

        const HintString label{snd_device_name_get_hint(*hint, "DESC")};
        description.name = label.empty() ? name.toString() : firstLine(label.toString());

        // ALSA has no capture-through-a-playback-handle concept, and this
        // backend is playback-only, so the input side is flatly zero.
        description.maxInputChannels = 0;
        description.maxOutputChannels = 2;
        description.defaultBufferFrames = kDefaultBufferFrames;

        // "default" is what ALSA routes to whatever the user's configuration
        // says, which is exactly the meaning of this flag.
        description.isDefault = std::strcmp(name.get(), "default") == 0;

        probeDevice(name.get(), description);
        devices.push_back(std::move(description));
    }

    return devices;
}

Result<std::unique_ptr<AudioDevice>>
AlsaAudioBackend::open(const AudioDeviceDescription& description, const AudioDeviceConfig& config) {
    if (!description.id.starts_with(kIdPrefix)) {
        return Error{ErrorCode::NotFound, "not an ALSA device id: " + description.id};
    }
    if (!config.isValid()) {
        return Error{ErrorCode::InvalidArgument, "audio device configuration is not usable"};
    }
    if (config.outputChannels <= 0) {
        // Said plainly rather than opened output-only: a caller that asked to
        // record and got a playback stream records nothing and never learns why.
        return Error{ErrorCode::InvalidArgument,
                     "the ALSA backend plays audio; it does not capture"};
    }

    std::string pcmName = pcmNameOf(description.id);
    if (pcmName.empty()) {
        return Error{ErrorCode::NotFound, "ALSA device id names no PCM: " + description.id};
    }
    if (!pcmCanOpen(pcmName.c_str())) {
        return Error{ErrorCode::NotFound, "the ALSA device is not available: " + description.id};
    }

    try {
        return std::unique_ptr<AudioDevice>{
            std::make_unique<AlsaAudioDevice>(description, config, std::move(pcmName))};
    } catch (const std::bad_alloc&) {
        // The buffers this device allocates up front are the only thing here
        // that can fail, and this module reports failure rather than throwing.
        return Error{ErrorCode::OutOfMemory, "could not allocate the ALSA device"};
    }
}

} // namespace sa::device

#endif // SA_DEVICE_HAVE_ALSA
