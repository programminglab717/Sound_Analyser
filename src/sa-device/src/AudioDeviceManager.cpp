#include <sa/device/AlsaAudioDevice.h>
#include <sa/device/AudioDeviceManager.h>
#include <sa/device/NullAudioDevice.h>
#include <sa/device/WasapiAudioDevice.h>

#include <string>
#include <utility>

namespace sa::device {

namespace {

int clampChannels(int requested, int available) noexcept {
    if (requested <= 0 || available <= 0) {
        return 0;
    }
    return requested < available ? requested : available;
}

int clampBufferFrames(int requested, int deviceDefault) noexcept {
    int frames = requested;
    if (frames <= 0) {
        frames = deviceDefault > 0 ? deviceDefault : kDefaultBufferFrames;
    }
    if (frames < kMinBufferFrames) {
        frames = kMinBufferFrames;
    }
    if (frames > kMaxBufferFrames) {
        frames = kMaxBufferFrames;
    }
    return frames;
}

/// The backends this build can talk to, in preference order.
///
/// Both platform headers declare nothing at all when their backend is not in
/// the build -- WASAPI off Windows, ALSA where libasound's headers were missing
/// at configure time -- so they are included unconditionally and it is only the
/// registration that is guarded. That keeps the conditional compilation to
/// three lines here instead of spreading it through every caller.
std::vector<std::unique_ptr<AudioDeviceBackend>> platformBackends() {
    std::vector<std::unique_ptr<AudioDeviceBackend>> backends;

#if defined(_WIN32)
    backends.push_back(std::make_unique<WasapiAudioBackend>());
#endif

#if defined(SA_DEVICE_HAVE_ALSA)
    backends.push_back(std::make_unique<AlsaAudioBackend>());
#endif

    // Last, always. It is what makes "there is always a device to open" true on
    // a build with no platform backend at all, on a machine whose audio service
    // is not running, and in a container with no sound card -- which is where
    // most of this layer's tests run.
    backends.push_back(std::make_unique<NullAudioBackend>());
    return backends;
}

} // namespace

AudioDeviceConfig negotiateConfig(const AudioDeviceDescription& description,
                                  const AudioDeviceConfig& requested) noexcept {
    AudioDeviceConfig result = requested;
    result.inputChannels = clampChannels(requested.inputChannels, description.maxInputChannels);
    result.outputChannels = clampChannels(requested.outputChannels, description.maxOutputChannels);
    result.bufferFrames =
        clampBufferFrames(requested.bufferFrames, description.defaultBufferFrames);

    if (!description.supportsSampleRate(result.sampleRate)) {
        result.sampleRate = description.closestSampleRate(result.sampleRate);
    }
    if (!result.sampleRate.isValid()) {
        // Nothing usable was asked for and the device listed nothing. A working
        // stream at a standard rate serves the caller better than a refusal it
        // has no way to act on.
        result.sampleRate = kSampleRate48000;
    }

    return result;
}

AudioDeviceManager::AudioDeviceManager() : AudioDeviceManager(platformBackends()) {}

AudioDeviceManager::AudioDeviceManager(std::vector<std::unique_ptr<AudioDeviceBackend>> backends)
    : backends_(std::move(backends)) {
    // Enumeration failure is not a construction failure: a manager with no
    // devices is a valid state that devices() and openDefault() already report.
    refresh();
}

Status AudioDeviceManager::refresh() {
    devices_.clear();
    deviceBackend_.clear();

    for (std::size_t index = 0; index < backends_.size(); ++index) {
        if (backends_[index] == nullptr) {
            continue;
        }
        for (AudioDeviceDescription& description : backends_[index]->enumerate()) {
            devices_.push_back(std::move(description));
            deviceBackend_.push_back(index);
        }
    }

    return Status{};
}

const AudioDeviceDescription* AudioDeviceManager::find(std::string_view id) const noexcept {
    for (const AudioDeviceDescription& description : devices_) {
        if (description.id == id) {
            return &description;
        }
    }
    return nullptr;
}

const AudioDeviceDescription* AudioDeviceManager::defaultDevice() const noexcept {
    const std::size_t index = defaultIndex();
    return index < devices_.size() ? &devices_[index] : nullptr;
}

std::size_t AudioDeviceManager::defaultIndex() const noexcept {
    // The system's own choice first; then anything that can make a sound, since
    // playback is what an audio editor is usually opening a device for; then
    // whatever is left, which is how a capture-only rig still gets a device.
    for (std::size_t i = 0; i < devices_.size(); ++i) {
        if (devices_[i].isDefault) {
            return i;
        }
    }
    for (std::size_t i = 0; i < devices_.size(); ++i) {
        if (devices_[i].hasOutput()) {
            return i;
        }
    }
    return devices_.empty() ? devices_.size() : 0;
}

Result<std::unique_ptr<AudioDevice>> AudioDeviceManager::open(std::string_view id,
                                                              const AudioDeviceConfig& config) {
    for (std::size_t i = 0; i < devices_.size(); ++i) {
        if (devices_[i].id == id) {
            return openAt(i, config);
        }
    }
    return Error{ErrorCode::NotFound, "no audio device with id " + std::string{id}};
}

Result<std::unique_ptr<AudioDevice>>
AudioDeviceManager::openDefault(const AudioDeviceConfig& config) {
    const std::size_t index = defaultIndex();
    if (index >= devices_.size()) {
        return Error{ErrorCode::NotFound, "no audio devices are available"};
    }
    return openAt(index, config);
}

Result<std::unique_ptr<AudioDevice>>
AudioDeviceManager::openOrFallback(std::string_view id, const AudioDeviceConfig& config) {
    if (!id.empty()) {
        Result<std::unique_ptr<AudioDevice>> opened = open(id, config);
        if (opened.hasValue()) {
            return opened;
        }
    }

    // The remembered interface has been unplugged, or refused to open. Coming
    // up on something else is what a user expects; refusing to start is not.
    const std::size_t preferred = defaultIndex();
    if (preferred < devices_.size() && devices_[preferred].id != id) {
        Result<std::unique_ptr<AudioDevice>> opened = openAt(preferred, config);
        if (opened.hasValue()) {
            return opened;
        }
    }

    // A device that enumerates is not necessarily a device that opens: it may
    // be held in exclusive mode, or its driver may have gone away since the
    // last refresh. Keep going -- the null backend is last in the list, so this
    // loop always has something that works to end on.
    for (std::size_t i = 0; i < devices_.size(); ++i) {
        if (i == preferred || devices_[i].id == id) {
            continue;
        }
        Result<std::unique_ptr<AudioDevice>> opened = openAt(i, config);
        if (opened.hasValue()) {
            return opened;
        }
    }

    return Error{ErrorCode::NotFound, "no audio device could be opened"};
}

Result<std::unique_ptr<AudioDevice>> AudioDeviceManager::openAt(std::size_t index,
                                                                const AudioDeviceConfig& config) {
    const AudioDeviceDescription& description = devices_[index];
    const AudioDeviceConfig effective = negotiateConfig(description, config);
    if (!effective.isValid()) {
        return Error{ErrorCode::InvalidArgument,
                     "device " + description.id +
                         " offers no channels in the requested directions"};
    }

    return backends_[deviceBackend_[index]]->open(description, effective);
}

} // namespace sa::device
