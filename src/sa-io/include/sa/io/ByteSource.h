#pragma once

#include <sa/core/Result.h>
#include <sa/core/Types.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <vector>

namespace sa::io {

/// Random-access source of bytes.
///
/// Parsers read through this rather than from a buffer, so a 10 GB file is
/// never loaded whole (docs/03-architecture.md principle 2) and tests can feed
/// a parser bytes from memory without touching the filesystem -- which is what
/// makes malformed-input testing practical.
class ByteSource {
public:
    virtual ~ByteSource() = default;

    /// Total bytes available.
    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;

    /// Read into `destination`, returning how many bytes were actually read.
    /// A short read is not an error: it means end of source.
    [[nodiscard]] virtual std::size_t read(std::uint64_t offset,
                                           std::span<std::byte> destination) const = 0;

    /// True if `length` bytes are available starting at `offset`, computed
    /// without overflowing. Parsers use this before trusting any size field
    /// read out of a file.
    [[nodiscard]] bool contains(std::uint64_t offset, std::uint64_t length) const noexcept {
        const std::uint64_t total = size();
        return offset <= total && length <= total - offset;
    }
};

/// A ByteSource over a caller-owned buffer. Does not copy.
class MemoryByteSource final : public ByteSource {
public:
    explicit MemoryByteSource(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] std::uint64_t size() const noexcept override { return bytes_.size(); }

    [[nodiscard]] std::size_t read(std::uint64_t offset,
                                   std::span<std::byte> destination) const override;

private:
    std::span<const std::byte> bytes_;
};

/// A ByteSource over a file on disk.
class FileByteSource final : public ByteSource {
public:
    [[nodiscard]] static Result<std::unique_ptr<FileByteSource>>
    open(const std::filesystem::path& path);

    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }

    [[nodiscard]] std::size_t read(std::uint64_t offset,
                                   std::span<std::byte> destination) const override;

private:
    FileByteSource(std::ifstream stream, std::uint64_t size)
        : stream_(std::move(stream)), size_(size) {}

    mutable std::ifstream stream_;
    std::uint64_t size_ = 0;
};

} // namespace sa::io
