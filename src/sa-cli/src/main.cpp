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
#include <sa/analysis/OctaveBands.h>
#include <sa/analysis/Provenance.h>
#include <sa/analysis/SignalStatistics.h>
#include <sa/analysis/StereoField.h>
#include <sa/analysis/TruePeakMeter.h>
#include <sa/dsp/ChannelOps.h>
#include <sa/dsp/Declick.h>
#include <sa/dsp/Declip.h>
#include <sa/dsp/Deess.h>
#include <sa/dsp/Dehum.h>
#include <sa/dsp/Dither.h>
#include <sa/dsp/OfflineDynamics.h>
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
    /// Invalid for anything that is not a stereo pair.
    sa::analysis::StereoField stereo;

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
    // Optional: only a stereo pair has a stereo field, and mono is ordinary
    // material rather than a failure to measure.
    auto stereo = sa::analysis::StereoFieldMeter::create(info.channelCount());

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
        if (stereo) {
            stereo.value().process(filled);
        }
        cursor += read.value();
    }

    out.loudness = loudness.value().measurement();
    out.truePeakDbtp = peaks.value().truePeakDbtp();
    if (stereo) {
        out.stereo = stereo.value().field();
    }

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
        std::printf("  \"dcOffset\": %.6f,\n", measurement.statistics.dcOffset);
        // Null rather than zeroes when there is no stereo pair, for the same
        // reason the ungated loudness is null: a consumer must not be handed a
        // number that looks like a measurement and is not one.
        if (measurement.stereo.valid) {
            std::printf("  \"stereoCorrelation\": %.4f,\n", measurement.stereo.correlation);
            std::printf("  \"stereoWidthDb\": %.3f,\n", measurement.stereo.widthDb);
            std::printf("  \"stereoBalanceDb\": %.3f,\n", measurement.stereo.balanceDb);
            std::printf("  \"monoLossDb\": %.3f\n", measurement.stereo.monoLossDb);
        } else {
            std::printf("  \"stereoCorrelation\": null,\n");
            std::printf("  \"stereoWidthDb\": null,\n");
            std::printf("  \"stereoBalanceDb\": null,\n");
            std::printf("  \"monoLossDb\": null\n");
        }
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
    if (measurement.stereo.valid) {
        std::printf("  correlation  %8.2f\n", measurement.stereo.correlation);
        std::printf("  width        %8.2f dB\n", measurement.stereo.widthDb);
        std::printf("  balance      %8.2f dB %s\n", std::abs(measurement.stereo.balanceDb),
                    std::abs(measurement.stereo.balanceDb) < 0.005
                        ? "(centred)"
                        : (measurement.stereo.balanceDb > 0.0 ? "right" : "left"));
        std::printf("  mono sum     %8.2f dB\n", measurement.stereo.monoLossDb);
    }
}

/// One row per file, for checking a folder of deliverables at once.
///
/// A spreadsheet is what someone actually does with twenty files, and neither
/// of the other two output modes is one: the text form is for reading and the
/// JSON form is one object per file rather than an array, which is fine for a
/// pipe and useless for a sort.
///
/// Empty cells rather than -200 or 0 where a measurement does not exist, for
/// the same reason the JSON writes null: a column that mixes real numbers with
/// sentinel ones is a column nobody can average.
void printCsvHeader() {
    std::printf("file,sampleRate,channels,seconds,format,integratedLufs,loudnessRangeLu,"
                "maxShortTermLufs,maxMomentaryLufs,truePeakDbtp,truePeakExact,samplePeakDbfs,"
                "rmsDbfs,crestFactorDb,dcOffset,stereoCorrelation,stereoWidthDb,stereoBalanceDb,"
                "monoLossDb\n");
}

void printCsvRow(const std::filesystem::path& path, const sa::io::AudioFileInfo& info,
                 const Measurement& measurement) {
    // Quoted and with any quote doubled, so a filename with a comma in it does
    // not silently become two columns.
    std::string name = path.filename().string();
    std::string escaped;
    escaped.reserve(name.size() + 2);
    for (const char c : name) {
        if (c == '"') {
            escaped += '"';
        }
        escaped += c;
    }
    std::printf("\"%s\",%d,%d,%.6f,%s,", escaped.c_str(), static_cast<int>(info.sampleRate.hz()),
                info.channelCount(), info.durationSeconds(),
                std::string{sa::io::toString(info.format)}.c_str());

    if (measurement.loudness.gatedBlockCount > 0) {
        std::printf("%.3f,%.3f,", measurement.loudness.integratedLufs,
                    measurement.loudness.loudnessRangeLu);
    } else {
        std::printf(",,");
    }
    std::printf("%.3f,%.3f,%.3f,%s,%.3f,%.3f,%.3f,%.6f,", measurement.loudness.maximumShortTermLufs,
                measurement.loudness.maximumMomentaryLufs, measurement.truePeakDbtp,
                measurement.truePeakIsExact ? "exact" : "estimated",
                measurement.statistics.samplePeakDbfs, measurement.statistics.rmsDbfs,
                measurement.statistics.crestFactorDb, measurement.statistics.dcOffset);

    if (measurement.stereo.valid) {
        std::printf("%.4f,%.3f,%.3f,%.3f\n", measurement.stereo.correlation,
                    measurement.stereo.widthDb, measurement.stereo.balanceDb,
                    measurement.stereo.monoLossDb);
    } else {
        std::printf(",,,\n");
    }
}

void usage() {
    std::printf(R"(sa-cli -- headless driver for Sound Analyser

  analyse <file>... [--json | --csv]
      Measure loudness, peaks and statistics. --csv writes one row per file
      with a header, for checking a folder of deliverables in a spreadsheet;
      a measurement that does not exist is an empty cell rather than a
      sentinel number.

  convert <in> <out> [--rate <hz>] [--format 16|24|float] [--dither none|tpdf|shaped]
      Convert sample rate and format. Resampling is the Kaiser-windowed-sinc
      converter: 141 dB stopband at best quality.

      --dither applies before the bits are dropped, and only where they are:
      asking for it on a float output writes the float file untouched. It
      defaults to none here, unlike the window, because a file passing through
      this tool is usually on its way somewhere else and dither belongs at the
      end of a chain rather than at every step of one.

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

  deess <in> <out> [--frequency <hz>] [--threshold <dB>] [--ratio <n>]
        [--max <dB>] [--format 16|24|float]
      Compress the sibilance band and leave the rest of the voice alone.
      Defaults are 5000 Hz, -30 dB, 6:1 and at most 12 dB off. Prints how
      much it took and how much of the file it acted on, which is what says
      whether the threshold is anywhere near right.

  declick <in> <out> [--sensitivity <n>] [--format 16|24|float]
      Find and repair clicks. Prints how many it found, and how many stretches
      of damage were too long to be clicks and were left alone.

  stretch <in> <out> --length <percent> [--format 16|24|float]
      Change how long it lasts without changing its pitch. 200 is twice as
      long, 50 is half.

  pitch <in> <out> --semitones <n> [--format 16|24|float]
      Change its pitch without changing how long it lasts. Fractions are
      allowed, and 0.01 of a semitone is a cent.

  bands <file> [--octave] [--json | --csv]
      Energy in third-octave bands, or in octaves with --octave. The oldest
      way of describing a spectrum and still the one people talk in.

      Integrated from the transform rather than from a filter bank, which is
      exact for steady material and cheap, and is not what IEC 61260
      specifies -- nothing here claims to meet its tolerance masks, and a
      certified measurement needs a bank this does not have.

  provenance <file>... [--json]
      What the audio says about where it came from, as opposed to what its
      header claims. Reports the frequency above which there is nothing and
      how sharply it stops -- a lossy encoder leaves an edge nothing acoustic
      produces -- and how many bits the file actually uses out of the depth it
      declares, which is exact rather than a guess.

      It does not say "this is an MP3". A brick wall at 16 kHz is also what a
      deliberate low-pass looks like, and what a 32 kHz source upsampled to 48
      looks like. What it says is that something with a very steep filter
      removed the top of the band, and leaves the conclusion to you.

  compress <in> <out> [--threshold <dB>] [--ratio <n>] [--attack <ms>]
           [--release <ms>] [--knee <dB>] [--makeup <dB>] [--no-link]
           [--format 16|24|float]
      Downward compression over the whole file. Defaults are -20 dB, 4:1,
      10 ms and 100 ms. A stereo pair shares one sidechain unless --no-link,
      so the image cannot move.

  gate <in> <out> [--threshold <dB>] [--depth <dB>] [--attack <ms>]
       [--hold <ms>] [--release <ms>] [--hysteresis <dB>] [--no-link]
       [--format 16|24|float]
      Noise gate with hysteresis and hold. Defaults are -40 dB open, 3 dB of
      hysteresis and 80 dB of depth. Depth is finite on purpose: a gate that
      mutes completely makes its own action more obvious than the noise it
      removed.

  channels <in> <out> --op reverse|invert|swap|mono [--format 16|24|float]
      The four edits that are pure arithmetic: play it backwards, flip its
      polarity, exchange left and right, or put the average of the two
      channels on both. swap and mono need a stereo file.

  render <session.sa> <out.wav> [--format 16|24|float]
      Render a saved arrangement to audio.

Every command returns 0 on success and 1 on failure, and writes nothing to
stdout on failure, so it composes in a script.
)");
}

/// Dither a buffer for the format it is about to be written as, if the user
/// asked for any and the format actually drops bits.
///
/// --dither defaults to none rather than to triangular, which is the opposite
/// of the window. The reason is that this tool's job is often to convert or
/// repair a file that is going on to something else, and adding noise at every
/// step of a chain is how a file ends up with four layers of it. The window
/// exports a delivery master; this writes an intermediate unless told
/// otherwise.
[[nodiscard]] bool ditherFor(sa::AudioBufferView audio, sa::io::SampleFormat format,
                             const Options& options, std::string& error) {
    const auto named = options.value("dither");
    if (!named) {
        return true;
    }
    sa::dsp::DitherSettings settings;
    if (*named == "none") {
        return true;
    }
    if (*named == "tpdf") {
        settings.type = sa::dsp::DitherType::Tpdf;
    } else if (*named == "shaped") {
        settings.type = sa::dsp::DitherType::TpdfNoiseShaped;
    } else {
        error = "unknown dither '" + *named + "'; try none, tpdf or shaped";
        return false;
    }

    settings.bits = format == sa::io::SampleFormat::PcmInt16
                        ? 16
                        : (format == sa::io::SampleFormat::PcmInt24 ? 24 : 0);
    if (settings.bits == 0) {
        // Not an error. Asking for dither on a float export is a reasonable
        // thing to type, and the right response is to write the float file
        // rather than to refuse it or to add noise to it.
        return true;
    }

    auto ditherer = sa::dsp::Ditherer::create(settings, audio.channelCount());
    if (!ditherer) {
        error = std::string{ditherer.error().what()};
        return false;
    }
    ditherer.value().process(audio);
    return true;
}

int analyse(const Options& options) {
    if (options.positional.size() < 2) {
        return fail("analyse needs at least one file");
    }
    const bool asJson = options.has("json");
    const bool asCsv = options.has("csv");
    if (asJson && asCsv) {
        return fail("--json and --csv are two different reports; pick one");
    }
    int failures = 0;

    if (asCsv) {
        // Before the first file, so a run that fails on every file still emits
        // a well-formed empty table rather than nothing.
        printCsvHeader();
    }

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
        if (asCsv) {
            printCsvRow(path, source->info(), measurement);
        } else {
            printMeasurement(path, source->info(), measurement, asJson);
        }
    }
    return failures == 0 ? 0 : 1;
}

int bands(const Options& options) {
    if (options.positional.size() != 2) {
        return fail("bands needs one file");
    }
    const bool asJson = options.has("json");
    const bool asCsv = options.has("csv");
    if (asJson && asCsv) {
        return fail("--json and --csv are two different reports; pick one");
    }

    std::string error;
    const auto source = open(options.positional[1], error);
    if (!source) {
        return fail(error);
    }
    const sa::io::AudioFileInfo& info = source->info();

    sa::analysis::OctaveBandSettings settings;
    settings.width = options.has("octave") ? sa::analysis::BandWidth::Octave
                                           : sa::analysis::BandWidth::ThirdOctave;

    // Bounded, like provenance: a band average is a property of the programme
    // and two minutes characterises it.
    constexpr sa::SampleCount kMostFrames = 48000 * 120;
    const sa::SampleCount take = std::min(info.frameCount, kMostFrames);
    sa::AudioBuffer audio{info.layout, take};
    if (const auto read = source->read(0, audio.view()); !read) {
        return fail(std::string{read.error().what()});
    }

    const auto measured = sa::analysis::measureBands(audio.view(), info.sampleRate, settings);
    if (!measured) {
        return fail(std::string{measured.error().what()});
    }

    if (asCsv) {
        std::printf("centreHz,lowHz,highHz,levelDbfs\n");
        for (const sa::analysis::Band& band : measured.value()) {
            std::printf("%.1f,%.2f,%.2f,%.2f\n", band.centreHz, band.lowHz, band.highHz,
                        band.levelDb);
        }
        return 0;
    }
    if (asJson) {
        std::printf("{\n  \"file\": \"%s\",\n  \"bands\": [\n",
                    std::filesystem::path{options.positional[1]}.filename().string().c_str());
        for (std::size_t i = 0; i < measured.value().size(); ++i) {
            const sa::analysis::Band& band = measured.value()[i];
            std::printf("    {\"centreHz\": %.1f, \"lowHz\": %.2f, \"highHz\": %.2f, "
                        "\"levelDbfs\": %.2f}%s\n",
                        band.centreHz, band.lowHz, band.highHz, band.levelDb,
                        i + 1 < measured.value().size() ? "," : "");
        }
        std::printf("  ]\n}\n");
        return 0;
    }

    std::printf("%s\n", std::filesystem::path{options.positional[1]}.filename().string().c_str());
    // Loudest band first, so the bars have something to be relative to and a
    // quiet recording is not drawn as thirty-one empty rows.
    double loudest = sa::analysis::kDecibelFloor;
    for (const sa::analysis::Band& band : measured.value()) {
        loudest = std::max(loudest, band.levelDb);
    }
    for (const sa::analysis::Band& band : measured.value()) {
        // Forty columns over sixty decibels, which is the range a band display
        // conventionally shows and enough to read a shape off.
        const double below = loudest - band.levelDb;
        const int columns =
            band.levelDb <= sa::analysis::kDecibelFloor
                ? 0
                : std::clamp(static_cast<int>(std::lround(40.0 * (1.0 - below / 60.0))), 0, 40);
        std::printf("  %7.1f Hz %7.1f dB ", band.centreHz, band.levelDb);
        for (int i = 0; i < columns; ++i) {
            std::printf("#");
        }
        std::printf("\n");
    }
    return 0;
}

int provenance(const Options& options) {
    if (options.positional.size() < 2) {
        return fail("provenance needs at least one file");
    }
    const bool asJson = options.has("json");
    int failures = 0;

    for (std::size_t index = 1; index < options.positional.size(); ++index) {
        const std::filesystem::path path = options.positional[index];
        std::string error;
        const auto source = open(path, error);
        if (!source) {
            ++failures;
            std::fprintf(stderr, "sa-cli: %s\n", error.c_str());
            continue;
        }
        const sa::io::AudioFileInfo& info = source->info();

        // A fair sample rather than the whole file: what is being looked for is
        // a property of the encode, which is the same everywhere in it. Two
        // minutes is plenty and bounds the memory for a feature-length file.
        constexpr sa::SampleCount kMostFrames = 48000 * 120;
        const sa::SampleCount take = std::min(info.frameCount, kMostFrames);
        sa::AudioBuffer audio{info.layout, take};
        if (const auto read = source->read(0, audio.view()); !read) {
            ++failures;
            std::fprintf(stderr, "sa-cli: %s\n", std::string{read.error().what()}.c_str());
            continue;
        }

        sa::analysis::ProvenanceSettings settings;
        settings.declaredBits = 8 * sa::io::bytesPerSample(info.format);
        if (info.format == sa::io::SampleFormat::Float32 ||
            info.format == sa::io::SampleFormat::Float64) {
            // A float file has no bit depth in the sense this measures, and
            // reporting 32 would invite the comparison that makes it look
            // padded.
            settings.declaredBits = 0;
        }

        const auto found = sa::analysis::examineProvenance(audio.view(), info.sampleRate, settings);
        if (!found) {
            ++failures;
            std::fprintf(stderr, "sa-cli: %s\n", std::string{found.error().what()}.c_str());
            continue;
        }
        const sa::analysis::Provenance& result = found.value();

        if (asJson) {
            std::printf("{\n");
            std::printf("  \"file\": \"%s\",\n", path.filename().string().c_str());
            std::printf("  \"sampleRate\": %d,\n", static_cast<int>(info.sampleRate.hz()));
            std::printf("  \"declaredBits\": %d,\n", result.declaredBits);
            if (result.declaredBits > 0) {
                std::printf("  \"effectiveBits\": %d,\n", result.effectiveBits);
                std::printf("  \"padded\": %s,\n", result.isPadded ? "true" : "false");
            } else {
                std::printf("  \"effectiveBits\": null,\n");
                std::printf("  \"padded\": null,\n");
            }
            if (result.hasSteepCutoff) {
                std::printf("  \"cutoffHz\": %.0f,\n", result.cutoffHz);
                std::printf("  \"cutoffDropDb\": %.1f,\n", result.cutoffDropDb);
            } else {
                std::printf("  \"cutoffHz\": null,\n");
                std::printf("  \"cutoffDropDb\": null,\n");
            }
            std::printf("  \"steepCutoff\": %s\n", result.hasSteepCutoff ? "true" : "false");
            std::printf("}\n");
            continue;
        }

        std::printf("%s\n", path.filename().string().c_str());
        if (!result.valid) {
            std::printf("  too short to say anything\n");
            continue;
        }
        if (result.declaredBits > 0) {
            if (result.effectiveBits == 0) {
                std::printf("  depth        %d-bit file, silent\n", result.declaredBits);
            } else if (result.isPadded) {
                std::printf("  depth        %d-bit file using %d bits -- padded\n",
                            result.declaredBits, result.effectiveBits);
            } else {
                std::printf("  depth        %d-bit, all of it used\n", result.declaredBits);
            }
        } else {
            std::printf("  depth        float, so not applicable\n");
        }
        if (result.hasSteepCutoff) {
            std::printf("  band stops   %.0f Hz, falling %.0f dB across a quarter octave\n",
                        result.cutoffHz, result.cutoffDropDb);
            std::printf("  reading      something with a very steep filter took the top off\n");
        } else {
            std::printf("  band         runs to the top; no encoder edge\n");
        }
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
        if (!options.value("dither")) {
            // Nothing to dither, so stream it straight through rather than
            // pulling the whole file into memory to change nothing.
            return write(*source, options.positional[2], format, error) ? 0 : fail(error);
        }
        sa::AudioBuffer same{info.layout, info.frameCount};
        if (const auto read = source->read(0, same.view()); !read) {
            return fail(std::string{read.error().what()});
        }
        if (!ditherFor(same.view(), format, options, error)) {
            return fail(error);
        }
        const sa::engine::BufferSource dithered{std::move(same), info.sampleRate};
        return write(dithered, options.positional[2], format, error) ? 0 : fail(error);
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

    if (!ditherFor(output.view(), format, options, error)) {
        return fail(error);
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

/// Compress or gate a whole file.
///
/// No run-up and no edge blend, unlike the window: the range is the file, so
/// there is nothing before it to settle on and nothing beside it to step
/// against. The processor starts cold, which is correct here -- the first
/// moments of a file are the first moments of the programme.
int dynamics(const Options& options, bool compressing) {
    const char* what = compressing ? "compress" : "gate";
    if (options.positional.size() != 3) {
        return fail(std::string{what} + " needs an input and an output");
    }

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

    sa::dsp::OfflineDynamicsSettings shared;
    shared.linkStereo = !options.has("no-link");

    sa::Status status;
    std::string summary;
    if (compressing) {
        sa::dsp::CompressorSettings settings;
        settings.thresholdDb = options.number("threshold", settings.thresholdDb);
        settings.ratio = options.number("ratio", settings.ratio);
        settings.attackSeconds = options.number("attack", settings.attackSeconds * 1000.0) / 1000.0;
        settings.releaseSeconds =
            options.number("release", settings.releaseSeconds * 1000.0) / 1000.0;
        settings.kneeDb = options.number("knee", settings.kneeDb);
        settings.makeupGainDb = options.number("makeup", settings.makeupGainDb);
        status = sa::dsp::compressOffline(audio.view(), info.sampleRate, settings, 0, 0, shared);
        summary = std::to_string(settings.ratio).substr(0, 4) + ":1 at " +
                  std::to_string(static_cast<int>(settings.thresholdDb)) + " dB";
    } else {
        sa::dsp::GateSettings settings;
        settings.thresholdDb = options.number("threshold", settings.thresholdDb);
        settings.rangeDb = -std::abs(options.number("depth", -settings.rangeDb));
        settings.hysteresisDb = options.number("hysteresis", settings.hysteresisDb);
        settings.attackSeconds = options.number("attack", settings.attackSeconds * 1000.0) / 1000.0;
        settings.holdSeconds = options.number("hold", settings.holdSeconds * 1000.0) / 1000.0;
        settings.releaseSeconds =
            options.number("release", settings.releaseSeconds * 1000.0) / 1000.0;
        status = sa::dsp::gateOffline(audio.view(), info.sampleRate, settings, 0, 0, shared);
        summary = std::to_string(static_cast<int>(settings.thresholdDb)) + " dB open, " +
                  std::to_string(static_cast<int>(-settings.rangeDb)) + " dB deep";
    }
    if (!status) {
        return fail(std::string{status.error().what()});
    }

    const sa::engine::BufferSource processed{std::move(audio), info.sampleRate};
    if (!write(processed, options.positional[2], formatFrom(options, info.format), error)) {
        return fail(error);
    }
    std::printf("%s: %s %s -> %s\n",
                std::filesystem::path{options.positional[1]}.filename().string().c_str(), what,
                summary.c_str(),
                std::filesystem::path{options.positional[2]}.filename().string().c_str());
    return 0;
}

int channels(const Options& options) {
    if (options.positional.size() != 3) {
        return fail("channels needs an input and an output");
    }
    const auto named = options.value("op");
    if (!named) {
        return fail("channels needs --op reverse, invert, swap or mono");
    }

    sa::dsp::ChannelOp operation{};
    if (*named == "reverse") {
        operation = sa::dsp::ChannelOp::Reverse;
    } else if (*named == "invert") {
        operation = sa::dsp::ChannelOp::InvertPolarity;
    } else if (*named == "swap") {
        operation = sa::dsp::ChannelOp::SwapChannels;
    } else if (*named == "mono") {
        operation = sa::dsp::ChannelOp::SumToMono;
    } else {
        return fail("unknown operation '" + *named + "'; try reverse, invert, swap or mono");
    }

    std::string error;
    const auto source = open(options.positional[1], error);
    if (!source) {
        return fail(error);
    }
    const sa::io::AudioFileInfo& info = source->info();

    // Refused before the file is read, so a mistyped command on a two-hour
    // mono recording costs nothing.
    if (sa::dsp::channelOpNeedsStereo(operation) && info.channelCount() != 2) {
        return fail(*named + " needs a stereo file; this one has " +
                    std::to_string(info.channelCount()) + " channel" +
                    (info.channelCount() == 1 ? "" : "s"));
    }

    sa::AudioBuffer audio{info.layout, info.frameCount};
    if (const auto read = source->read(0, audio.view()); !read) {
        return fail(std::string{read.error().what()});
    }

    if (const auto status = sa::dsp::applyChannelOp(audio.view(), operation); !status) {
        return fail(std::string{status.error().what()});
    }

    const sa::engine::BufferSource edited{std::move(audio), info.sampleRate};
    if (!write(edited, options.positional[2], formatFrom(options, info.format), error)) {
        return fail(error);
    }
    std::printf(
        "%s: %s -> %s\n", std::filesystem::path{options.positional[1]}.filename().string().c_str(),
        named->c_str(), std::filesystem::path{options.positional[2]}.filename().string().c_str());
    return 0;
}

int deessFile(const Options& options) {
    if (options.positional.size() != 3) {
        return fail("deess needs an input and an output");
    }

    sa::dsp::DeessSettings settings;
    settings.frequencyHz = options.number("frequency", settings.frequencyHz);
    settings.thresholdDb = options.number("threshold", settings.thresholdDb);
    settings.ratio = options.number("ratio", settings.ratio);
    settings.maximumReductionDb = options.number("max", settings.maximumReductionDb);

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

    // No run-up: the range is the file, so there is nothing before it to
    // settle on and the first moments of a file are the first moments of the
    // programme.
    const auto report = sa::dsp::deess(audio.view(), info.sampleRate, settings, 0);
    if (!report) {
        return fail(std::string{report.error().what()});
    }

    const sa::engine::BufferSource processed{std::move(audio), info.sampleRate};
    if (!write(processed, options.positional[2], formatFrom(options, info.format), error)) {
        return fail(error);
    }
    std::printf("%s: de-essed above %.0f Hz, up to %.1f dB off on %.0f%% of it -> %s\n",
                std::filesystem::path{options.positional[1]}.filename().string().c_str(),
                settings.frequencyHz, report.value().peakReductionDb,
                100.0 * report.value().fractionReduced,
                std::filesystem::path{options.positional[2]}.filename().string().c_str());
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
    if (command == "bands") {
        return bands(options);
    }
    if (command == "provenance") {
        return provenance(options);
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
    if (command == "deess") {
        return deessFile(options);
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
    if (command == "compress") {
        return dynamics(options, true);
    }
    if (command == "gate") {
        return dynamics(options, false);
    }
    if (command == "channels") {
        return channels(options);
    }
    if (command == "render") {
        return render(options);
    }
    usage();
    return fail("unknown command '" + command + "'");
}
