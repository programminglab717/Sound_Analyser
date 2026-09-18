#include <sa/core/Result.h>

namespace sa {

std::string_view toString(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Unknown:
        return "unknown error";
    case ErrorCode::InvalidArgument:
        return "invalid argument";
    case ErrorCode::OutOfRange:
        return "out of range";
    case ErrorCode::NotFound:
        return "not found";
    case ErrorCode::IoFailure:
        return "I/O failure";
    case ErrorCode::UnsupportedFormat:
        return "unsupported format";
    case ErrorCode::CorruptData:
        return "corrupt data";
    case ErrorCode::OutOfMemory:
        return "out of memory";
    case ErrorCode::Cancelled:
        return "cancelled";
    }
    return "unknown error";
}

} // namespace sa
