/// Headless driver for the engine.
///
/// The architecture doc asks for this early as a forcing function: everything
/// it can do, it does without Qt, which is what keeps the engine from quietly
/// growing a dependency on the window. If a feature cannot be reached from
/// here, it is in the wrong layer.
///
/// Argument parsing is hand-rolled. A parser library would be a dependency, a
/// licence entry and a build-system entry for something that is forty lines,
/// and this project's rule is that a dependency has to earn its place.

#include <sa/analysis/ComplianceTarget.h>
#include <sa/analysis/LoudnessMeter.h>
#include <sa/analysis/SignalStatistics.h>
#include <sa/analysis/TruePeakMeter.h>
#include <sa/dsp/Declick.h>
#include <sa/dsp/Declip.h>
#include <sa/dsp/Dehum.h>
#include <sa/dsp/Resampler.h>
#include <sa/dsp/TimeStretch.h>
#include <sa/engine/BufferSource.h>
#include <sa/engine/Consolidate.h>
#include <sa/engine/Document.h>
#include <sa/engine/DocumentSource.h>
#include <sa/engine/Edits.h>
#include <sa/engine/SessionFile.h>
#include <sa/io/AudioFile.h>
#include <sa/io/WavWriter.h>
#include <sa/spectral/Denoise.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr sa::SampleCount kBlock = 65536;

struct Options {
    std::vector<std::string> positional;
    std::vector<std::pair<std::string, std::string>> named;

    [[nodiscard]] std::optional<std::string> value(std::string_view name) const {
        for (const auto& [key, held] : named) {
            if (key == name) {
                return held;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool has(std::string_view name) const { return value(name).has_value(); }

    [[nodiscard]] double number(std::string_view name, double fallback) const {
        const auto held = value(name);
        if (!held) {
            return fallback;
        }
        try {
            return std::stod(*held);
        } catch (...) {
            return fallback;
        }
    }
};

/// `--name value` and `--name=value` and bare `--flag`. Everything else is
/// positional.
[[nodiscard]] Options parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string argument = argv[i];
        if (argument.rfind("--", 0) != 0) {
            options.positional.push_back(std::move(argument));
            continue;
        }
        argument.erase(0, 2);
        const auto equals = argument.find('=');
        if (equals != std::string::npos) {
            options.named.emplace_back(argument.substr(0, equals), argument.substr(equals + 1));
        } else if (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0) {
            options.named.emplace_back(std::move(argument), argv[++i]);
        } else {
            options.named.emplace_back(std::move(argument), "");
        }
    }
    return options;
}

int fail(const std::string& message) {
    std::fprintf(stderr, "sa-cli: %s\n", message.c_str());
    return 1;
}

[[nodiscard]] std::shared_ptr<const sa::io::AudioSource> open(const std::filesystem::path& path,
                                                              std::string& error) {
    auto opened = sa::io::openAudioFile(path);
    if (!opened) {
        error = path.string() + ": " + std::string{opened.error().what()};
        return nullptr;
    }
    return opened.value();
}

/// Write a source out blockwise, so a long file costs one block of memory.
[[nodiscard]] bool write(const sa::io::AudioSource& source, const std::filesystem::path& path,
                         sa::io::SampleFormat format, std::string& error) {
    std::ofstream stream{path, std::ios::binary};
    if (!stream) {
        error = "could not write " + path.string();
        return false;
    }
    sa::io::WavOptions options;
    options.format = format;

    const sa::io::AudioFileInfo& info = source.info();
    auto writer = sa::io::WavWriter::create(stream, info.sampleRate, info.layout, options);
    if (!writer) {
        error = std::string{writer.error().what()};
        return false;
    }

    sa::AudioBuffer block{info.layout, kBlock};
    sa::SampleIndex cursor = 0;
    while (true) {
        sa::AudioBufferView view = block.view();
        const auto read = source.read(cursor, view);
        if (!read) {
            error = std::string{read.error().what()};
            return false;
        }
        if (read.value() <= 0) {
            break;
        }
        if (!writer.value().write(view.subRange(0, read.value()))) {
            error = "the write failed partway through";
            return false;
        }
        cursor += read.value();
    }
    if (!writer.value().finish()) {
        error = "could not finish " + path.string();
        return false;
    }
    return true;
}

[[nodiscard]] sa::io::SampleFormat formatFrom(const Options& options,
                                              sa::io::SampleFormat fallback) {
    const auto named = options.value("format");
    if (!named) {
        return fallback;
    }
    if (*named == "16") {
        return sa::io::SampleFormat::PcmInt16;
    }
    if (*named == "24") {
        return sa::io::SampleFormat::PcmInt24;
    }
    if (*named == "32" || *named == "float") {
        return sa::io::SampleFormat::Float32;
    }
    return fallback;
}

struct Measurement {
    sa::analysis::LoudnessMeasurement loudness;
    double truePeakDbtp = sa::analysis::kDecibelFloor;
    sa::analysis::SignalStatistics statistics;

    /// Whether the true peak above is the exact reconstruction or the streaming
    /// meter's estimate. A compliance report that does not say which is one
    /// nobody can check.
    bool truePeakIsExact = false;
};

/// Stream the whole source through all three meters.
[[nodiscard]] bool measure(const sa::io::AudioSource& source, Measurement& out,
                           std::string& error) {
    const sa::io::AudioFileInfo& info = source.info();
    auto loudness = sa::analysis::LoudnessMeter::create(info.sampleRate, info.layout);
    auto peaks = sa::analysis::TruePeakMeter::create(info.channelCount());
    auto statistics = sa::analysis::SignalStatisticsMeter::create(info.channelCount());
    if (!loudness || !peaks || !statistics) {
        error = "could not create the meters for this layout";
        return false;
    }

    sa::AudioBuffer block{info.layout, kBlock};
    sa::SampleIndex cursor = 0;
    while (true) {
        sa::AudioBufferView view = block.view();
        const auto read = source.read(cursor, view);
        if (!read || read.value() <= 0) {
            break;
        }
        const sa::ConstAudioBufferView filled = view.subRange(0, read.value());
        loudness.value().process(filled);
        peaks.value().process(filled);
        statistics.value().process(filled);
        cursor += read.value();
    }

    out.loudness = loudness.value().measurement();
    out.truePeakDbtp = peaks.value().truePeakDbtp();

    // Exact where the file fits. The streaming meter is an interpolator and
    // interpolators droop: ours reads up to 0.44 dB low on bright transients,
    // and a compliance report wrong in that direction is worse than none.
    constexpr std::size_t kExactBudgetBytes = 800'000'000;
    const auto bytes = static_cast<std::size_t>(info.frameCount) *
                       static_cast<std::size_t>(info.channelCount()) * sizeof(float);
    if (cursor > 0 && bytes <= kExactBudgetBytes) {
        sa::AudioBuffer whole{info.layout, cursor};
        if (const auto read = source.read(0, whole.view()); read && read.value() == cursor) {
            if (auto exact = sa::analysis::exactTruePeakDbtp(whole.constView()); exact) {
                out.truePeakDbtp = exact.value();
                out.truePeakIsExact = true;
            }
        }
    }

    out.statistics = statistics.value().statistics(out.truePeakDbtp, out.loudness.integratedLufs);
    return true;
}

void printMeasurement(const std::filesystem::path& path, const sa::io::AudioFileInfo& info,
                      const Measurement& measurement, bool asJson) {
    const auto& loudness = measurement.loudness;
    const bool gated = loudness.gatedBlockCount > 0;

    if (asJson) {
        std::printf("{\n");
        std::printf("  \"file\": \"%s\",\n", path.filename().string().c_str());
        std::printf("  \"sampleRate\": %d,\n", static_cast<int>(info.sampleRate.hz()));
        std::printf("  \"channels\": %d,\n", info.channelCount());
        std::printf("  \"frames\": %lld,\n", static_cast<long long>(info.frameCount));
        std::printf("  \"seconds\": %.6f,\n", info.durationSeconds());
        std::printf("  \"format\": \"%s\",\n", std::string{sa::io::toString(info.format)}.c_str());
        // Null rather than a floor value: "nothing cleared the gate" is not a
        // loudness, and a consumer that averages our output must not be handed
        // -200 as though it were one.
        if (gated) {
            std::printf("  \"integratedLufs\": %.3f,\n", loudness.integratedLufs);
            std::printf("  \"loudnessRangeLu\": %.3f,\n", loudness.loudnessRangeLu);
        } else {
            std::printf("  \"integratedLufs\": null,\n");
            std::printf("  \"loudnessRangeLu\": null,\n");
        }
        std::printf("  \"maxShortTermLufs\": %.3f,\n", loudness.maximumShortTermLufs);
        std::printf("  \"maxMomentaryLufs\": %.3f,\n", loudness.maximumMomentaryLufs);
        std::printf("  \"truePeakDbtp\": %.3f,\n", measurement.truePeakDbtp);
        std::printf("  \"samplePeakDbfs\": %.3f,\n", measurement.statistics.samplePeakDbfs);
        std::printf("  \"rmsDbfs\": %.3f,\n", measurement.statistics.rmsDbfs);
        std::printf("  \"crestFactorDb\": %.3f,\n", measurement.statistics.crestFactorDb);
        std::printf("  \"dcOffset\": %.6f\n", measurement.statistics.dcOffset);
        std::printf("}\n");
        return;
    }

    std::printf("%s\n", path.filename().string().c_str());
    std::printf("  %d Hz, %d ch, %s, %.2f s\n", static_cast<int>(info.sampleRate.hz()),
                info.channelCount(), std::string{sa::io::toString(info.format)}.c_str(),
                info.durationSeconds());
    if (gated) {
        std::printf("  integrated   %8.2f LUFS\n", loudness.integratedLufs);
        std::printf("  range        %8.2f LU\n", loudness.loudnessRangeLu);
    } else {
        std::printf("  integrated         -- (nothing cleared the absolute gate)\n");
        std::printf("  range              --\n");
    }
    std::printf("  max short    %8.2f LUFS\n", loudness.maximumShortTermLufs);
    std::printf("  true peak    %8.2f dBTP\n", measurement.truePeakDbtp);
    std::printf("  sample peak  %8.2f dBFS\n", measurement.statistics.samplePeakDbfs);
    std::printf("  rms          %8.2f dBFS\n", measurement.statistics.rmsDbfs);
    std::printf("  crest        %8.2f dB\n", measurement.statistics.crestFactorDb);
    std::printf("  dc offset    %8.5f\n", measurement.statistics.dcOffset);
}

void usage() {
    std::printf(R"(sa-cli -- headless driver for Sound Analyser

  analyse <file>... [--json]
      Measure loudness, peaks and statistics.

  convert <in> <out> [--rate <hz>] [--format 16|24|float]
      Convert sample rate and format. Resampling is the Kaiser-windowed-sinc
      converter: 141 dB stopband at best quality.

  normalise <in> <out> --target <name> [--format 16|24|float]
      Measure, apply the gain that meets the target without breaching its true
      peak ceiling, and write. Targets: )");
    for (std::size_t i = 0; i < sa::analysis::targetCount(); ++i) {
        const auto& target = sa::analysis::allTargets()[i];
        std::printf("%s%.*s", i == 0 ? "" : ", ", static_cast<int>(target.name.size()),
                    target.name.data());
    }
    std::printf(R"(

  denoise <in> <out> --noise <from>-<to> [--amount <dB>] [--format 16|24|float]
      Learn a noise profile from <from>-<to> in seconds, then clean the file.

  dehum <in> <out> [--frequency <hz>] [--amount <0..1>] [--format 16|24|float]
      Find mains hum and subtract it. Prints the frequency it found and how
      many partials it took out; finding none is success, not failure.

  declip <in> <out> [--keep-level] [--format 16|24|float]
      Restore clipped peaks. The result is brought down to fit them unless
      --keep-level says otherwise, and the gain applied is printed.

  declick <in> <out> [--sensitivity <n>] [--format 16|24|float]
      Find and repair clicks. Prints how many it found, and how many stretches
      of damage were too long to be clicks and were left alone.

  stretch <in> <out> --length <percent> [--format 16|24|float]
      Change how long it lasts without changing its pitch. 200 is twice as
      long, 50 is half.

  pitch <in> <out> --semitones <n> [--format 16|24|float]
      Change its pitch without changing how long it lasts. Fractions are
      allowed, and 0.01 of a semitone is a cent.

  render <session.sa> <out.wav> [--format 16|24|float]
      Render a saved arrangement to audio.

Every command returns 0 on success and 1 on failure, and writes nothing to
stdout on failure, so it composes in a script.
)");
}

int analyse(const Options& options) {
    if (options.positional.size() < 2) {
        return fail("analyse needs at least one file");
    }
    const bool asJson = options.has("json");
    int failures = 0;

    for (std::size_t i = 1; i < options.positional.size(); ++i) {
        const std::filesystem::path path = options.positional[i];
        std::string error;
        const auto source = open(path, error);
        if (!source) {
            ++failures;
            std::fprintf(stderr, "sa-cli: %s\n", error.c_str());
            continue;
        }
        Measurement measurement;
        if (!measure(*source, measurement, error)) {
            ++failures;
            std::fprintf(stderr, "sa-cli: %s\n", error.c_str());
            continue;
        }
        printMeasurement(path, source->info(), measurement, asJson);
    }
    return failures == 0 ? 0 : 1;
}

int convert(const Options& options) {
    if (options.positional.size() != 3) {
        return fail("convert needs an input and an output");
    }
    std::string error;
    const auto source = open(options.positional[1], error);
    if (!source) {
        return fail(error);
    }

    const sa::io::AudioFileInfo& info = source->info();
    const auto wanted = options.number("rate", info.sampleRate.hz());
    const auto format = formatFrom(options, info.format);

    if (std::abs(wanted - info.sampleRate.hz()) < 0.5) {
        return write(*source, options.positional[2], format, error) ? 0 : fail(error);
    }

    // Resample into memory, then write. Streaming straight through would be
    // better for very long files and is worth doing; this is honest about
    // holding the result, and refuses rather than thrashing when it will not
    // fit.
    const double ratio = wanted / info.sampleRate.hz();
    const auto outputFrames =
        static_cast<sa::SampleCount>(std::ceil(static_cast<double>(info.frameCount) * ratio));
    if (static_cast<std::size_t>(outputFrames) * static_cast<std::size_t>(info.channelCount()) *
            sizeof(float) >
        2'000'000'000ULL) {
        return fail("the converted file would not fit in memory; streaming conversion is not "
                    "implemented yet");
    }

    sa::AudioBuffer input{info.layout, info.frameCount};
    if (const auto read = source->read(0, input.view()); !read) {
        return fail(std::string{read.error().what()});
    }

    sa::AudioBuffer output{info.layout, outputFrames};
    for (int channel = 0; channel < info.channelCount(); ++channel) {
        // Built from the rate pair, not the ratio: where the two rates are
        // whole numbers the converter steps its phase in integers and stays
        // exactly on the grid however long the stream runs. Handing it a real
        // number instead gives up that guarantee for nothing.
        sa::dsp::ResamplerSpec spec;
        spec.inputRate = info.sampleRate;
        spec.outputRate = sa::SampleRate{wanted};
        spec.quality = sa::dsp::ResamplerQuality::Best;

        auto resampler = sa::dsp::Resampler::create(spec);
        if (!resampler) {
            return fail(std::string{resampler.error().what()});
        }

        // process() stops on whichever of input and output runs out first, so
        // it loops; flush() then drains what the fed input still owes.
        sa::SampleCount consumed = 0;
        sa::SampleCount produced = 0;
        while (consumed < info.frameCount && produced < outputFrames) {
            const auto step = resampler.value().process(
                input.channel(channel) + consumed, info.frameCount - consumed,
                output.channel(channel) + produced, outputFrames - produced);
            if (step.inputConsumed == 0 && step.outputProduced == 0) {
                break;
            }
            consumed += step.inputConsumed;
            produced += step.outputProduced;
        }
        while (produced < outputFrames) {
            const auto drained = resampler.value().flush(output.channel(channel) + produced,
                                                         outputFrames - produced);
            if (drained <= 0) {
                break;
            }
            produced += drained;
        }
    }

    const sa::engine::BufferSource converted{std::move(output), sa::SampleRate{wanted}};
    return write(converted, options.positional[2], format, error) ? 0 : fail(error);
}

int normalise(const Options& options) {
    if (options.positional.size() != 3) {
        return fail("normalise needs an input and an output");
    }
    const auto targetName = options.value("target");
    if (!targetName) {
        return fail("normalise needs --target");
    }

    const sa::analysis::ComplianceTarget* target = nullptr;
    for (std::size_t i = 0; i < sa::analysis::targetCount(); ++i) {
        const auto& candidate = sa::analysis::allTargets()[i];
        std::string name{candidate.name};
        std::string wanted = *targetName;
        // Through unsigned char: std::tolower takes an int whose value must be
        // representable as unsigned char, and a plain char is signed on most
        // platforms, so a byte above 127 is undefined behaviour the compiler
        // will not warn about.
        const auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
        std::transform(name.begin(), name.end(), name.begin(), lower);
        std::transform(wanted.begin(), wanted.end(), wanted.begin(), lower);
        if (name == wanted) {
            target = &candidate;
            break;
        }
    }
    if (target == nullptr) {
        return fail("unknown target '" + *targetName +
                    "' -- run sa-cli with no arguments for "
                    "the list");
    }

    std::string error;
    const auto source = open(options.positional[1], error);
    if (!source) {
        return fail(error);
    }

    Measurement measurement;
    if (!measure(*source, measurement, error)) {
        return fail(error);
    }
    if (measurement.loudness.gatedBlockCount <= 0) {
        return fail("nothing in this file cleared the absolute gate, so there is no loudness to "
                    "normalise");
    }

    const auto check =
        sa::analysis::check(*target, measurement.loudness.integratedLufs, measurement.truePeakDbtp);
    const double gainDb = check.conformGainDb;

    sa::engine::Document document{source->info().sampleRate, source->info().layout};
    auto registered = document.addSource(source, options.positional[1]);
    if (!registered || !document.appendSource(registered.value(), 0)) {
        return fail("could not place the audio on a timeline");
    }
    if (const auto status = sa::engine::applyRangeGain(
            document, 0, document.duration(), static_cast<float>(std::pow(10.0, gainDb / 20.0)));
        !status) {
        return fail(std::string{status.error().what()});
    }

    const sa::engine::DocumentSource rendered{document};
    if (!write(rendered, options.positional[2], formatFrom(options, source->info().format),
               error)) {
        return fail(error);
    }

    std::printf("%s: %.2f LUFS -> applied %+.2f dB -> %s (target %.1f LUFS, ceiling %.1f dBTP)\n",
                std::filesystem::path{options.positional[1]}.filename().string().c_str(),
                measurement.loudness.integratedLufs, gainDb,
                std::filesystem::path{options.positional[2]}.filename().string().c_str(),
                target->integratedLufs, target->truePeakCeilingDbtp);
    return 0;
}

int denoise(const Options& options) {
    if (options.positional.size() != 3) {
        return fail("denoise needs an input and an output");
    }
    const auto span = options.value("noise");
    if (!span) {
        return fail("denoise needs --noise <from>-<to> in seconds, over a passage of noise alone");
    }
    const auto dash = span->find('-', 1);
    if (dash == std::string::npos) {
        return fail("--noise wants <from>-<to>, for example 0-1.5");
    }

    std::string error;
    const auto source = open(options.positional[1], error);
    if (!source) {
        return fail(error);
    }
    const sa::io::AudioFileInfo& info = source->info();

    const double from = std::stod(span->substr(0, dash));
    const double to = std::stod(span->substr(dash + 1));
    const auto noiseStart = static_cast<sa::SampleIndex>(from * info.sampleRate.hz());
    const auto noiseEnd = static_cast<sa::SampleIndex>(to * info.sampleRate.hz());

    sa::AudioBuffer audio{info.layout, info.frameCount};
    if (const auto read = source->read(0, audio.view()); !read) {
        return fail(std::string{read.error().what()});
    }

    auto profile =
        sa::spectral::NoiseProfile::learn(audio.constView(), info.sampleRate, noiseStart, noiseEnd);
    if (!profile) {
        return fail(std::string{profile.error().what()});
    }

    sa::spectral::DenoiseSettings settings;
    settings.reductionDb = options.number("amount", 12.0);
    if (const auto status = sa::spectral::denoise(audio.view(), info.sampleRate, profile.value(), 0,
                                                  audio.frames(), settings);
        !status) {
        return fail(std::string{status.error().what()});
    }

    const sa::engine::BufferSource cleaned{std::move(audio), info.sampleRate};
    if (!write(cleaned, options.positional[2], formatFrom(options, info.format), error)) {
        return fail(error);
    }
    std::printf("%s: learned from %.2f-%.2f s, reduced by %.1f dB -> %s\n",
                std::filesystem::path{options.positional[1]}.filename().string().c_str(), from, to,
                settings.reductionDb,
                std::filesystem::path{options.positional[2]}.filename().string().c_str());
    return 0;
}

/// Shared by both halves of the machine below: read the whole file in, hand the
/// buffer to a transform, write what comes back.
int reshape(const Options& options,
            const std::function<sa::Result<sa::AudioBuffer>(const sa::AudioBuffer&)>& transform,
            const std::string& description) {
    std::string error;
    const auto source = open(options.positional[1], error);
    if (!source) {
        return fail(error);
    }
    const sa::io::AudioFileInfo& info = source->info();

    sa::AudioBuffer audio{info.layout, info.frameCount};
    if (const auto read = source->read(0, audio.view()); !read) {
        return fail(std::string{read.error().what()});
    }

    auto reshaped = transform(audio);
    if (!reshaped) {
        return fail(std::string{reshaped.error().what()});
    }

    const double seconds = sa::samplesToSeconds(reshaped.value().frames(), info.sampleRate);
    const sa::engine::BufferSource result{std::move(reshaped.value()), info.sampleRate};
    if (!write(result, options.positional[2], formatFrom(options, info.format), error)) {
        return fail(error);
    }
    std::printf("%s: %s -> %s (%.2f s)\n",
                std::filesystem::path{options.positional[1]}.filename().string().c_str(),
                description.c_str(),
                std::filesystem::path{options.positional[2]}.filename().string().c_str(), seconds);
    return 0;
}

int dehumFile(const Options& options) {
    if (options.positional.size() != 3) {
        return fail("dehum needs an input and an output");
    }

    sa::dsp::DehumSettings settings;
    settings.frequency = options.number("frequency", 0.0);
    settings.amount = options.number("amount", settings.amount);

    std::string error;
    const auto source = open(options.positional[1], error);
    if (!source) {
        return fail(error);
    }
    const sa::io::AudioFileInfo& info = source->info();

    sa::AudioBuffer audio{info.layout, info.frameCount};
    if (const auto read = source->read(0, audio.view()); !read) {
        return fail(std::string{read.error().what()});
    }

    const auto report = sa::dsp::dehum(audio.view(), info.sampleRate, settings);
    if (!report) {
        return fail(std::string{report.error().what()});
    }

    const sa::engine::BufferSource cleaned{std::move(audio), info.sampleRate};
    if (!write(cleaned, options.positional[2], formatFrom(options, info.format), error)) {
        return fail(error);
    }
    if (report.value().found) {
        std::printf("%s: %.2f Hz hum, %d partial%s removed, %.2f dB taken out -> %s\n",
                    std::filesystem::path{options.positional[1]}.filename().string().c_str(),
                    report.value().frequency, report.value().harmonics,
                    report.value().harmonics == 1 ? "" : "s", -report.value().removedDb,
                    std::filesystem::path{options.positional[2]}.filename().string().c_str());
    } else {
        std::printf("%s: no mains hum found -> %s\n",
                    std::filesystem::path{options.positional[1]}.filename().string().c_str(),
                    std::filesystem::path{options.positional[2]}.filename().string().c_str());
    }
    return 0;
}

int declipFile(const Options& options) {
    if (options.positional.size() != 3) {
        return fail("declip needs an input and an output");
    }

    sa::dsp::DeclipSettings settings;
    settings.fitToCeiling = !options.has("keep-level");

    std::string error;
    const auto source = open(options.positional[1], error);
    if (!source) {
        return fail(error);
    }
    const sa::io::AudioFileInfo& info = source->info();

    sa::AudioBuffer audio{info.layout, info.frameCount};
    if (const auto read = source->read(0, audio.view()); !read) {
        return fail(std::string{read.error().what()});
    }

    const auto report = sa::dsp::declip(audio.view(), settings);
    if (!report) {
        return fail(std::string{report.error().what()});
    }

    const sa::engine::BufferSource restored{std::move(audio), info.sampleRate};
    if (!write(restored, options.positional[2], formatFrom(options, info.format), error)) {
        return fail(error);
    }
    std::printf("%s: %d clipped peak%s restored, %lld samples, %.2f dB applied to fit, "
                "%d left as too long -> %s\n",
                std::filesystem::path{options.positional[1]}.filename().string().c_str(),
                report.value().runs, report.value().runs == 1 ? "" : "s",
                static_cast<long long>(report.value().samplesRestored), report.value().gainDb,
                report.value().tooLong,
                std::filesystem::path{options.positional[2]}.filename().string().c_str());
    return 0;
}

int declickFile(const Options& options) {
    if (options.positional.size() != 3) {
        return fail("declick needs an input and an output");
    }

    sa::dsp::DeclickSettings settings;
    settings.threshold = options.number("sensitivity", settings.threshold);

    std::string error;
    const auto source = open(options.positional[1], error);
    if (!source) {
        return fail(error);
    }
    const sa::io::AudioFileInfo& info = source->info();

    sa::AudioBuffer audio{info.layout, info.frameCount};
    if (const auto read = source->read(0, audio.view()); !read) {
        return fail(std::string{read.error().what()});
    }

    const auto report = sa::dsp::declick(audio.view(), settings);
    if (!report) {
        return fail(std::string{report.error().what()});
    }

    const sa::engine::BufferSource repaired{std::move(audio), info.sampleRate};
    if (!write(repaired, options.positional[2], formatFrom(options, info.format), error)) {
        return fail(error);
    }
    std::printf("%s: %d click%s repaired, %lld samples, %d left as too long -> %s\n",
                std::filesystem::path{options.positional[1]}.filename().string().c_str(),
                report.value().clicks, report.value().clicks == 1 ? "" : "s",
                static_cast<long long>(report.value().samplesRepaired), report.value().tooLong,
                std::filesystem::path{options.positional[2]}.filename().string().c_str());
    return 0;
}

int stretch(const Options& options) {
    if (options.positional.size() != 3) {
        return fail("stretch needs an input and an output");
    }
    if (!options.value("length")) {
        return fail("stretch needs --length <percent>, where 200 is twice as long");
    }
    const double percent = options.number("length", 100.0);
    const double factor = percent / 100.0;
    if (!(factor >= sa::dsp::stretch::kMinimumFactor &&
          factor <= sa::dsp::stretch::kMaximumFactor)) {
        return fail("--length is outside 10 to 1000 percent");
    }

    char described[64];
    std::snprintf(described, sizeof described, "stretched to %.2f%%", percent);
    return reshape(
        options,
        [factor](const sa::AudioBuffer& audio) {
            sa::dsp::StretchSettings settings;
            settings.factor = factor;
            return sa::dsp::timeStretch(audio, settings);
        },
        described);
}

int pitch(const Options& options) {
    if (options.positional.size() != 3) {
        return fail("pitch needs an input and an output");
    }
    if (!options.value("semitones")) {
        return fail("pitch needs --semitones <n>; fractions are allowed, and 0.01 is a cent");
    }
    const double semitones = options.number("semitones", 0.0);
    if (!(semitones >= sa::dsp::stretch::kMinimumSemitones &&
          semitones <= sa::dsp::stretch::kMaximumSemitones)) {
        return fail("--semitones is outside three octaves either way");
    }

    char described[64];
    std::snprintf(described, sizeof described, "shifted by %+.2f semitones (x%.5f)", semitones,
                  sa::dsp::pitchRatio(semitones));
    return reshape(
        options,
        [semitones](const sa::AudioBuffer& audio) {
            sa::dsp::PitchSettings settings;
            settings.semitones = semitones;
            return sa::dsp::pitchShift(audio, settings);
        },
        described);
}

int render(const Options& options) {
    if (options.positional.size() != 3) {
        return fail("render needs a session and an output");
    }
    sa::engine::FileSourceResolver resolver;
    auto loaded = sa::engine::loadSession(options.positional[1], resolver);
    if (!loaded) {
        return fail(std::string{loaded.error().what()});
    }
    for (const auto& missing : loaded.value().missingSources) {
        std::fprintf(stderr, "sa-cli: missing source %s (%s) -- its clips will be silent\n",
                     missing.path.string().c_str(), missing.reason.c_str());
    }

    const sa::engine::DocumentSource rendered{loaded.value().document};
    std::string error;
    if (!write(rendered, options.positional[2], formatFrom(options, sa::io::SampleFormat::PcmInt24),
               error)) {
        return fail(error);
    }
    std::printf("%s -> %s (%.2f s)\n",
                std::filesystem::path{options.positional[1]}.filename().string().c_str(),
                std::filesystem::path{options.positional[2]}.filename().string().c_str(),
                sa::samplesToSeconds(loaded.value().document.duration(),
                                     loaded.value().document.sampleRate()));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const Options options = parse(argc, argv);
    if (options.positional.empty() || options.has("help")) {
        usage();
        return options.positional.empty() && !options.has("help") ? 1 : 0;
    }

    const std::string& command = options.positional.front();
    if (command == "analyse" || command == "analyze") {
        return analyse(options);
    }
    if (command == "convert") {
        return convert(options);
    }
    if (command == "normalise" || command == "normalize") {
        return normalise(options);
    }
    if (command == "denoise") {
        return denoise(options);
    }
    if (command == "dehum") {
        return dehumFile(options);
    }
    if (command == "declip") {
        return declipFile(options);
    }
    if (command == "declick") {
        return declickFile(options);
    }
    if (command == "stretch") {
        return stretch(options);
    }
    if (command == "pitch") {
        return pitch(options);
    }
    if (command == "render") {
        return render(options);
    }
    usage();
    return fail("unknown command '" + command + "'");
}
