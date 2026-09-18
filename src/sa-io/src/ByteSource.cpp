#include <sa/io/ByteSource.h>

#include <algorithm>

namespace sa::io {

std::size_t MemoryByteSource::read(std::uint64_t offset, std::span<std::byte> destination) const {
    if (offset >= bytes_.size()) {
        return 0;
    }
    const auto available = static_cast<std::size_t>(bytes_.size() - offset);
    const std::size_t count = std::min(available, destination.size());
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset), count, destination.begin());
    return count;
}

Result<std::unique_ptr<FileByteSource>> FileByteSource::open(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return Error{ErrorCode::IoFailure, "cannot stat " + path.string() + ": " + error.message()};
    }

    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        return Error{ErrorCode::IoFailure, "cannot open " + path.string()};
    }

    return std::unique_ptr<FileByteSource>{
        new FileByteSource{std::move(stream), static_cast<std::uint64_t>(size)}};
}

std::size_t FileByteSource::read(std::uint64_t offset, std::span<std::byte> destination) const {
    if (offset >= size_ || destination.empty()) {
        return 0;
    }
    const auto available = static_cast<std::size_t>(size_ - offset);
    const std::size_t count = std::min(available, destination.size());

    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream_) {
        return 0;
    }
    stream_.read(reinterpret_cast<char*>(destination.data()), static_cast<std::streamsize>(count));
    return static_cast<std::size_t>(stream_.gcount());
}

} // namespace sa::io
