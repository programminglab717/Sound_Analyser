#pragma once

#include <sa/io/ByteSource.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace sa::io::detail {

/// The read position a third-party decoder's callbacks work through.
///
/// dr_flac and dr_mp3 both pull bytes through read/seek/tell callbacks rather
/// than from a buffer they are handed, which is the only reason a file larger
/// than memory can be decoded at all. Both need the same three operations over
/// a ByteSource, so they share this instead of each growing its own copy.
///
/// A decoder is handed a pointer to one of these and keeps it for its lifetime,
/// so the object must not move while the decoder is open. That is why the
/// readers hold it on the heap: moving a reader then moves the owning pointer
/// and not the thing the decoder points at.
struct ByteSourceCursor {
    std::shared_ptr<const ByteSource> source;
    std::uint64_t position = 0;

    /// Total bytes in the source, as the signed type both decoders use for
    /// stream offsets.
    [[nodiscard]] std::int64_t size() const noexcept {
        return source == nullptr ? 0 : static_cast<std::int64_t>(source->size());
    }

    /// Read up to `bytes` into `destination`, advancing by however many bytes
    /// were actually read. A short read is not an error: it means end of source.
    std::size_t readInto(void* destination, std::size_t bytes) {
        if (source == nullptr || destination == nullptr || bytes == 0) {
            return 0;
        }
        const std::size_t got =
            source->read(position, std::span{static_cast<std::byte*>(destination), bytes});
        position += got;
        return got;
    }

    /// Move to an absolute byte offset, refusing anything outside the source.
    ///
    /// Refusing rather than clamping is deliberate. A decoder seeking outside
    /// the stream has trusted a length field the file lied about; clamping
    /// would quietly hand it bytes from somewhere else and let it carry on
    /// decoding them as audio, where refusing makes it stop.
    [[nodiscard]] bool seekTo(std::int64_t offset) noexcept {
        if (offset < 0 || offset > size()) {
            return false;
        }
        position = static_cast<std::uint64_t>(offset);
        return true;
    }
};

} // namespace sa::io::detail
