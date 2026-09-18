#include <sa/io/WavReader.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <string>

namespace sa::io {

namespace {

/// Explicit little-endian decoding rather than memcpy onto a native type: WAV
/// is little-endian by specification, and reading it through host layout would
/// silently produce nonsense on a big-endian target.
std::uint16_t readU16(const std::byte* p) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[0])) |
        static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[1]) << 8));
}

std::uint32_t readU32(const std::byte* p) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1])) << 8) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2])) << 16) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[3])) << 24);
}

std::uint64_t readU64(const std::byte* p) noexcept {
    return static_cast<std::uint64_t>(readU32(p)) |
           (static_cast<std::uint64_t>(readU32(p + 4)) << 32);
}

/// FourCC as a single integer, so chunk dispatch is a switch rather than a
/// chain of memcmp calls.
constexpr std::uint32_t fourCc(const char (&tag)[5]) noexcept {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(tag[0])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(tag[1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(tag[2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(tag[3])) << 24);
}

constexpr std::uint32_t kRiff = fourCc("RIFF");
constexpr std::uint32_t kRf64 = fourCc("RF64");
constexpr std::uint32_t kWave = fourCc("WAVE");
constexpr std::uint32_t kFmt = fourCc("fmt ");
constexpr std::uint32_t kData = fourCc("data");
constexpr std::uint32_t kDs64 = fourCc("ds64");
constexpr std::uint32_t kBext = fourCc("bext");
constexpr std::uint32_t kList = fourCc("LIST");
constexpr std::uint32_t kInfo = fourCc("INFO");
constexpr std::uint32_t kAdtl = fourCc("adtl");
constexpr std::uint32_t kLabl = fourCc("labl");
constexpr std::uint32_t kCue = fourCc("cue ");
constexpr std::uint32_t kInam = fourCc("INAM");
constexpr std::uint32_t kIart = fourCc("IART");
constexpr std::uint32_t kIcmt = fourCc("ICMT");
constexpr std::uint32_t kIsft = fourCc("ISFT");
constexpr std::uint32_t kIcrd = fourCc("ICRD");

constexpr std::uint16_t kFormatPcm = 0x0001;
constexpr std::uint16_t kFormatFloat = 0x0003;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;

/// Chunks larger than this are rejected outright. Metadata chunks are kilobytes
/// in practice; a header claiming hundreds of megabytes of `bext` is either
/// corrupt or hostile, and we would rather refuse than allocate for it.
constexpr std::uint32_t kMaxMetadataChunkBytes = 16u * 1024u * 1024u;

/// Read a fixed-size chunk into a vector, refusing anything implausible.
bool readChunkBytes(const ByteSource& source, std::uint64_t offset, std::uint32_t size,
                    std::vector<std::byte>& out) {
    if (size == 0 || size > kMaxMetadataChunkBytes || !source.contains(offset, size)) {
        return false;
    }
    out.resize(size);
    return source.read(offset, out) == size;
}

/// Trim at the first NUL and strip trailing whitespace. RIFF strings are
/// NUL-padded and frequently not NUL-terminated at all.
std::string toTrimmedString(const std::byte* data, std::size_t length) {
    std::size_t actual = 0;
    while (actual < length && std::to_integer<char>(data[actual]) != '\0') {
        ++actual;
    }
    std::string text(reinterpret_cast<const char*>(data), actual);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\r' || text.back() == '\n')) {
        text.pop_back();
    }
    return text;
}

SampleFormat resolveFormat(std::uint16_t formatTag, std::uint16_t bitsPerSample) noexcept {
    if (formatTag == kFormatFloat) {
        if (bitsPerSample == 32) {
            return SampleFormat::Float32;
        }
        if (bitsPerSample == 64) {
            return SampleFormat::Float64;
        }
        return SampleFormat::Unknown;
    }
    if (formatTag == kFormatPcm) {
        switch (bitsPerSample) {
        case 8:
            return SampleFormat::PcmUInt8;
        case 16:
            return SampleFormat::PcmInt16;
        case 24:
            return SampleFormat::PcmInt24;
        case 32:
            return SampleFormat::PcmInt32;
        default:
            return SampleFormat::Unknown;
        }
    }
    return SampleFormat::Unknown;
}

/// Decode one sample to float32 in [-1, 1].
float decodeSample(const std::byte* p, SampleFormat format) noexcept {
    switch (format) {
    case SampleFormat::PcmUInt8:
        // 8-bit WAV is unsigned with 128 as silence, unlike every other depth.
        return (static_cast<float>(std::to_integer<std::uint8_t>(*p)) - 128.0f) / 128.0f;
    case SampleFormat::PcmInt16:
        return static_cast<float>(static_cast<std::int16_t>(readU16(p))) / 32768.0f;
    case SampleFormat::PcmInt24: {
        const std::uint32_t raw =
            static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0])) |
            (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1])) << 8) |
            (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2])) << 16);
        // Sign-extend from 24 bits.
        const std::int32_t value = (raw & 0x800000u) != 0
                                       ? static_cast<std::int32_t>(raw | 0xFF000000u)
                                       : static_cast<std::int32_t>(raw);
        return static_cast<float>(value) / 8388608.0f;
    }
    case SampleFormat::PcmInt32:
        return static_cast<float>(static_cast<std::int32_t>(readU32(p))) / 2147483648.0f;
    case SampleFormat::Float32:
        return std::bit_cast<float>(readU32(p));
    case SampleFormat::Float64:
        return static_cast<float>(std::bit_cast<double>(readU64(p)));
    case SampleFormat::Unknown:
        return 0.0f;
    }
    return 0.0f;
}

} // namespace

Result<WavReader> WavReader::open(const std::filesystem::path& path) {
    auto source = FileByteSource::open(path);
    if (!source) {
        return source.error();
    }
    return parse(std::shared_ptr<const ByteSource>{std::move(source).value()});
}

Result<WavReader> WavReader::fromMemory(std::span<const std::byte> bytes) {
    return parse(std::make_shared<const MemoryByteSource>(bytes));
}

Result<WavReader> WavReader::parse(std::shared_ptr<const ByteSource> source) {
    std::array<std::byte, 12> header{};
    if (source->read(0, header) != header.size()) {
        return Error{ErrorCode::CorruptData, "file is too short to be a WAV"};
    }

    const std::uint32_t riffTag = readU32(header.data());
    if (riffTag != kRiff && riffTag != kRf64) {
        return Error{ErrorCode::UnsupportedFormat, "not a RIFF or RF64 container"};
    }
    if (readU32(header.data() + 8) != kWave) {
        return Error{ErrorCode::UnsupportedFormat, "RIFF container is not WAVE"};
    }

    WavReader reader;
    reader.source_ = std::move(source);
    const ByteSource& bytes = *reader.source_;

    bool haveFormat = false;
    std::uint64_t dataBytes = 0;
    bool haveData = false;
    // RF64 carries the real data size out of band, because the 32-bit field
    // cannot express it.
    std::uint64_t rf64DataBytes = 0;
    bool haveRf64Size = false;
    std::vector<SampleIndex> cuePositions;

    std::uint64_t offset = 12;
    while (offset + 8 <= bytes.size()) {
        std::array<std::byte, 8> chunkHeader{};
        if (bytes.read(offset, chunkHeader) != chunkHeader.size()) {
            break;
        }
        const std::uint32_t id = readU32(chunkHeader.data());
        const std::uint32_t declaredSize = readU32(chunkHeader.data() + 4);
        const std::uint64_t bodyOffset = offset + 8;

        // The declared size is attacker-controlled. Clamp it to what the source
        // actually holds before it is used for anything.
        const std::uint64_t available = bytes.size() - std::min(bodyOffset, bytes.size());
        const std::uint64_t bodySize = std::min<std::uint64_t>(declaredSize, available);

        std::vector<std::byte> body;

        if (id == kFmt && bodySize >= 16) {
            if (readChunkBytes(bytes, bodyOffset, static_cast<std::uint32_t>(bodySize), body)) {
                std::uint16_t formatTag = readU16(body.data());
                const auto channels = readU16(body.data() + 2);
                const auto rate = readU32(body.data() + 4);
                auto bits = readU16(body.data() + 14);

                // WAVE_FORMAT_EXTENSIBLE hides the real format tag in the first
                // two bytes of its SubFormat GUID.
                if (formatTag == kFormatExtensible && bodySize >= 40) {
                    formatTag = readU16(body.data() + 24);
                    const auto validBits = readU16(body.data() + 18);
                    if (validBits != 0 && validBits <= bits) {
                        bits = validBits <= 8    ? 8
                               : validBits <= 16 ? 16
                               : validBits <= 24 ? 24
                                                 : bits;
                    }
                }

                if (channels == 0 || channels > kMaxChannels) {
                    return Error{ErrorCode::UnsupportedFormat, "unsupported channel count"};
                }
                const SampleRate sampleRate{static_cast<double>(rate)};
                if (!sampleRate.isValid()) {
                    return Error{ErrorCode::UnsupportedFormat, "unsupported sample rate"};
                }
                const SampleFormat format = resolveFormat(formatTag, bits);
                if (format == SampleFormat::Unknown) {
                    return Error{ErrorCode::UnsupportedFormat,
                                 "unsupported sample format (tag " + std::to_string(formatTag) +
                                     ", " + std::to_string(bits) + " bits)"};
                }

                reader.info_.sampleRate = sampleRate;
                reader.info_.layout = channels == 1   ? ChannelLayout::mono()
                                      : channels == 2 ? ChannelLayout::stereo()
                                                      : ChannelLayout::discrete(channels);
                reader.info_.format = format;
                reader.bytesPerFrame_ =
                    static_cast<std::uint32_t>(bytesPerSample(format)) * channels;
                haveFormat = true;
            }
        } else if (id == kData) {
            reader.dataOffset_ = bodyOffset;
            dataBytes = bodySize;
            haveData = true;
        } else if (id == kDs64 && bodySize >= 16) {
            if (readChunkBytes(bytes, bodyOffset, static_cast<std::uint32_t>(bodySize), body)) {
                rf64DataBytes = readU64(body.data() + 8);
                haveRf64Size = true;
            }
        } else if (id == kBext && bodySize >= 346) {
            if (readChunkBytes(bytes, bodyOffset, static_cast<std::uint32_t>(bodySize), body)) {
                BroadcastInfo& broadcast = reader.metadata_.broadcast;
                broadcast.description = toTrimmedString(body.data(), 256);
                broadcast.originator = toTrimmedString(body.data() + 256, 32);
                broadcast.originatorReference = toTrimmedString(body.data() + 288, 32);
                broadcast.originationDate = toTrimmedString(body.data() + 320, 10);
                broadcast.originationTime = toTrimmedString(body.data() + 330, 8);
                broadcast.timeReference = readU64(body.data() + 338);
                broadcast.present = true;
            }
        } else if (id == kCue && bodySize >= 4) {
            if (readChunkBytes(bytes, bodyOffset, static_cast<std::uint32_t>(bodySize), body)) {
                const std::uint32_t declared = readU32(body.data());
                // Trust the buffer we actually have, not the count in the file.
                const std::uint64_t fits = (body.size() - 4) / 24;
                const auto count =
                    static_cast<std::uint32_t>(std::min<std::uint64_t>(declared, fits));
                for (std::uint32_t i = 0; i < count; ++i) {
                    const std::byte* point = body.data() + 4 + static_cast<std::size_t>(i) * 24;
                    cuePositions.push_back(static_cast<SampleIndex>(readU32(point + 20)));
                }
            }
        } else if (id == kList && bodySize >= 4) {
            if (readChunkBytes(bytes, bodyOffset, static_cast<std::uint32_t>(bodySize), body)) {
                const std::uint32_t listType = readU32(body.data());
                std::size_t cursor = 4;
                while (cursor + 8 <= body.size()) {
                    const std::uint32_t subId = readU32(body.data() + cursor);
                    const std::uint32_t subSize = readU32(body.data() + cursor + 4);
                    const std::size_t subBody = cursor + 8;
                    if (subSize > body.size() - subBody) {
                        break;
                    }
                    const std::byte* text = body.data() + subBody;

                    if (listType == kInfo) {
                        std::string value = toTrimmedString(text, subSize);
                        switch (subId) {
                        case kInam:
                            reader.metadata_.title = std::move(value);
                            break;
                        case kIart:
                            reader.metadata_.artist = std::move(value);
                            break;
                        case kIcmt:
                            reader.metadata_.comment = std::move(value);
                            break;
                        case kIsft:
                            reader.metadata_.software = std::move(value);
                            break;
                        case kIcrd:
                            reader.metadata_.date = std::move(value);
                            break;
                        default:
                            break;
                        }
                    } else if (listType == kAdtl && subId == kLabl && subSize >= 4) {
                        FileMarker marker;
                        marker.label = toTrimmedString(text + 4, subSize - 4);
                        marker.position = 0; // resolved against cue points below
                        reader.metadata_.markers.push_back(std::move(marker));
                    }

                    // Sub-chunks pad to even, same as top-level chunks.
                    cursor = subBody + subSize + (subSize & 1u);
                }
            }
        }

        // Advance is always at least 8, so a zero-size chunk cannot stall the
        // scan -- the classic way a container parser hangs on a crafted file.
        offset = bodyOffset + bodySize + (bodySize & 1u);
    }

    if (!haveFormat) {
        return Error{ErrorCode::CorruptData, "WAV has no fmt chunk"};
    }
    if (!haveData) {
        return Error{ErrorCode::CorruptData, "WAV has no data chunk"};
    }
    if (reader.bytesPerFrame_ == 0) {
        return Error{ErrorCode::CorruptData, "WAV frame size resolved to zero"};
    }

    if (haveRf64Size && rf64DataBytes > dataBytes) {
        const std::uint64_t availableFromData = bytes.size() - reader.dataOffset_;
        dataBytes = std::min(rf64DataBytes, availableFromData);
    }
    reader.info_.frameCount = static_cast<SampleCount>(dataBytes / reader.bytesPerFrame_);

    // Attach labels to cue positions in order; files that supply one without
    // the other still yield usable markers.
    for (std::size_t i = 0; i < cuePositions.size(); ++i) {
        if (i < reader.metadata_.markers.size()) {
            reader.metadata_.markers[i].position = cuePositions[i];
        } else {
            reader.metadata_.markers.push_back(FileMarker{cuePositions[i], 0, {}});
        }
    }

    return reader;
}

Result<SampleCount> WavReader::read(SampleIndex startFrame, AudioBufferView destination) const {
    if (source_ == nullptr) {
        return Error{ErrorCode::InvalidArgument, "reader is not open"};
    }
    if (startFrame < 0) {
        return Error{ErrorCode::OutOfRange, "startFrame is negative"};
    }
    if (destination.isEmpty() || startFrame >= info_.frameCount) {
        return SampleCount{0};
    }

    const SampleCount wanted = std::min(destination.frames(), info_.frameCount - startFrame);
    const int fileChannels = info_.channelCount();
    const int copyChannels = std::min(fileChannels, destination.channelCount());
    const int sampleBytes = bytesPerSample(info_.format);

    // Fixed stack block: streaming reads must not allocate per call, and a
    // 16 KiB frame is safe on any thread stack we create.
    constexpr std::size_t kBlockBytes = 16 * 1024;
    std::array<std::byte, kBlockBytes> block{};
    const auto framesPerBlock =
        static_cast<SampleCount>(kBlockBytes / std::max<std::uint32_t>(1, bytesPerFrame_));
    if (framesPerBlock == 0) {
        return Error{ErrorCode::UnsupportedFormat, "frame larger than the read block"};
    }

    SampleCount done = 0;
    while (done < wanted) {
        const SampleCount batch = std::min(framesPerBlock, wanted - done);
        const std::uint64_t byteOffset =
            dataOffset_ + static_cast<std::uint64_t>(startFrame + done) * bytesPerFrame_;
        const auto byteCount = static_cast<std::size_t>(batch) * bytesPerFrame_;

        const std::size_t got = source_->read(byteOffset, std::span{block.data(), byteCount});
        const auto framesGot = static_cast<SampleCount>(got / bytesPerFrame_);
        if (framesGot == 0) {
            break; // truncated file: report what we managed rather than failing
        }

        for (int channel = 0; channel < copyChannels; ++channel) {
            float* out = destination.channel(channel) + done;
            const std::byte* base = block.data() + static_cast<std::size_t>(channel) *
                                                       static_cast<std::size_t>(sampleBytes);
            for (SampleCount frame = 0; frame < framesGot; ++frame) {
                out[frame] = decodeSample(base + static_cast<std::size_t>(frame) * bytesPerFrame_,
                                          info_.format);
            }
        }

        done += framesGot;
        if (framesGot < batch) {
            break;
        }
    }

    return done;
}

Result<AudioBuffer> WavReader::readAll() const {
    AudioBuffer buffer{info_.layout, info_.frameCount};
    if (info_.frameCount == 0) {
        return buffer;
    }
    auto framesRead = read(0, buffer.view());
    if (!framesRead) {
        return framesRead.error();
    }
    return buffer;
}

} // namespace sa::io
