#include <sa/io/AudioFileInfo.h>

namespace sa::io {

std::string_view toString(SampleFormat format) noexcept {
    switch (format) {
    case SampleFormat::Unknown:
        return "unknown";
    case SampleFormat::PcmUInt8:
        return "8-bit PCM";
    case SampleFormat::PcmInt16:
        return "16-bit PCM";
    case SampleFormat::PcmInt24:
        return "24-bit PCM";
    case SampleFormat::PcmInt32:
        return "32-bit PCM";
    case SampleFormat::Float32:
        return "32-bit float";
    case SampleFormat::Float64:
        return "64-bit float";
    }
    return "unknown";
}

int bytesPerSample(SampleFormat format) noexcept {
    switch (format) {
    case SampleFormat::Unknown:
        return 0;
    case SampleFormat::PcmUInt8:
        return 1;
    case SampleFormat::PcmInt16:
        return 2;
    case SampleFormat::PcmInt24:
        return 3;
    case SampleFormat::PcmInt32:
    case SampleFormat::Float32:
        return 4;
    case SampleFormat::Float64:
        return 8;
    }
    return 0;
}

} // namespace sa::io
