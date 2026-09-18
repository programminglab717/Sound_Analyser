#pragma once

#include <sa/core/Types.h>

#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace sa {

/// Coarse error category. Kept small and stable -- callers switch on this,
/// and the message carries the detail.
enum class ErrorCode {
    Unknown,
    InvalidArgument,
    OutOfRange,
    NotFound,
    IoFailure,
    UnsupportedFormat,
    CorruptData,
    OutOfMemory,
    Cancelled,
};

[[nodiscard]] std::string_view toString(ErrorCode code) noexcept;

/// An error with a human-readable message.
///
/// Note the message is a std::string, so constructing an Error allocates.
/// That is deliberate: errors are not part of any audio-thread path. Audio
/// thread code reports failure through lock-free status flags instead.
class Error {
public:
    Error() = default;

    explicit Error(ErrorCode code, std::string message = {})
        : code_(code), message_(std::move(message)) {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }

    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    /// Message if one was supplied, otherwise the category name.
    [[nodiscard]] std::string_view what() const noexcept {
        return message_.empty() ? toString(code_) : std::string_view{message_};
    }

private:
    ErrorCode code_ = ErrorCode::Unknown;
    std::string message_;
};

/// Either a value or an Error.
///
/// A deliberately small stand-in for std::expected, which is C++23. Swapping
/// to std::expected later is a mechanical change confined to this header.
template <typename T>
class Result {
public:
    Result(T value) : storage_(std::move(value)) {} // NOLINT(google-explicit-constructor)

    Result(Error error) : storage_(std::move(error)) {} // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool hasValue() const noexcept { return storage_.index() == 0; }

    explicit operator bool() const noexcept { return hasValue(); }

    [[nodiscard]] T& value() & { return std::get<0>(storage_); }

    [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }

    [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }

    [[nodiscard]] const Error& error() const& { return std::get<1>(storage_); }

    /// The value if present, otherwise `fallback`. Never throws.
    template <typename U>
    [[nodiscard]] T valueOr(U&& fallback) const& {
        return hasValue() ? std::get<0>(storage_) : static_cast<T>(std::forward<U>(fallback));
    }

private:
    std::variant<T, Error> storage_;
};

/// Result specialisation for operations that return nothing on success.
class Status {
public:
    Status() = default;

    Status(Error error) : error_(std::move(error)), ok_(false) {} // NOLINT

    [[nodiscard]] bool ok() const noexcept { return ok_; }

    explicit operator bool() const noexcept { return ok_; }

    [[nodiscard]] const Error& error() const noexcept { return error_; }

private:
    Error error_;
    bool ok_ = true;
};

} // namespace sa
