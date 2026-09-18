#include <sa/dsp/Stft.h>
#include <sa/spectral/SpectrogramPyramid.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace sa::spectral {

namespace {

constexpr SampleCount kMinimumTopLevelFrames = 2;

} // namespace

namespace {

/// Quantise one frame of complex spectra into the pyramid's 8-bit log scale.
///
/// Shared by both builds, because two copies of this is two chances for the
/// streaming one to draw a slightly different picture from the one-shot one.
void quantiseFrame(const std::complex<float>* spectra, int bins, float reference,
                   const SpectrogramConfig& config, std::uint8_t* out) noexcept {
    const float span = config.maximumDecibels - config.minimumDecibels;
    for (int bin = 0; bin < bins; ++bin) {
        const float magnitude = std::abs(spectra[bin]) / reference;
        // Floor before the log so that true silence maps to the bottom of the
        // range rather than to negative infinity.
        const float decibels =
            magnitude > 1e-12f ? 20.0f * std::log10(magnitude) : config.minimumDecibels;
        const float normalised = (decibels - config.minimumDecibels) / span;
        out[bin] = static_cast<std::uint8_t>(std::clamp(normalised, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
}

} // namespace

void SpectrogramPyramid::buildUpperLevels() {
    const auto bins = static_cast<std::size_t>(binCount_);

    // --- Levels 1..n: max-combine pairs of frames --------------------------
    //
    // Maximum, not mean: a click that is one frame wide must stay visible at
    // every zoom level, or the display hides the thing the user opened the tool
    // to find. Quantisation is monotonic in decibels, so taking the max of the
    // stored bytes is the same as taking the max of the magnitudes.
    while (levels_.back().frameCount > kMinimumTopLevelFrames) {
        const Level& previous = levels_.back();

        Level next;
        next.hop = previous.hop * 2;
        next.frameCount = (previous.frameCount + 1) / 2;
        next.magnitudes.resize(static_cast<std::size_t>(next.frameCount) * bins);

        for (SampleCount frame = 0; frame < next.frameCount; ++frame) {
            const SampleCount firstChild = frame * 2;
            const SampleCount childCount =
                std::min<SampleCount>(2, previous.frameCount - firstChild);
            const auto destination = static_cast<std::size_t>(frame) * bins;

            const auto firstSource = static_cast<std::size_t>(firstChild) * bins;
            std::copy_n(previous.magnitudes.begin() + static_cast<std::ptrdiff_t>(firstSource),
                        bins, next.magnitudes.begin() + static_cast<std::ptrdiff_t>(destination));

            if (childCount > 1) {
                const auto secondSource = static_cast<std::size_t>(firstChild + 1) * bins;
                for (std::size_t bin = 0; bin < bins; ++bin) {
                    next.magnitudes[destination + bin] =
                        std::max(next.magnitudes[destination + bin],
                                 previous.magnitudes[secondSource + bin]);
                }
            }
        }
        levels_.push_back(std::move(next));
    }
}

Result<SpectrogramPyramid> SpectrogramPyramid::buildStreaming(const io::AudioSource& source,
                                                              int channel,
                                                              const SpectrogramConfig& config,
                                                              const JobMonitor& monitor) {
    if (config.maximumDecibels <= config.minimumDecibels) {
        return Error{ErrorCode::InvalidArgument, "maximumDecibels must exceed minimumDecibels"};
    }
    const io::AudioFileInfo& info = source.info();
    if (channel < 0 || channel >= info.channelCount()) {
        return Error{ErrorCode::OutOfRange, "channel index outside the source"};
    }

    auto stftResult = dsp::Stft::create(config.fftSize, config.hopSize, config.window);
    if (!stftResult) {
        return stftResult.error();
    }
    const dsp::Stft stft = std::move(stftResult).value();

    SpectrogramPyramid pyramid;
    pyramid.config_ = config;
    pyramid.binCount_ = stft.binCount();
    pyramid.sourceFrames_ = info.frameCount;
    if (info.frameCount <= 0) {
        return pyramid;
    }

    const SampleCount frames = stft.frameCount(info.frameCount);
    const auto bins = static_cast<std::size_t>(stft.binCount());

    Level base;
    base.hop = config.hopSize;
    base.frameCount = frames;
    base.magnitudes.assign(static_cast<std::size_t>(frames) * bins, std::uint8_t{0});

    // The framing has to match analyse() exactly: it treats the signal as
    // zero-padded at the front by fftSize - hopSize, so frame f begins at
    // f * hop - padding. Getting this wrong by one hop puts the whole
    // spectrogram out of step with the waveform above it.
    const int padding = config.fftSize - config.hopSize;
    const dsp::RealFft fft{config.fftSize};
    std::vector<std::complex<float>> spectrum(bins);
    std::vector<float> windowed(static_cast<std::size_t>(config.fftSize), 0.0f);
    const auto reference = static_cast<float>(stft.window().coherentGain() * 0.5);

    // Read in blocks and slide, rather than reading one window per frame.
    //
    // Windows overlap by fftSize - hopSize, so a read per frame reads the file
    // fftSize/hopSize times over -- four times at the display settings, and
    // that dominated the time to open a long file. This keeps a buffer that
    // always covers the window being analysed and refills it only when it runs
    // out, so the file is read once.
    constexpr SampleCount kReadBlock = 1 << 16;
    const SampleCount capacity = kReadBlock + config.fftSize;
    std::vector<float> buffer(static_cast<std::size_t>(capacity), 0.0f);
    AudioBuffer readInto{info.layout, kReadBlock};

    SampleIndex bufferStart = 0; // Sample index of buffer[0].
    SampleCount bufferFill = 0;
    SampleIndex readCursor = 0;

    for (SampleCount frame = 0; frame < frames; ++frame) {
        if (monitor.shouldCancel()) {
            return Error{ErrorCode::Cancelled, "cancelled"};
        }

        // The framing has to match analyse() exactly: it treats the signal as
        // zero-padded at the front by fftSize - hopSize, so frame f begins at
        // f * hop - padding. Getting this wrong by one hop puts the whole
        // spectrogram out of step with the waveform above it.
        const SampleIndex windowStart = frame * config.hopSize - padding;
        const SampleIndex windowEnd = windowStart + config.fftSize;

        const SampleIndex wantFrom = std::max<SampleIndex>(0, windowStart);
        const SampleIndex wantTo = std::min<SampleIndex>(info.frameCount, windowEnd);

        // Refill whenever the window runs past what is buffered.
        while (wantTo > bufferStart + bufferFill && readCursor < info.frameCount) {
            // Keep whatever of the window is already here, drop the rest.
            const SampleIndex keepFrom = std::max<SampleIndex>(bufferStart, wantFrom);
            const SampleCount keep = std::max<SampleCount>(0, bufferStart + bufferFill - keepFrom);
            if (keep > 0 && keepFrom != bufferStart) {
                std::copy(buffer.begin() + static_cast<std::ptrdiff_t>(keepFrom - bufferStart),
                          buffer.begin() +
                              static_cast<std::ptrdiff_t>(keepFrom - bufferStart + keep),
                          buffer.begin());
            }
            bufferStart = keep > 0 ? keepFrom : readCursor;
            bufferFill = keep;

            const SampleCount room = capacity - bufferFill;
            const SampleCount want =
                std::min<SampleCount>({room, kReadBlock, info.frameCount - readCursor});
            if (want <= 0) {
                break;
            }
            AudioBufferView view = readInto.view().subRange(0, want);
            const auto read = source.read(readCursor, view);
            if (!read) {
                return read.error();
            }
            const SampleCount got = read.value();
            if (got <= 0) {
                break;
            }
            std::copy_n(view.channel(channel), got,
                        buffer.begin() + static_cast<std::ptrdiff_t>(bufferFill));
            bufferFill += got;
            readCursor += got;
        }

        // Window it, zero outside the file, which is the padding analyse()
        // applies implicitly.
        for (int i = 0; i < config.fftSize; ++i) {
            const SampleIndex index = windowStart + i;
            const bool inside = index >= 0 && index < info.frameCount && index >= bufferStart &&
                                index < bufferStart + bufferFill;
            const float sample =
                inside ? buffer[static_cast<std::size_t>(index - bufferStart)] : 0.0f;
            windowed[static_cast<std::size_t>(i)] = sample * stft.window()[i];
        }
        fft.forward(windowed.data(), spectrum.data());

        quantiseFrame(spectrum.data(), stft.binCount(), reference, config,
                      base.magnitudes.data() + static_cast<std::size_t>(frame) * bins);

        if ((frame & 0xFF) == 0) {
            monitor.report(static_cast<double>(frame) / static_cast<double>(frames));
        }
    }

    pyramid.levels_.push_back(std::move(base));
    pyramid.buildUpperLevels();
    monitor.report(1.0);
    return pyramid;
}

Result<SpectrogramPyramid> SpectrogramPyramid::build(ConstAudioBufferView source, int channel,
                                                     const SpectrogramConfig& config) {
    // Emptiness first: an empty source has no channel 0, and rejecting it as an
    // out-of-range channel would be a confusing way to say "nothing to do".
    if (config.maximumDecibels <= config.minimumDecibels) {
        return Error{ErrorCode::InvalidArgument, "maximumDecibels must exceed minimumDecibels"};
    }

    auto stftResult = dsp::Stft::create(config.fftSize, config.hopSize, config.window);
    if (!stftResult) {
        return stftResult.error();
    }
    const dsp::Stft stft = std::move(stftResult).value();

    SpectrogramPyramid pyramid;
    pyramid.config_ = config;
    pyramid.binCount_ = stft.binCount();
    pyramid.sourceFrames_ = source.frames();

    if (source.isEmpty()) {
        return pyramid;
    }

    if (channel < 0 || channel >= source.channelCount()) {
        return Error{ErrorCode::OutOfRange, "channel index outside the source"};
    }

    // --- Level 0: one STFT pass over the channel ---------------------------
    const SampleCount frames = stft.frameCount(source.frames());
    const auto bins = static_cast<std::size_t>(stft.binCount());

    std::vector<std::complex<float>> spectra(static_cast<std::size_t>(frames) * bins);
    stft.analyse(source.channel(channel), source.frames(), spectra.data());

    Level base;
    base.hop = config.hopSize;
    base.frameCount = frames;
    base.magnitudes.resize(static_cast<std::size_t>(frames) * bins);

    // Normalise so a bin-centred full-scale sine reads 0 dBFS: its peak bin
    // magnitude is half the window's coherent gain.
    const auto reference = static_cast<float>(stft.window().coherentGain() * 0.5);
    const float span = config.maximumDecibels - config.minimumDecibels;

    for (std::size_t i = 0; i < spectra.size(); ++i) {
        const float magnitude = std::abs(spectra[i]) / reference;
        // Floor before the log so that true silence maps to the bottom of the
        // range rather than to negative infinity.
        const float decibels =
            magnitude > 1e-12f ? 20.0f * std::log10(magnitude) : config.minimumDecibels;
        const float normalised = (decibels - config.minimumDecibels) / span;
        base.magnitudes[i] =
            static_cast<std::uint8_t>(std::clamp(normalised, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    pyramid.levels_.push_back(std::move(base));

    pyramid.buildUpperLevels();
    return pyramid;
}

SampleCount SpectrogramPyramid::hopAt(int level) const noexcept {
    if (level < 0 || level >= levelCount()) {
        return 0;
    }
    return levels_[static_cast<std::size_t>(level)].hop;
}

SampleCount SpectrogramPyramid::frameCountAt(int level) const noexcept {
    if (level < 0 || level >= levelCount()) {
        return 0;
    }
    return levels_[static_cast<std::size_t>(level)].frameCount;
}

std::uint8_t SpectrogramPyramid::magnitudeAt(int level, SampleCount frame, int bin) const noexcept {
    if (level < 0 || level >= levelCount() || bin < 0 || bin >= binCount_) {
        return 0;
    }
    const Level& data = levels_[static_cast<std::size_t>(level)];
    if (frame < 0 || frame >= data.frameCount) {
        return 0;
    }
    return data.magnitudes[static_cast<std::size_t>(frame) * static_cast<std::size_t>(binCount_) +
                           static_cast<std::size_t>(bin)];
}

int SpectrogramPyramid::levelForSamplesPerColumn(SampleCount samplesPerColumn) const noexcept {
    int chosen = 0;
    for (int level = 0; level < levelCount(); ++level) {
        if (hopAt(level) <= samplesPerColumn) {
            chosen = level;
        } else {
            break;
        }
    }
    return chosen;
}

void SpectrogramPyramid::render(SampleIndex startSample, SampleIndex endSample,
                                const float* rowBinEdges, int rows, int columns,
                                std::uint8_t* out) const noexcept {
    if (out == nullptr || rowBinEdges == nullptr || rows <= 0 || columns <= 0) {
        return;
    }
    std::fill_n(out, static_cast<std::size_t>(rows) * static_cast<std::size_t>(columns),
                std::uint8_t{0});

    if (levels_.empty() || endSample <= startSample) {
        return;
    }

    const SampleCount span = endSample - startSample;
    const SampleCount samplesPerColumn = std::max<SampleCount>(1, span / columns);
    const int level = levelForSamplesPerColumn(samplesPerColumn);
    const Level& data = levels_[static_cast<std::size_t>(level)];
    const auto bins = static_cast<std::size_t>(binCount_);

    for (int column = 0; column < columns; ++column) {
        const SampleIndex columnStart = startSample + (span * column) / columns;
        const SampleIndex columnEnd = startSample + (span * (column + 1)) / columns;

        const SampleCount firstFrame = std::max<SampleIndex>(0, columnStart / data.hop);
        const SampleCount lastFrame =
            std::min<SampleIndex>(data.frameCount - 1, (columnEnd - 1) / data.hop);
        if (firstFrame > lastFrame) {
            continue;
        }

        for (int row = 0; row < rows; ++row) {
            const float lowEdge = rowBinEdges[row];
            const float highEdge = rowBinEdges[row + 1];

            std::uint8_t peak = 0;

            if (highEdge - lowEdge < 1.0f) {
                // The row is finer than the analysis. Taking the nearest bin
                // would draw the bottom of a log axis -- where a dozen bins are
                // stretched over half the display -- as a stack of flat blocks,
                // so interpolate between the two bins the row sits between.
                // Values are log-magnitude, so this interpolates in decibels,
                // which is the axis the eye is reading.
                const float centre = 0.5f * (lowEdge + highEdge);
                const int lower =
                    std::clamp(static_cast<int>(std::floor(centre)), 0, binCount_ - 1);
                const int upper = std::min(lower + 1, binCount_ - 1);
                const float weight = std::clamp(centre - static_cast<float>(lower), 0.0f, 1.0f);

                for (SampleCount frame = firstFrame; frame <= lastFrame; ++frame) {
                    const auto frameOffset = static_cast<std::size_t>(frame) * bins;
                    const float a = data.magnitudes[frameOffset + static_cast<std::size_t>(lower)];
                    const float b = data.magnitudes[frameOffset + static_cast<std::size_t>(upper)];
                    const float blended = a + (b - a) * weight;
                    peak = std::max(peak, static_cast<std::uint8_t>(blended + 0.5f));
                }
            } else {
                // Several bins to one row: combine by maximum, so a narrow peak
                // survives the squeeze at the top of a log axis.
                int binStart = std::clamp(static_cast<int>(std::floor(lowEdge)), 0, binCount_ - 1);
                const int binEnd =
                    std::clamp(static_cast<int>(std::ceil(highEdge)), binStart + 1, binCount_);

                for (SampleCount frame = firstFrame; frame <= lastFrame; ++frame) {
                    const auto frameOffset = static_cast<std::size_t>(frame) * bins;
                    for (int bin = binStart; bin < binEnd; ++bin) {
                        peak = std::max(
                            peak, data.magnitudes[frameOffset + static_cast<std::size_t>(bin)]);
                    }
                }
            }

            out[static_cast<std::size_t>(row) * static_cast<std::size_t>(columns) +
                static_cast<std::size_t>(column)] = peak;
        }
    }
}

const std::uint8_t* SpectrogramPyramid::frameData(int level, SampleCount frame) const noexcept {
    if (level < 0 || level >= levelCount()) {
        return nullptr;
    }
    const Level& data = levels_[static_cast<std::size_t>(level)];
    if (frame < 0 || frame >= data.frameCount) {
        return nullptr;
    }
    return data.magnitudes.data() +
           static_cast<std::size_t>(frame) * static_cast<std::size_t>(binCount_);
}

float SpectrogramPyramid::toDecibels(std::uint8_t value) const noexcept {
    const float span = config_.maximumDecibels - config_.minimumDecibels;
    return config_.minimumDecibels + span * static_cast<float>(value) / 255.0f;
}

void SpectrogramPyramid::render(SampleIndex startSample, SampleIndex endSample, int firstBin,
                                int rows, int columns, std::uint8_t* out) const noexcept {
    if (out == nullptr || rows <= 0 || columns <= 0) {
        return;
    }
    std::fill_n(out, static_cast<std::size_t>(rows) * static_cast<std::size_t>(columns),
                std::uint8_t{0});

    if (levels_.empty() || endSample <= startSample) {
        return;
    }

    const SampleCount span = endSample - startSample;
    const SampleCount samplesPerColumn = std::max<SampleCount>(1, span / columns);
    const int level = levelForSamplesPerColumn(samplesPerColumn);
    const Level& data = levels_[static_cast<std::size_t>(level)];
    const auto bins = static_cast<std::size_t>(binCount_);

    // Bins map to rows by nearest whole-bin ranges; more bins than rows means
    // several bins collapse into one row, again by maximum.
    const int availableBins = binCount_ - firstBin;
    if (availableBins <= 0) {
        return;
    }

    for (int column = 0; column < columns; ++column) {
        const SampleIndex columnStart = startSample + (span * column) / columns;
        const SampleIndex columnEnd = startSample + (span * (column + 1)) / columns;

        const SampleCount firstFrame = std::max<SampleIndex>(0, columnStart / data.hop);
        const SampleCount lastFrame =
            std::min<SampleIndex>(data.frameCount - 1, (columnEnd - 1) / data.hop);
        if (firstFrame > lastFrame) {
            continue;
        }

        for (int row = 0; row < rows; ++row) {
            const int binStart = firstBin + (availableBins * row) / rows;
            const int binEnd =
                std::max(binStart + 1, firstBin + (availableBins * (row + 1)) / rows);

            std::uint8_t peak = 0;
            for (SampleCount frame = firstFrame; frame <= lastFrame; ++frame) {
                const auto frameOffset = static_cast<std::size_t>(frame) * bins;
                for (int bin = binStart; bin < binEnd && bin < binCount_; ++bin) {
                    peak = std::max(peak,
                                    data.magnitudes[frameOffset + static_cast<std::size_t>(bin)]);
                }
            }
            out[static_cast<std::size_t>(row) * static_cast<std::size_t>(columns) +
                static_cast<std::size_t>(column)] = peak;
        }
    }
}

std::size_t SpectrogramPyramid::memoryFootprint() const noexcept {
    std::size_t bytes = 0;
    for (const Level& level : levels_) {
        bytes += level.magnitudes.size();
    }
    return bytes;
}

} // namespace sa::spectral
