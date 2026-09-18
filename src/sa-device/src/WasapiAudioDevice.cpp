#include <sa/device/WasapiAudioDevice.h>

#if defined(_WIN32)

// The Windows SDK headers are reached through this one block.
// WIN32_LEAN_AND_MEAN keeps <windows.h> from dragging in the multimedia and RPC
// headers we do not use, and NOMINMAX stops it defining min and max as macros,
// which would break std::min at every later use in this translation unit.
//
// The block below is in dependency order, not alphabetical, and clang-format is
// switched off across it for that reason. <windows.h> has to come first.
// WAVEFORMATEX and WAVEFORMATEXTENSIBLE then have to come from <mmreg.h>,
// because WIN32_LEAN_AND_MEAN is what stops <windows.h> pulling them in through
// <mmsystem.h>, and the endpoint headers below are MIDL output that uses them.
// Sorted alphabetically this list puts <audioclient.h> first and does not
// compile -- on the one platform that ever compiles it.
//
// Nothing here depends on INITGUID, deliberately; see kFriendlyNameKey below.
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif

#if defined(_MSC_VER)
#pragma warning(push)
// The endpoint headers are generated MIDL output and are not written to this
// project's warning set. Scoped to the include block alone, so every line we
// write ourselves is still held to /W4 /WX.
//   C4668 undefined macro tested in #if -- winioctl.h and friends do this.
//   C4820 padding inserted after a member -- unavoidable in an ABI struct.
//   C5105 macro expansion producing 'defined' -- winbase.h, in some SDKs.
#pragma warning(disable : 4668)
#pragma warning(disable : 4820)
#pragma warning(disable : 5105)
#endif

// clang-format off
#include <windows.h>

#include <objbase.h>
#include <mmreg.h>
#include <propsys.h>
#include <propidl.h>

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
// clang-format on

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace sa::device {

namespace {

/// How long the render loop will wait for the endpoint's event before deciding
/// the stream has stopped working. Two seconds is what the WASAPI documentation
/// suggests, and it is long enough that no amount of ordinary scheduling delay
/// reaches it.
constexpr DWORD kRenderWaitTimeoutMs = 2000;

/// The subtype GUIDs a WAVEFORMATEXTENSIBLE can carry that we know how to feed.
///
/// Spelled out rather than taken from <ksmedia.h>: that header exists to
/// describe kernel streaming and pulls in a great deal of it for the sake of
/// two constants. These two are not guesses -- every WAVE_FORMAT_* tag has a
/// subtype GUID of the form {0000tttt-0000-0010-8000-00AA00389B71}, and these
/// are tags 3 (IEEE float) and 1 (PCM).
constexpr GUID kSubtypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr GUID kSubtypePcm = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

/// PKEY_Device_FriendlyName.
///
/// Written out rather than included from <functiondiscoverykeys_devpkey.h>,
/// which only defines its keys when INITGUID is set -- and INITGUID has to be
/// defined before the *first* Windows header, because <propkeydef.h> fixes its
/// definition of DEFINE_PROPERTYKEY the first time anything processes it. That
/// ordering requirement is invisible at the point it breaks and survives
/// exactly until someone sorts the includes. The cost of spelling it out is
/// that a wrong value degrades to a device named after its endpoint id rather
/// than failing loudly, which is why enumerate() has that fallback anyway.
constexpr PROPERTYKEY kFriendlyNameKey = {
    {0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};

/// Owns one COM interface reference.
///
/// Hand-written because this is the only COM in the codebase: one small class
/// that can be read in a minute beats a dependency on wrl/client.h, whose
/// ownership conventions -- which methods release first, which return a
/// non-owning pointer -- would have to be re-learned at every call site.
template <typename Interface>
class ComRef {
public:
    ComRef() = default;

    ~ComRef() { reset(); }

    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;
    ComRef(ComRef&&) = delete;
    ComRef& operator=(ComRef&&) = delete;

    void reset() noexcept {
        if (pointer_ != nullptr) {
            pointer_->Release();
            pointer_ = nullptr;
        }
    }

    /// The address to hand a COM factory. Releases anything already held first,
    /// so reusing one of these cannot leak the previous reference.
    [[nodiscard]] Interface** put() noexcept {
        reset();
        return &pointer_;
    }

    /// The same address as void**, which is the shape Activate() and
    /// GetService() take.
    [[nodiscard]] void** putVoid() noexcept { return reinterpret_cast<void**>(put()); }

    [[nodiscard]] Interface* get() const noexcept { return pointer_; }

    Interface* operator->() const noexcept { return pointer_; }

    explicit operator bool() const noexcept { return pointer_ != nullptr; }

private:
    Interface* pointer_ = nullptr;
};

/// Owns a block the COM allocator returned. GetMixFormat and GetId both hand
/// one back, and both sit in front of several early exits.
template <typename T>
class CoMemory {
public:
    CoMemory() = default;

    ~CoMemory() { CoTaskMemFree(pointer_); }

    CoMemory(const CoMemory&) = delete;
    CoMemory& operator=(const CoMemory&) = delete;
    CoMemory(CoMemory&&) = delete;
    CoMemory& operator=(CoMemory&&) = delete;

    [[nodiscard]] T** put() noexcept { return &pointer_; }

    [[nodiscard]] T* get() const noexcept { return pointer_; }

    explicit operator bool() const noexcept { return pointer_ != nullptr; }

private:
    T* pointer_ = nullptr;
};

/// Initialises COM for the calling thread and leaves it exactly as it was
/// found. RPC_E_CHANGED_MODE means the caller already put this thread in a
/// single-threaded apartment: that is still a usable apartment, and
/// uninitialising it afterwards would be undoing someone else's work.
class ComScope {
public:
    ComScope() noexcept : owned_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}

    ~ComScope() {
        if (owned_) {
            CoUninitialize();
        }
    }

    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    bool owned_ = false;
};

[[nodiscard]] std::string toUtf8(const wchar_t* text) {
    if (text == nullptr || text[0] == L'\0') {
        return std::string{};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) {
        return std::string{};
    }
    // The count includes the terminator, which std::string supplies itself.
    std::string result(static_cast<std::size_t>(bytes - 1), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), bytes, nullptr, nullptr) <= 0) {
        return std::string{};
    }
    return result;
}

[[nodiscard]] std::wstring toWide(const std::string& text) {
    if (text.empty()) {
        return std::wstring{};
    }
    const int length = static_cast<int>(text.size());
    const int characters = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), length, nullptr, 0);
    if (characters <= 0) {
        return std::wstring{};
    }
    std::wstring result(static_cast<std::size_t>(characters), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, text.c_str(), length, result.data(), characters) <= 0) {
        return std::wstring{};
    }
    return result;
}

/// Works out which of our three interleaved formats a WAVEFORMATEX describes.
///
/// A 32-bit PCM format whose valid bits say 24 is deliberately treated as
/// 32-bit: the container really is 32 bits wide, and writing the full word lets
/// the endpoint take whichever top bits it uses. Packed 24-bit is rejected,
/// because in shared mode it does not occur and guessing at it would be a
/// silent channel-scrambling bug rather than an honest refusal.
[[nodiscard]] bool classifyFormat(const WAVEFORMATEX& format, InterleavedFormat& chosen) noexcept {
    WORD tag = format.wFormatTag;

    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        if (format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            return false;
        }
        const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        if (IsEqualGUID(extensible.SubFormat, kSubtypeIeeeFloat)) {
            tag = WAVE_FORMAT_IEEE_FLOAT;
        } else if (IsEqualGUID(extensible.SubFormat, kSubtypePcm)) {
            tag = WAVE_FORMAT_PCM;
        } else {
            return false;
        }
    }

    if (tag == WAVE_FORMAT_IEEE_FLOAT && format.wBitsPerSample == 32) {
        chosen = InterleavedFormat::Float32;
        return true;
    }
    if (tag == WAVE_FORMAT_PCM && format.wBitsPerSample == 16) {
        chosen = InterleavedFormat::Int16;
        return true;
    }
    if (tag == WAVE_FORMAT_PCM && format.wBitsPerSample == 32) {
        chosen = InterleavedFormat::Int32;
        return true;
    }
    return false;
}

/// Copies the endpoint's own mix format and changes only its sample rate.
///
/// Copying rather than building one is the whole point: the mixer's format
/// carries a channel mask and a subtype GUID that describe this endpoint's
/// speaker arrangement, and a hand-built WAVEFORMATEXTENSIBLE that gets either
/// of them wrong plays perfectly happily out of the wrong speakers. Returns
/// false for a format with more trailing data than a WAVEFORMATEXTENSIBLE,
/// which does not occur in shared mode and is not worth guessing at.
[[nodiscard]] bool copyFormatAtRate(const WAVEFORMATEX& mix, DWORD rate,
                                    WAVEFORMATEXTENSIBLE& tuned) noexcept {
    const std::size_t total = sizeof(WAVEFORMATEX) + mix.cbSize;
    if (total > sizeof(WAVEFORMATEXTENSIBLE) || mix.nBlockAlign == 0) {
        return false;
    }

    std::memset(&tuned, 0, sizeof(tuned));
    std::memcpy(&tuned, &mix, total);
    tuned.Format.nSamplesPerSec = rate;
    tuned.Format.nAvgBytesPerSec = rate * static_cast<DWORD>(tuned.Format.nBlockAlign);
    return true;
}

/// Frames as a REFERENCE_TIME, which counts in hundreds of nanoseconds.
[[nodiscard]] REFERENCE_TIME framesToReferenceTime(int frames, DWORD rate) noexcept {
    if (frames <= 0 || rate == 0) {
        return 0;
    }
    return static_cast<REFERENCE_TIME>(static_cast<long long>(frames) * 10000000LL /
                                       static_cast<long long>(rate));
}

/// Notices that Windows has moved the default render endpoint.
///
/// The callback arrives on a thread COM owns and must not be made to wait, so
/// it sets a flag and returns; the render thread does the rebuilding in its own
/// time. Reference counting is by hand because writing it out is five lines and
/// because a COM object that deletes itself is exactly the kind of thing that
/// should be visible rather than hidden inside a helper.
#if defined(__GNUC__)
#pragma GCC diagnostic push
// A COM interface has no virtual destructor by design: lifetime is Release(),
// never delete, so -Wnon-virtual-dtor fires on every implementation of one.
// The class below is final and destroys itself through its own static type, so
// the slicing this warning is about cannot happen. Scoped to this class alone
// rather than weakened for the target. MSVC, which is the compiler that
// actually builds this file, does not warn at /W4 -- the suppression is here so
// that cross-checking the file with a MinGW or clang toolchain stays clean,
// which is the only way it gets compiled at all outside a Windows CI run.
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif

class DefaultEndpointWatcher final : public IMMNotificationClient {
public:
    explicit DefaultEndpointWatcher(std::atomic<bool>& moved) noexcept : moved_(moved) {}

    DefaultEndpointWatcher(const DefaultEndpointWatcher&) = delete;
    DefaultEndpointWatcher& operator=(const DefaultEndpointWatcher&) = delete;

    ULONG STDMETHODCALLTYPE AddRef() override {
        return references_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (IsEqualGUID(id, __uuidof(IUnknown)) ||
            IsEqualGUID(id, __uuidof(IMMNotificationClient))) {
            *object = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        // eConsole only. Windows keeps three separate defaults, and following
        // the communications one would move music playback whenever a call
        // started -- which is precisely what that separation exists to prevent.
        if (flow == eRender && role == eConsole) {
            moved_.store(true, std::memory_order_release);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
        return S_OK;
    }

private:
    // Private so that the only way out is Release(), which is the contract
    // every COM caller already follows.
    ~DefaultEndpointWatcher() = default;

    std::atomic<ULONG> references_{1};
    std::atomic<bool>& moved_;
};

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/// The endpoint's friendly name, or an empty string if it will not say.
[[nodiscard]] std::string friendlyNameOf(IMMDevice& endpoint) {
    ComRef<IPropertyStore> properties;
    if (FAILED(endpoint.OpenPropertyStore(STGM_READ, properties.put())) || !properties) {
        return std::string{};
    }

    PROPVARIANT value;
    PropVariantInit(&value);
    std::string name;
    if (SUCCEEDED(properties->GetValue(kFriendlyNameKey, &value)) && value.vt == VT_LPWSTR) {
        name = toUtf8(value.pwszVal);
    }
    PropVariantClear(&value);
    return name;
}

/// True when the named endpoint is there right now.
///
/// AudioDeviceManager::openOrFallback walks the device list opening candidates
/// until one works, and the entire value of that loop is that a device which
/// enumerated a moment ago may be gone now -- unplugged, or its driver
/// replaced. Without this check every well-formed WASAPI id would "open"
/// successfully, the loop would stop at the first one, and the failure would
/// only surface from start() with the fallback chain already spent. That is
/// exactly the case a headless build agent is in: it has no render endpoint at
/// all, and it should come up on the null device rather than on a WASAPI
/// stream that can never start.
///
/// It proves the endpoint exists, not that it is free. Proving the latter would
/// mean initialising a client and throwing it away, and shared mode does not
/// refuse for contention in the first place.
[[nodiscard]] bool endpointExists(bool followsDefault, const std::wstring& endpointId) {
    const ComScope com;

    ComRef<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), enumerator.putVoid())) ||
        !enumerator) {
        return false;
    }

    ComRef<IMMDevice> endpoint;
    const HRESULT found =
        followsDefault ? enumerator->GetDefaultAudioEndpoint(eRender, eConsole, endpoint.put())
                       : enumerator->GetDevice(endpointId.c_str(), endpoint.put());
    return SUCCEEDED(found) && endpoint;
}

/// Fills in the channel count and rate an endpoint's mixer is running at.
/// Silent on failure: an endpoint that will not describe itself is still worth
/// listing, because opening it may well work.
void describeMixFormat(IMMDevice& endpoint, AudioDeviceDescription& description) {
    ComRef<IAudioClient> client;
    if (FAILED(endpoint.Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.putVoid())) ||
        !client) {
        return;
    }

    CoMemory<WAVEFORMATEX> mix;
    if (FAILED(client->GetMixFormat(mix.put())) || !mix) {
        return;
    }

    description.maxOutputChannels =
        static_cast<int>(std::min<UINT32>(mix.get()->nChannels, static_cast<UINT32>(kMaxChannels)));

    // The mixer's own rate first, because that is the one that costs nothing.
    // The others are reachable through the stream resampler, which is why they
    // are listed at all -- IsFormatSupported cannot confirm them, since in
    // shared mode it only ever agrees with the mix format.
    description.sampleRates.clear();
    description.sampleRates.push_back(SampleRate{static_cast<double>(mix.get()->nSamplesPerSec)});
    for (const double rate : {44100.0, 48000.0, 88200.0, 96000.0, 192000.0}) {
        if (static_cast<DWORD>(rate) != mix.get()->nSamplesPerSec) {
            description.sampleRates.push_back(SampleRate{rate});
        }
    }

    REFERENCE_TIME defaultPeriod = 0;
    REFERENCE_TIME minimumPeriod = 0;
    if (SUCCEEDED(client->GetDevicePeriod(&defaultPeriod, &minimumPeriod)) && defaultPeriod > 0) {
        // Honouring the endpoint's own period keeps WASAPI from re-blocking
        // behind our back, which is latency we would neither see nor control.
        const double frames = static_cast<double>(defaultPeriod) *
                              static_cast<double>(mix.get()->nSamplesPerSec) / 10000000.0;
        const auto rounded = static_cast<int>(std::lround(frames));
        description.defaultBufferFrames = std::clamp(rounded, kMinBufferFrames, kMaxBufferFrames);
    }
}

} // namespace

/// Every Windows handle and interface the stream holds. Defined here rather
/// than in the header so that the header stays free of <windows.h>.
struct WasapiStream {
    ComRef<IMMDeviceEnumerator> enumerator;
    ComRef<IAudioClient> client;
    ComRef<IAudioRenderClient> render;

    /// Refcounted by COM, so a raw pointer with an explicit Release is the
    /// honest representation; ComRef would only obscure the register and
    /// unregister pairing it has to sit inside. Held as the interface rather
    /// than the concrete class so that this struct, which the header names,
    /// has no member whose type is local to this file.
    IMMNotificationClient* watcher = nullptr;

    /// Signalled by the endpoint when it wants more audio.
    HANDLE bufferEvent = nullptr;

    /// Signalled by stop(), so shutting down does not wait out the render
    /// timeout. Manual-reset: once stopping, it must stay stopped.
    HANDLE stopEvent = nullptr;

    /// The MMCSS registration for the render thread, reverted on the same
    /// thread that took it out.
    HANDLE priorityTask = nullptr;

    UINT32 endpointFrames = 0;
    int endpointChannels = 0;
    double sampleRateHz = 0.0;
    InterleavedFormat format = InterleavedFormat::Float32;

    /// Set from a COM notification thread, read by the render thread.
    std::atomic<bool> defaultEndpointMoved{false};

    bool comInitialised = false;
};

namespace {

/// Activates a client on `endpoint` and brings a shared-mode, event-driven
/// stream up on it. Split out because Initialize() may be called only once per
/// client, so the fallback to the mixer's own format needs a fresh one.
[[nodiscard]] Status startClient(IMMDevice& endpoint, const WAVEFORMATEX& format, DWORD extraFlags,
                                 REFERENCE_TIME bufferDuration, WasapiStream& stream) {
    stream.render.reset();
    stream.client.reset();
    if (stream.bufferEvent != nullptr) {
        CloseHandle(stream.bufferEvent);
        stream.bufferEvent = nullptr;
    }

    InterleavedFormat sampleFormat = InterleavedFormat::Float32;
    if (!classifyFormat(format, sampleFormat)) {
        return Error{ErrorCode::UnsupportedFormat,
                     "the WASAPI mixer is running a format that is neither float32 nor 16- or "
                     "32-bit PCM"};
    }

    if (FAILED(endpoint.Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                 stream.client.putVoid())) ||
        !stream.client) {
        return Error{ErrorCode::IoFailure, "could not activate the WASAPI endpoint"};
    }

    // Periodicity is zero on purpose: in shared mode the endpoint owns the
    // period, and passing anything else here is rejected outright.
    if (FAILED(stream.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                         AUDCLNT_STREAMFLAGS_EVENTCALLBACK | extraFlags,
                                         bufferDuration, 0, &format, nullptr))) {
        return Error{ErrorCode::IoFailure, "WASAPI would not open a shared-mode stream"};
    }

    stream.bufferEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (stream.bufferEvent == nullptr) {
        return Error{ErrorCode::IoFailure, "could not create the WASAPI render event"};
    }
    if (FAILED(stream.client->SetEventHandle(stream.bufferEvent))) {
        return Error{ErrorCode::IoFailure, "WASAPI would not take the render event"};
    }

    if (FAILED(stream.client->GetBufferSize(&stream.endpointFrames)) ||
        stream.endpointFrames == 0) {
        return Error{ErrorCode::IoFailure, "WASAPI reported no endpoint buffer"};
    }

    if (FAILED(stream.client->GetService(__uuidof(IAudioRenderClient), stream.render.putVoid())) ||
        !stream.render) {
        return Error{ErrorCode::IoFailure, "could not obtain the WASAPI render client"};
    }

    stream.format = sampleFormat;
    stream.endpointChannels = static_cast<int>(format.nChannels);
    stream.sampleRateHz = static_cast<double>(format.nSamplesPerSec);

    // A bufferful of silence before the first Start(), so what comes out at the
    // top of the stream is nothing rather than whatever the shared buffer
    // happened to contain.
    BYTE* preroll = nullptr;
    if (SUCCEEDED(stream.render->GetBuffer(stream.endpointFrames, &preroll))) {
        (void)stream.render->ReleaseBuffer(stream.endpointFrames, AUDCLNT_BUFFERFLAGS_SILENT);
    }

    return Status{};
}

} // namespace

WasapiAudioDevice::WasapiAudioDevice(AudioDeviceDescription description, AudioDeviceConfig config,
                                     std::wstring endpointId, bool followsDefault)
    : ThreadedOutputDevice(std::move(description), config), endpointId_(std::move(endpointId)),
      followsDefault_(followsDefault), stream_(std::make_unique<WasapiStream>()) {
    // Created here, on the calling thread, because wakeStream() has to be able
    // to signal it whether or not a stream was ever successfully opened.
    stream_->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

WasapiAudioDevice::~WasapiAudioDevice() {
    // Before the base class runs: stop() dispatches virtually, and the render
    // thread is still calling openStream() and friends on this object.
    stop();

    if (stream_->stopEvent != nullptr) {
        CloseHandle(stream_->stopEvent);
        stream_->stopEvent = nullptr;
    }
}

void WasapiAudioDevice::configureRenderThread() noexcept {
    WasapiStream& stream = *stream_;

    // MMCSS is how Windows is told this thread is audio: it grants a
    // guaranteed share of the CPU and lifts the thread above the ordinary
    // scheduler, which SetThreadPriority alone does not do. The task name has
    // to be one the system knows; "Pro Audio" is the one with the shortest
    // guaranteed period.
    DWORD taskIndex = 0;
    stream.priorityTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (stream.priorityTask != nullptr) {
        (void)AvSetMmThreadPriority(stream.priorityTask, AVRT_PRIORITY_CRITICAL);
        return;
    }

    // MMCSS can be disabled by policy. Falling back to a plain time-critical
    // thread is worse -- it has no CPU guarantee -- but it is a great deal
    // better than running the audio thread at normal priority.
    (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

void WasapiAudioDevice::releaseRenderThread() noexcept {
    WasapiStream& stream = *stream_;

    if (stream.priorityTask != nullptr) {
        // Has to happen on the thread that registered, which is why the base
        // class offers this hook rather than letting the destructor do it.
        (void)AvRevertMmThreadCharacteristics(stream.priorityTask);
        stream.priorityTask = nullptr;
    }

    if (stream.comInitialised) {
        CoUninitialize();
        stream.comInitialised = false;
    }
}

Status WasapiAudioDevice::openStream() {
    WasapiStream& stream = *stream_;

    if (!stream.comInitialised) {
        // The render thread owns every COM object it uses. Creating them on the
        // caller's thread instead would mean marshalling across apartments the
        // moment a caller happened to be in a single-threaded one -- a fault
        // that surfaces as a hang inside somebody else's message loop.
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
            return Error{ErrorCode::IoFailure, "could not initialise COM on the render thread"};
        }
        stream.comInitialised = true;
    }

    if (stream.stopEvent != nullptr && keepRunning()) {
        ResetEvent(stream.stopEvent);
    }

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), stream.enumerator.putVoid())) ||
        !stream.enumerator) {
        return Error{ErrorCode::IoFailure, "could not create the WASAPI device enumerator"};
    }

    ComRef<IMMDevice> endpoint;
    const HRESULT found =
        followsDefault_
            ? stream.enumerator->GetDefaultAudioEndpoint(eRender, eConsole, endpoint.put())
            : stream.enumerator->GetDevice(endpointId_.c_str(), endpoint.put());
    if (FAILED(found) || !endpoint) {
        return Error{ErrorCode::NotFound, "the requested WASAPI endpoint is not available"};
    }

    ComRef<IAudioClient> probe;
    if (FAILED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, probe.putVoid())) ||
        !probe) {
        return Error{ErrorCode::IoFailure, "could not activate the WASAPI endpoint"};
    }

    CoMemory<WAVEFORMATEX> mix;
    if (FAILED(probe->GetMixFormat(mix.put())) || !mix) {
        return Error{ErrorCode::IoFailure, "WASAPI would not report the endpoint's mix format"};
    }

    REFERENCE_TIME defaultPeriod = 0;
    REFERENCE_TIME minimumPeriod = 0;
    (void)probe->GetDevicePeriod(&defaultPeriod, &minimumPeriod);
    probe.reset();

    const auto wantedRate = static_cast<DWORD>(std::lround(config().sampleRate.hz()));
    REFERENCE_TIME duration =
        framesToReferenceTime(config().bufferFrames, mix.get()->nSamplesPerSec);
    if (duration < defaultPeriod) {
        // Asking for less than the endpoint's own period gets us the period
        // anyway; asking for it explicitly keeps the arithmetic honest.
        duration = defaultPeriod;
    }

    WAVEFORMATEXTENSIBLE tuned{};
    const bool retune = wantedRate != 0 && wantedRate != mix.get()->nSamplesPerSec &&
                        copyFormatAtRate(*mix.get(), wantedRate, tuned);

    // The stream resampler is only asked for when the rate really differs.
    // Turning it on unconditionally would put a resampler in the path of the
    // overwhelmingly common case where it has nothing to do.
    Status opened = Status{};
    if (retune) {
        opened = startClient(*endpoint.get(), tuned.Format,
                             AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                 AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                             duration, stream);
    }
    if (!retune || !opened.ok()) {
        // Either no conversion was needed, or the endpoint would not do it --
        // an older driver, or a policy that forbids it. The mixer's own format
        // always works, and sampleRate() reports what we actually got.
        opened = startClient(*endpoint.get(), *mix.get(), 0, duration, stream);
    }
    if (!opened.ok()) {
        return opened;
    }

    setActualSampleRate(SampleRate{stream.sampleRateHz});

    if (followsDefault_ && stream.watcher == nullptr) {
        // Only the "default output" device follows Windows around. A caller
        // that named an endpoint asked for that endpoint.
        stream.watcher = new (std::nothrow) DefaultEndpointWatcher{stream.defaultEndpointMoved};
        if (stream.watcher != nullptr &&
            FAILED(stream.enumerator->RegisterEndpointNotificationCallback(stream.watcher))) {
            stream.watcher->Release();
            stream.watcher = nullptr;
        }
    }
    stream.defaultEndpointMoved.store(false, std::memory_order_release);

    return Status{};
}

bool WasapiAudioDevice::renderAvailable() noexcept {
    WasapiStream& stream = *stream_;

    UINT32 padding = 0;
    if (FAILED(stream.client->GetCurrentPadding(&padding)) || padding > stream.endpointFrames) {
        // AUDCLNT_E_DEVICE_INVALIDATED arrives here as often as anywhere else:
        // the endpoint has been unplugged, its driver replaced, or the audio
        // service restarted underneath us. Every one of those is a rebuild.
        return false;
    }

    const UINT32 available = stream.endpointFrames - padding;
    if (available == 0) {
        return true;
    }

    BYTE* data = nullptr;
    if (FAILED(stream.render->GetBuffer(available, &data)) || data == nullptr) {
        return false;
    }

    blocker().render(callback(), data, static_cast<SampleCount>(available), stream.format,
                     stream.endpointChannels);

    return SUCCEEDED(stream.render->ReleaseBuffer(available, 0));
}

void WasapiAudioDevice::runStream() noexcept {
    WasapiStream& stream = *stream_;
    if (!stream.client || !stream.render || stream.bufferEvent == nullptr) {
        return;
    }

    if (FAILED(stream.client->Start())) {
        return;
    }

    const HANDLE waits[2] = {stream.bufferEvent, stream.stopEvent};
    const DWORD waitCount = stream.stopEvent != nullptr ? 2u : 1u;

    while (keepRunning()) {
        if (stream.defaultEndpointMoved.load(std::memory_order_acquire)) {
            // Windows has moved the default output. Returning rebuilds the
            // stream on whatever it moved to, which is what a user who just
            // plugged in headphones expects to happen.
            break;
        }

        const DWORD waited = WaitForMultipleObjects(waitCount, waits, FALSE, kRenderWaitTimeoutMs);
        if (waited != WAIT_OBJECT_0) {
            // The stop event, a timeout or an outright failure. A timeout means
            // the endpoint has stopped asking for audio, which is a dead stream
            // whatever the reason, so all three end this run.
            break;
        }

        if (!renderAvailable()) {
            break;
        }
    }

    (void)stream.client->Stop();
}

void WasapiAudioDevice::closeStream() noexcept {
    WasapiStream& stream = *stream_;

    if (stream.watcher != nullptr) {
        if (stream.enumerator) {
            (void)stream.enumerator->UnregisterEndpointNotificationCallback(stream.watcher);
        }
        stream.watcher->Release();
        stream.watcher = nullptr;
    }

    stream.render.reset();
    stream.client.reset();
    stream.enumerator.reset();

    if (stream.bufferEvent != nullptr) {
        CloseHandle(stream.bufferEvent);
        stream.bufferEvent = nullptr;
    }

    stream.endpointFrames = 0;
    stream.endpointChannels = 0;
}

void WasapiAudioDevice::wakeStream() noexcept {
    if (stream_->stopEvent != nullptr) {
        SetEvent(stream_->stopEvent);
    }
}

std::vector<AudioDeviceDescription> WasapiAudioBackend::enumerate() const {
    std::vector<AudioDeviceDescription> devices;

    // enumerate() may be called from any thread and we do not know the caller's
    // apartment, so COM is initialised for the call and left as it was found.
    const ComScope com;

    ComRef<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), enumerator.putVoid())) ||
        !enumerator) {
        return devices;
    }

    // The synthetic default comes first and is the one flagged, so that a
    // caller with no stored preference gets "follows Windows" rather than
    // whichever endpoint happens to be default at this instant.
    AudioDeviceDescription systemDefault;
    systemDefault.id = std::string{kDefaultDeviceId};
    systemDefault.name = "Default output (follows Windows)";
    systemDefault.maxOutputChannels = 2;
    systemDefault.defaultBufferFrames = kDefaultBufferFrames;
    systemDefault.isDefault = true;

    std::string defaultEndpointId;
    ComRef<IMMDevice> defaultEndpoint;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, defaultEndpoint.put())) &&
        defaultEndpoint) {
        CoMemory<WCHAR> rawId;
        if (SUCCEEDED(defaultEndpoint->GetId(rawId.put())) && rawId) {
            defaultEndpointId = toUtf8(rawId.get());
        }
        describeMixFormat(*defaultEndpoint.get(), systemDefault);

        const std::string label = friendlyNameOf(*defaultEndpoint.get());
        if (!label.empty()) {
            systemDefault.name = "Default output (" + label + ")";
        }
    }
    devices.push_back(std::move(systemDefault));

    ComRef<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.put())) ||
        !collection) {
        return devices;
    }

    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) {
        return devices;
    }

    for (UINT index = 0; index < count; ++index) {
        ComRef<IMMDevice> endpoint;
        if (FAILED(collection->Item(index, endpoint.put())) || !endpoint) {
            continue;
        }

        CoMemory<WCHAR> rawId;
        if (FAILED(endpoint->GetId(rawId.put())) || !rawId) {
            continue;
        }

        AudioDeviceDescription description;
        const std::string endpointId = toUtf8(rawId.get());
        if (endpointId.empty()) {
            continue;
        }
        description.id = std::string{kIdPrefix} + endpointId;

        const std::string label = friendlyNameOf(*endpoint.get());
        // An endpoint that will not give up its name is still usable, and a row
        // showing its id beats dropping a device the user can see in Windows.
        description.name = label.empty() ? endpointId : label;

        // Playback only, so the capture side is flatly zero rather than
        // hopeful. A caller asking to record gets a clear refusal from the
        // manager rather than a playback stream that records silence.
        description.maxInputChannels = 0;
        description.maxOutputChannels = 2;
        description.defaultBufferFrames = kDefaultBufferFrames;
        description.isDefault = false;

        describeMixFormat(*endpoint.get(), description);
        devices.push_back(std::move(description));
    }

    return devices;
}

Result<std::unique_ptr<AudioDevice>>
WasapiAudioBackend::open(const AudioDeviceDescription& description,
                         const AudioDeviceConfig& config) {
    if (!description.id.starts_with(kIdPrefix)) {
        return Error{ErrorCode::NotFound, "not a WASAPI device id: " + description.id};
    }
    if (!config.isValid()) {
        return Error{ErrorCode::InvalidArgument, "audio device configuration is not usable"};
    }
    if (config.outputChannels <= 0) {
        return Error{ErrorCode::InvalidArgument,
                     "the WASAPI backend plays audio; it does not capture"};
    }

    const bool followsDefault = description.id == kDefaultDeviceId;
    std::wstring endpointId;
    if (!followsDefault) {
        endpointId = toWide(description.id.substr(kIdPrefix.size()));
        if (endpointId.empty()) {
            return Error{ErrorCode::NotFound,
                         "WASAPI device id names no endpoint: " + description.id};
        }
    }

    if (!endpointExists(followsDefault, endpointId)) {
        return Error{ErrorCode::NotFound,
                     "the WASAPI endpoint is no longer available: " + description.id};
    }

    try {
        return std::unique_ptr<AudioDevice>{std::make_unique<WasapiAudioDevice>(
            description, config, std::move(endpointId), followsDefault)};
    } catch (const std::bad_alloc&) {
        // The buffers this device allocates up front are the only thing here
        // that can fail, and this module reports failure rather than throwing.
        return Error{ErrorCode::OutOfMemory, "could not allocate the WASAPI device"};
    }
}

} // namespace sa::device

#endif // _WIN32
