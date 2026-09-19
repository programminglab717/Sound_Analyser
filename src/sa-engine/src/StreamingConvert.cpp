#include <sa/core/AudioBuffer.h>
#include <sa/core/Cancellation.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/dsp/Resampler.h>
#include <sa/engine/StreamingConvert.h>
#include <sa/io/AudioFileInfo.h>
#include <sa/io/AudioSource.h>

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace sa::engine {

namespace {

/// Fill `destination` from `source`, reading again on a short read.
///
/// A decoder is entitled to hand back fewer frames than it was asked for -- a
/// frame boundary, an internal packet edge -- and treating the first short read
/// as the end of the file would silently truncate the conversion. Only a read
/// of nothing ends the stream. `wanted` bounds the read at the frame count the
/// source claims, so a source that never says zero cannot spin here.
[[nodiscard]] Result<SampleCount> fillBlock(const io::AudioSource& source, SampleIndex startFrame,
                                            AudioBuffer& destination, SampleCount wanted) {
    SampleCount filled = 0;
    while (filled < wanted) {
        auto read =
            source.read(startFrame + filled, destination.view().subRange(filled, wanted - filled));
        if (!read) {
            return read.error();
        }
        if (read.value() <= 0) {
            break;
        }
        filled += read.value();
    }
    return filled;
}

/// Copy the source through in blocks, without a converter.
///
/// A ratio of one is a copy, and dsp::Resampler knows that and behaves like
/// one -- but it still builds a quarter-megabyte filter table per channel in
/// order to multiply by one. Changing format without changing rate is a common
/// enough request to be worth not paying for.
[[nodiscard]] Result<ConversionProgress> copyStreaming(const io::AudioSource& source,
                                                       AudioSink& sink, SampleCount blockFrames,
                                                       const JobMonitor& monitor) {
    const io::AudioFileInfo& info = source.info();
    const SampleCount totalFrames = std::max<SampleCount>(info.frameCount, 0);

    AudioBuffer block{info.layout, blockFrames};
    ConversionProgress progress;

    while (progress.framesRead < totalFrames) {
        if (monitor.shouldCancel()) {
            return Error{ErrorCode::Cancelled, "conversion cancelled"};
        }

        const SampleCount wanted =
            std::min<SampleCount>(blockFrames, totalFrames - progress.framesRead);
        auto filled = fillBlock(source, progress.framesRead, block, wanted);
        if (!filled) {
            return filled.error();
        }
        if (filled.value() <= 0) {
            // The source ended earlier than its own frame count claimed. Keep
            // what exists rather than failing over it.
            break;
        }

        if (auto written = sink.write(block.view().subRange(0, filled.value())); !written) {
            return written.error();
        }
        progress.framesRead += filled.value();
        progress.framesWritten += filled.value();
        monitor.report(static_cast<double>(progress.framesRead) / static_cast<double>(totalFrames));
    }

    monitor.report(1.0);
    return progress;
}

} // namespace

Result<ConversionProgress> convertStreaming(const io::AudioSource& source, AudioSink& sink,
                                            const ConversionSpec& spec, const JobMonitor& monitor) {
    const io::AudioFileInfo& info = source.info();

    if (!info.sampleRate.isValid()) {
        return Error{ErrorCode::InvalidArgument,
                     "the source's sample rate is not a usable audio rate"};
    }
    if (!spec.outputRate.isValid()) {
        return Error{ErrorCode::InvalidArgument, "the output rate is not a usable audio rate"};
    }
    if (spec.blockFrames <= 0) {
        return Error{ErrorCode::InvalidArgument, "the block size must be at least one frame"};
    }
    const int channels = info.channelCount();
    if (channels <= 0) {
        return Error{ErrorCode::InvalidArgument, "the source has no channels"};
    }

    if (spec.outputRate == info.sampleRate) {
        return copyStreaming(source, sink, spec.blockFrames, monitor);
    }

    // One converter per channel, because the filter history is the channel's
    // own: sharing one would fold the channels into each other, and stepping
    // them separately over a shared state would put them at different points in
    // the stream. Built from the rate pair rather than the ratio, so that where
    // both rates are whole numbers the phase steps in integers and the sampling
    // instants stay exactly on the grid however long the file runs.
    std::vector<dsp::Resampler> converters;
    converters.reserve(static_cast<std::size_t>(channels));
    for (int channel = 0; channel < channels; ++channel) {
        dsp::ResamplerSpec converterSpec;
        converterSpec.inputRate = info.sampleRate;
        converterSpec.outputRate = spec.outputRate;
        converterSpec.quality = spec.quality;

        auto converter = dsp::Resampler::create(converterSpec);
        if (!converter) {
            return converter.error();
        }
        converters.push_back(std::move(converter).value());
    }

    // Everything this function allocates is allocated here: two blocks and the
    // converters. Nothing inside the loop grows, and none of these is sized
    // from the length of the file.
    const SampleCount outputCapacity = converters.front().maximumOutputFor(spec.blockFrames);
    AudioBuffer input{info.layout, spec.blockFrames};
    AudioBuffer output{info.layout, outputCapacity};

    const SampleCount totalFrames = std::max<SampleCount>(info.frameCount, 0);
    ConversionProgress progress;

    while (progress.framesRead < totalFrames) {
        if (monitor.shouldCancel()) {
            return Error{ErrorCode::Cancelled, "conversion cancelled"};
        }

        const SampleCount wanted =
            std::min<SampleCount>(spec.blockFrames, totalFrames - progress.framesRead);
        auto filled = fillBlock(source, progress.framesRead, input, wanted);
        if (!filled) {
            return filled.error();
        }
        if (filled.value() <= 0) {
            break;
        }

        // process() stops on whichever of input and output runs out first, so a
        // block may take more than one pass. Each pass converts every channel
        // over the same span of input before anything is written, because the
        // sink takes whole frames and a channel written ahead of its neighbour
        // is a channel that has drifted.
        SampleCount consumed = 0;
        while (consumed < filled.value()) {
            dsp::ResamplerProgress step;
            for (int channel = 0; channel < channels; ++channel) {
                const auto index = static_cast<std::size_t>(channel);
                const auto channelStep = converters[index].process(
                    input.channel(channel) + consumed, filled.value() - consumed,
                    output.channel(channel), outputCapacity);
                if (channel == 0) {
                    step = channelStep;
                    continue;
                }
                // Identically configured converters fed identical counts stop
                // at identical counts -- what a converter emits depends on the
                // samples, when it can emit does not. Checked rather than
                // assumed, because the failure it guards against is channels
                // sliding apart by a sample, which is inaudible one channel at
                // a time and obvious in the stereo image.
                if (channelStep.inputConsumed != step.inputConsumed ||
                    channelStep.outputProduced != step.outputProduced) {
                    return Error{ErrorCode::Unknown,
                                 "the channels of the conversion fell out of step"};
                }
            }

            if (step.inputConsumed <= 0 && step.outputProduced <= 0) {
                return Error{ErrorCode::Unknown, "the converter stopped making progress"};
            }
            consumed += step.inputConsumed;

            if (step.outputProduced > 0) {
                if (auto written = sink.write(output.view().subRange(0, step.outputProduced));
                    !written) {
                    return written.error();
                }
                progress.framesWritten += step.outputProduced;
            }
        }

        progress.framesRead += filled.value();
        monitor.report(static_cast<double>(progress.framesRead) / static_cast<double>(totalFrames));
    }

    // The filter is centred, so the last input samples are only fully converted
    // once silence has been run in behind them. Stopping at the last process()
    // call drops that tail -- the filter's reach scaled by the ratio, so tens of
    // samples rather than one -- and leaves a file whose length is no longer
    // what the ratio implies.
    while (true) {
        if (monitor.shouldCancel()) {
            return Error{ErrorCode::Cancelled, "conversion cancelled"};
        }

        SampleCount drained = 0;
        for (int channel = 0; channel < channels; ++channel) {
            const auto index = static_cast<std::size_t>(channel);
            const SampleCount channelDrained =
                converters[index].flush(output.channel(channel), outputCapacity);
            if (channel == 0) {
                drained = channelDrained;
                continue;
            }
            if (channelDrained != drained) {
                return Error{ErrorCode::Unknown, "the channels of the conversion fell out of step"};
            }
        }

        if (drained <= 0) {
            break;
        }
        if (auto written = sink.write(output.view().subRange(0, drained)); !written) {
            return written.error();
        }
        progress.framesWritten += drained;
    }

    monitor.report(1.0);
    return progress;
}

} // namespace sa::engine
