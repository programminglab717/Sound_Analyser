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
#include <sa/analysis/KeyDetect.h>
#include <sa/analysis/LoudnessContour.h>
#include <sa/analysis/LoudnessMeter.h>
#include <sa/analysis/NullTest.h>
#include <sa/analysis/OctaveBands.h>
#include <sa/analysis/PitchTrack.h>
#include <sa/analysis/Provenance.h>
#include <sa/analysis/RoomAcoustics.h>
#include <sa/analysis/SignalStatistics.h>
#include <sa/analysis/StereoField.h>
#include <sa/analysis/SweepMeasurement.h>
#include <sa/analysis/TempoTrack.h>
#include <sa/analysis/TruePeakMeter.h>
#include <sa/dsp/ChannelOps.h>
#include <sa/dsp/Declick.h>
#include <sa/dsp/Declip.h>
#include <sa/dsp/Deess.h>
#include <sa/dsp/Dehum.h>
#include <sa/dsp/Dither.h>
#include <sa/dsp/FilterBank.h>
#include <sa/dsp/OfflineDynamics.h>
#include <sa/dsp/Resampler.h>
#include <sa/dsp/TimeStretch.h>
#include <sa/engine/BufferSource.h>
#include <sa/engine/Consolidate.h>
#include <sa/engine/Document.h>
#include <sa/engine/DocumentSource.h>
#include <sa/engine/Edits.h>
#include <sa/engine/SessionFile.h>
#include <sa/engine/StreamingConvert.h>
#include <sa/io/AudioFile.h>
#include <sa/io/WavWriter.h>
#include <sa/spectral/Denoise.h>
#include <sa/spectral/Dereverb.h>

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
    std::printf(R"(auscult-cli -- headless driver for Auscult

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

  dereverb <in> <out> [--amount <dB>] [--decay <s>] [--floor <dB>]
        [--onset <s>] [--format 16|24|float]
      Take some of the room back out of a take made in too live a one.
      Estimates the late reverberant energy in each frequency bin from that
      bin's own recent history and subtracts it. Defaults are 10 dB of
      removal assuming a 0.4 s decay.

      Set --decay to roughly the room's reverberation time. Over-stating it is
      not a free way to remove more: a sustained note is indistinguishable
      from its own tail, so too long a setting starts eating the material and
      sounding like a gate.

      What it does not do: shorten the decay. It scales the tail down and
      leaves the slope alone, so T30 barely moves -- judge it on EDT, C50 and
      D50. A discrete echo is barely touched; this is for a diffuse tail, not
      a slapback.

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

  bands <file> [--octave] [--filters [--order <n>]] [--json | --csv]
      Energy in third-octave bands, or in octaves with --octave. The oldest
      way of describing a spectrum and still the one people talk in.

      By default the energy is integrated from the transform: exact for steady
      material, cheap, and a rectangular band with no skirts.

      --filters runs the audio through an actual Butterworth band-pass per
      band instead and takes the level of what comes out, which is closer to
      what a sound level meter does. --order sets the filter order, six poles
      by default.

      The two do not agree, and neither is wrong. A full-scale sine at a band
      centre reads the same through both, but on white noise the filters read
      about a decibel higher, because a real filter's skirts reach past its
      nominal edges and into its neighbours while a rectangular band does not.
      Which you want depends on whether you are describing a spectrum or
      measuring a level.

      The two also label bands differently, and both labels are right. The
      default prints the preferred numbers people say aloud -- 125, 250,
      16000 -- and --filters prints the exact centres those names stand for,
      125.89, 251.19, 15848.9. They are the same bands. Joining two of these
      reports on the centre column will not line up.

      Neither is IEC 61260. Nothing here claims to meet its tolerance masks:
      they are not in this repository and the response has never been checked
      against them.

  tempo <file> [--min <bpm>] [--max <bpm>] [--channel <n>] [--json]
      Find the tempo and where the beats fall. Reads the first two minutes.
      --json gives every beat time, which is what a grid is for.

      Reports one tempo and one phase for the whole passage: no tempo curve,
      no rubato, no metre or downbeat. Material with nothing rhythmic in it is
      reported as having no tempo rather than given a number, and that is not
      an error.

      Of 60, 120 and 240 BPM the weighting prefers 120 -- the ambiguity is
      real and something has to break it. Narrow --min and --max if you know
      roughly where the answer should be.

  pitch-of <file> [--min <hz>] [--max <hz>] [--threshold <t>] [--channel <n>]
        [--csv | --json]
      Track the fundamental over time, by YIN. Prints the median of the voiced
      frames and how much of the file was voiced at all; --csv gives the whole
      contour, one row per frame, with an empty cell where nothing periodic
      was found rather than a zero.

      Monophonic. Given two notes at once it reports one of them and which one
      is not defined. Widening --min costs time on every frame, because the
      lowest pitch sets how many samples each one has to read.

  null <reference> <other> [--no-align] [--no-gain-match] [--max-delay <n>]
        [--channel <n>] [--json]
      Subtract two recordings that should be the same and report what is left.
      Aligns them, matches their level, then prints how far the residual sits
      below the reference, where in time it is worst, and a per-octave
      breakdown.

      This is the forensic answer to "did that processing chain actually change
      anything". A residual at the float floor means it did not. The band table
      prints the reference level beside the residual, because a band where the
      reference is silent shows a large positive ratio that means the opposite
      of what it looks like.

  contour <file> [--interval <s>] [--csv | --json]
      Loudness over time, not just overall. An integrated figure says a master
      sits at -14 LUFS; the contour says whether it sits there throughout or
      whether one loud chorus is carrying the average.

      Momentary (400 ms) and short-term (3 s) loudness at each point, with
      true peak, PSR and crest factor beside them. --csv gives every point.

      A cell is empty rather than zero where a window has not filled: a
      momentary reading needs 400 ms behind it and a short-term one needs
      three seconds, and a figure taken over half a window is a different
      measurement rather than a smaller one.

      PSR and PLR are industry practice rather than standards, and
      implementations differ over the windows. These use three seconds for
      both PSR operands. Reproducible here; not comparable with a tool that
      chose otherwise.

  key <file> [--channel <n>] [--json]
      Work out what key the music is in. Folds the spectrum onto the twelve
      pitch classes and compares the result against each of the twenty-four
      keys. Reads the first minute.

      Reports a strength, which is low both when the music does not fit a key
      and when it uses all twelve notes evenly -- a correlation alone cannot
      tell those apart from a confident answer, so it is not what is printed.
      Also reports the tuning offset from A = 440: far from zero and the
      answer is worth less, because the fold assumes equal temperament.

  sweep <out> [--rate <hz>] [--start <hz>] [--end <hz>] [--seconds <s>]
        [--level <dBFS>] [--fade <s>] [--format 16|24|float]
      Write an exponential sine sweep to play into a room or through a
      loudspeaker. Defaults are 20 Hz to 20 kHz over five seconds at -6 dBFS,
      written as float so the excitation carries no quantisation noise of its
      own. Exponential rather than linear so that a loudspeaker's harmonic
      distortion separates out later rather than smearing through the answer.

  deconvolve <recording> <out> [--start <hz>] [--end <hz>] [--seconds <s>]
        [--level <dBFS>] [--fade <s>] [--keep <s>] [--channel <n>]
        [--format 16|24|float]
      Turn a recording of that sweep back into an impulse response. The sweep
      flags must match what `sweep` was given: the deconvolution is only valid
      against the sweep that was actually played. --keep bounds the length
      kept, which is worth setting a little above the reverberation. Prints
      how far the peak stands above the end of the window, which is the number
      that says whether the measurement was loud enough to trust.

  room <impulse.wav> [--bands | --thirds] [--json]
      Reverberation and clarity from an impulse response: EDT, T20, T30, C50,
      C80, D50 and centre time, per the definitions in ISO 3382.

      Not claimed to be ISO 3382 conformant -- the definitions are implemented
      from their arithmetic and no certified reference material has been run
      against them. What is checked is that on a synthetic decay whose rate is
      known exactly, they recover it.

      A figure the recording has no range for is reported as "--" rather than
      extrapolated. Measuring T30 needs the decay to fall 35 dB clear of the
      noise, and most impulse responses do not.

      --bands gives a reverberation time per octave, which is how a room is
      actually described: a single T30 says "reverberant" without saying what
      to do about it, and 2.0 s at 125 Hz against 0.5 s at 4 kHz says both.
      --thirds does the same in third-octaves. The impulse response is
      filtered per band for these, which is not the same operation as the
      energy integration behind the `bands` command.

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
/// Build the ditherer a conversion should use, if any.
///
/// Hands back an object rather than processing a buffer, because a streamed
/// conversion never has the whole buffer: noise shaping carries an error term
/// from one sample to the next and the same ditherer has to see every block in
/// order. Returns false only when the request itself was wrong.
[[nodiscard]] bool ditherFor(std::optional<sa::dsp::Ditherer>& out, sa::io::SampleFormat format,
                             int channelCount, const Options& options, std::string& error) {
    out.reset();
    const auto named = options.value("dither");
    if (!named || *named == "none") {
        return true;
    }
    sa::dsp::DitherSettings settings;
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

    auto ditherer = sa::dsp::Ditherer::create(settings, channelCount);
    if (!ditherer) {
        error = std::string{ditherer.error().what()};
        return false;
    }
    out.emplace(std::move(ditherer).value());
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

    // Two ways of asking the same question, and they are not the same
    // measurement. The default integrates the transform's bins over each
    // band; --filters runs the audio through an actual Butterworth band-pass
    // per band and takes the level of what comes out. The filters have real
    // skirts and real overlap, so their answer is the one a sound level meter
    // would give, and the transform's is the one an analyser would.
    std::vector<sa::analysis::Band> bandLevels;
    if (options.has("filters")) {
        sa::dsp::FilterBankSettings bankSettings;
        bankSettings.spacing = settings.width == sa::analysis::BandWidth::Octave
                                   ? sa::dsp::BandSpacing::Octave
                                   : sa::dsp::BandSpacing::ThirdOctave;
        bankSettings.order = static_cast<int>(options.number("order", 6.0));
        auto bank = sa::dsp::FilterBank::create(info.sampleRate, bankSettings);
        if (!bank) {
            return fail(std::string{bank.error().what()});
        }
        const auto levels = bank.value().measure(audio.constView(), 0);
        if (!levels) {
            return fail(std::string{levels.error().what()});
        }
        for (const sa::dsp::BandLevel& level : levels.value()) {
            sa::analysis::Band band;
            band.centreHz = level.band.centreHz;
            band.lowHz = level.band.lowHz;
            band.highHz = level.band.highHz;
            // The bank reports a plain RMS, so a full-scale sine inside a band
            // reads -3.01 dBFS. The transform path is sine-referenced, where
            // the same tone reads 0. Adding the 3.0103 dB puts the two on one
            // scale so they can be read against each other; without it every
            // band would look 3 dB quieter for a reason that is a convention
            // rather than a measurement.
            band.levelDb = level.levelDb + 3.0102999566398120;
            bandLevels.push_back(band);
        }
    } else {
        auto measuredBands = sa::analysis::measureBands(audio.view(), info.sampleRate, settings);
        if (!measuredBands) {
            return fail(std::string{measuredBands.error().what()});
        }
        bandLevels = std::move(measuredBands).value();
    }

    if (asCsv) {
        std::printf("centreHz,lowHz,highHz,levelDbfs\n");
        for (const sa::analysis::Band& band : bandLevels) {
            std::printf("%.1f,%.2f,%.2f,%.2f\n", band.centreHz, band.lowHz, band.highHz,
                        band.levelDb);
        }
        return 0;
    }
    if (asJson) {
        std::printf("{\n  \"file\": \"%s\",\n  \"bands\": [\n",
                    std::filesystem::path{options.positional[1]}.filename().string().c_str());
        for (std::size_t i = 0; i < bandLevels.size(); ++i) {
            const sa::analysis::Band& band = bandLevels[i];
            std::printf("    {\"centreHz\": %.1f, \"lowHz\": %.2f, \"highHz\": %.2f, "
                        "\"levelDbfs\": %.2f}%s\n",
                        band.centreHz, band.lowHz, band.highHz, band.levelDb,
                        i + 1 < bandLevels.size() ? "," : "");
        }
        std::printf("  ]\n}\n");
        return 0;
    }

    std::printf("%s\n", std::filesystem::path{options.positional[1]}.filename().string().c_str());
    // Loudest band first, so the bars have something to be relative to and a
    // quiet recording is not drawn as thirty-one empty rows.
    double loudest = sa::analysis::kDecibelFloor;
    for (const sa::analysis::Band& band : bandLevels) {
        loudest = std::max(loudest, band.levelDb);
    }
    for (const sa::analysis::Band& band : bandLevels) {
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

int room(const Options& options) {
    if (options.positional.size() != 2) {
        return fail("room needs one impulse response");
    }
    const bool asJson = options.has("json");

    std::string error;
    const auto source = open(options.positional[1], error);
    if (!source) {
        return fail(error);
    }
    const sa::io::AudioFileInfo& info = source->info();

    // An impulse response is short by nature; the whole thing is read.
    sa::AudioBuffer audio{info.layout, info.frameCount};
    if (const auto read = source->read(0, audio.view()); !read) {
        return fail(std::string{read.error().what()});
    }

    const auto name = std::filesystem::path{options.positional[1]}.filename().string();

    if (options.has("bands") || options.has("thirds")) {
        const auto width = options.has("thirds") ? sa::analysis::BandWidth::ThirdOctave
                                                 : sa::analysis::BandWidth::Octave;
        const auto banded =
            sa::analysis::measureRoomAcousticsByBand(audio.view(), info.sampleRate, 0, width);
        if (!banded) {
            return fail(std::string{banded.error().what()});
        }
        if (asJson) {
            std::printf("{\n  \"file\": \"%s\",\n  \"bands\": [\n", name.c_str());
            for (std::size_t i = 0; i < banded.value().size(); ++i) {
                const auto& band = banded.value()[i];
                std::printf("    {\"centreHz\": %.1f, ", band.centreHz);
                if (band.measures.hasT20) {
                    std::printf("\"t20Seconds\": %.3f, ", band.measures.t20Seconds);
                } else {
                    std::printf("\"t20Seconds\": null, ");
                }
                if (band.measures.hasT30) {
                    std::printf("\"t30Seconds\": %.3f, ", band.measures.t30Seconds);
                } else {
                    std::printf("\"t30Seconds\": null, ");
                }
                if (band.measures.hasEarlyDecay) {
                    std::printf("\"edtSeconds\": %.3f}%s\n", band.measures.earlyDecaySeconds,
                                i + 1 < banded.value().size() ? "," : "");
                } else {
                    std::printf("\"edtSeconds\": null}%s\n",
                                i + 1 < banded.value().size() ? "," : "");
                }
            }
            std::printf("  ]\n}\n");
            return 0;
        }

        std::printf("%s\n", name.c_str());
        std::printf("   band        EDT       T20       T30\n");
        for (const auto& band : banded.value()) {
            const auto field = [](bool have, double value) {
                static char buffer[16];
                if (!have) {
                    std::snprintf(buffer, sizeof buffer, "       --");
                } else {
                    std::snprintf(buffer, sizeof buffer, "%8.3f", value);
                }
                return std::string{buffer};
            };
            std::printf("  %7.1f Hz %s %s %s\n", band.centreHz,
                        field(band.measures.hasEarlyDecay, band.measures.earlyDecaySeconds).c_str(),
                        field(band.measures.hasT20, band.measures.t20Seconds).c_str(),
                        field(band.measures.hasT30, band.measures.t30Seconds).c_str());
        }
        return 0;
    }

    if (asJson) {
        std::printf("{\n  \"file\": \"%s\",\n  \"channels\": [\n", name.c_str());
    } else {
        std::printf("%s\n", name.c_str());
    }

    for (int channel = 0; channel < info.channelCount(); ++channel) {
        const auto measured =
            sa::analysis::measureRoomAcoustics(audio.view(), info.sampleRate, channel);
        if (!measured) {
            return fail(std::string{measured.error().what()});
        }
        const sa::analysis::RoomAcoustics& r = measured.value();

        if (asJson) {
            std::printf("    {\"channel\": %d, ", channel);
            if (!r.valid) {
                std::printf("\"valid\": false}%s\n", channel + 1 < info.channelCount() ? "," : "");
                continue;
            }
            std::printf("\"valid\": true, ");
            // Null rather than zero for a figure the decay had no range for:
            // a consumer averaging these must not be handed a zero that looks
            // like a very dead room.
            if (r.hasEarlyDecay) {
                std::printf("\"edtSeconds\": %.3f, ", r.earlyDecaySeconds);
            } else {
                std::printf("\"edtSeconds\": null, ");
            }
            if (r.hasT20) {
                std::printf("\"t20Seconds\": %.3f, ", r.t20Seconds);
            } else {
                std::printf("\"t20Seconds\": null, ");
            }
            if (r.hasT30) {
                std::printf("\"t30Seconds\": %.3f, ", r.t30Seconds);
            } else {
                std::printf("\"t30Seconds\": null, ");
            }
            std::printf("\"c50Db\": %.2f, \"c80Db\": %.2f, \"d50\": %.3f, "
                        "\"centreTimeSeconds\": %.4f, \"usableRangeDb\": %.1f}%s\n",
                        r.clarity50Db, r.clarity80Db, r.definition50, r.centreTimeSeconds,
                        r.usableRangeDb, channel + 1 < info.channelCount() ? "," : "");
            continue;
        }

        if (info.channelCount() > 1) {
            std::printf("  channel %d\n", channel);
        }
        if (!r.valid) {
            std::printf("    not an impulse response, or too short to measure\n");
            continue;
        }
        const auto seconds = [](bool have, double value) {
            static char buffer[32];
            if (!have) {
                std::snprintf(buffer, sizeof buffer, "      --");
            } else {
                std::snprintf(buffer, sizeof buffer, "%8.3f", value);
            }
            return buffer;
        };
        std::printf("    EDT        %s s\n", seconds(r.hasEarlyDecay, r.earlyDecaySeconds));
        std::printf("    T20        %s s\n", seconds(r.hasT20, r.t20Seconds));
        std::printf("    T30        %s s\n", seconds(r.hasT30, r.t30Seconds));
        std::printf("    C50        %8.2f dB\n", r.clarity50Db);
        std::printf("    C80        %8.2f dB\n", r.clarity80Db);
        std::printf("    D50        %8.3f\n", r.definition50);
        std::printf("    centre     %8.4f s\n", r.centreTimeSeconds);
        std::printf("    usable     %8.1f dB of decay\n", r.usableRangeDb);
    }

    if (asJson) {
        std::printf("  ]\n}\n");
    }
    return 0;
}

int tempo(const Options& options) {
    if (options.positional.size() != 2) {
        return fail("tempo needs one file");
    }
    std::string error;
    const auto source = open(options.positional[1], error);
    if (!source) {
        return fail(error);
    }
    const sa::io::AudioFileInfo& info = source->info();

    // Two minutes is more than enough to settle a tempo and bounds what a long
    // file costs. From the start, for the same reason `key` reads from the
    // start: choosing a "representative" stretch would be a guess presented as
    // an analysis.
    const auto wanted = static_cast<sa::SampleCount>(
        std::min<double>(static_cast<double>(info.frameCount), info.sampleRate.hz() * 120.0));
    sa::AudioBuffer audio{info.layout, wanted};
    if (const auto read = source->read(0, audio.view()); !read) {
        return fail(std::string{read.error().what()});
    }

    sa::analysis::TempoSettings settings;
    settings.minBpm = options.number("min", settings.minBpm);
    settings.maxBpm = options.number("max", settings.maxBpm);
    const auto channel = static_cast<int>(options.number("channel", 0.0));

    const auto grid = sa::analysis::trackTempo(audio.view(), info.sampleRate, settings, channel);
    if (!grid) {
        return fail(std::string{grid.error().what()});
    }
    const auto& found = grid.value();
    const auto name = std::filesystem::path{options.positional[1]}.filename().string();

    if (options.has("json")) {
        std::printf("{\n  \"file\": \"%s\",\n  \"valid\": %s,\n", name.c_str(),
                    found.valid ? "true" : "false");
        if (!found.valid) {
            std::printf("  \"bpm\": null,\n  \"confidence\": %.4f,\n  \"beats\": []\n}\n",
                        found.confidence);
            return 0;
        }
        std::printf("  \"bpm\": %.3f,\n  \"confidence\": %.4f,\n", found.bpm, found.confidence);
        std::printf("  \"firstBeatSeconds\": %.4f,\n  \"beats\": [", found.firstBeatSeconds);
        for (std::size_t i = 0; i < found.beatSeconds.size(); ++i) {
            std::printf("%s%.4f", i == 0 ? "" : ", ", found.beatSeconds[i]);
        }
        std::printf("]\n}\n");
        return 0;
    }

    std::printf("%s\n", name.c_str());
    if (!found.valid) {
        // Not a failure. A held chord, a field recording or a spoken word file
        // has no tempo, and saying so is the right answer rather than an error
        // or a number nobody should use.
        std::printf("    no tempo found -- nothing in this is rhythmic enough to have one\n");
        return 0;
    }
    std::printf("    tempo      %.2f BPM\n", found.bpm);
    std::printf("    confidence %.2f\n", found.confidence);
    std::printf("    first beat %.3f s\n", found.firstBeatSeconds);
    std::printf("    beats      %zu over %.1f s\n", found.beatSeconds.size(),
                static_cast<double>(wanted) / info.sampleRate.hz());
    std::printf("    --json gives every beat time.\n");
    return 0;
}

int pitch_of(const Options& options) {
    if (options.positional.size() != 2) {
        return fail("pitch-of needs one file");
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

    sa::analysis::PitchSettings settings;
    settings.minHz = options.number("min", settings.minHz);
    settings.maxHz = options.number("max", settings.maxHz);
    settings.threshold = options.number("threshold", settings.threshold);
    const auto channel = static_cast<int>(options.number("channel", 0.0));

    const auto contour = sa::analysis::trackPitch(audio.view(), info.sampleRate, settings, channel);
    if (!contour) {
        return fail(std::string{contour.error().what()});
    }
    const auto& points = contour.value();

    // The median of the voiced frames, which is what estimatePitch would give
    // and is the number worth printing above a contour.
    std::vector<double> voiced;
    for (const auto& point : points) {
        if (point.voiced) {
            voiced.push_back(point.hz);
        }
    }
    std::sort(voiced.begin(), voiced.end());
    const double median = voiced.empty() ? 0.0 : voiced[voiced.size() / 2];
    const double voicedFraction =
        points.empty() ? 0.0
                       : static_cast<double>(voiced.size()) / static_cast<double>(points.size());

    if (options.has("csv")) {
        std::printf("seconds,hz,confidence,voiced\n");
        for (const auto& point : points) {
            std::printf("%.4f,", point.timeSeconds);
            if (point.voiced) {
                std::printf("%.3f,", point.hz);
            } else {
                std::printf(",");
            }
            std::printf("%.4f,%d\n", point.confidence, point.voiced ? 1 : 0);
        }
        return 0;
    }

    const auto name = std::filesystem::path{options.positional[1]}.filename().string();
    if (options.has("json")) {
        std::printf("{\n  \"file\": \"%s\",\n", name.c_str());
        // Null rather than zero where nothing was voiced: zero hertz is not a
        // pitch that was measured, and a consumer summing a column of these
        // should not be handed one.
        if (voiced.empty()) {
            std::printf("  \"medianHz\": null,\n  \"lowestHz\": null,\n  \"highestHz\": null,\n");
        } else {
            std::printf("  \"medianHz\": %.3f,\n  \"lowestHz\": %.3f,\n  \"highestHz\": %.3f,\n",
                        median, voiced.front(), voiced.back());
        }
        std::printf("  \"voicedFraction\": %.4f,\n  \"frames\": %zu\n}\n", voicedFraction,
                    points.size());
        return 0;
    }

    std::printf("%s\n", name.c_str());
    if (voiced.empty()) {
        std::printf("    nothing periodic found between %.0f Hz and %.0f Hz\n", settings.minHz,
                    settings.maxHz);
        return 0;
    }
    std::printf("    median     %.2f Hz\n", median);
    std::printf("    range      %.2f Hz to %.2f Hz over the voiced frames\n", voiced.front(),
                voiced.back());
    std::printf("    voiced     %.0f%% of %zu frames\n", voicedFraction * 100.0, points.size());
    std::printf("    --csv gives the whole contour, one row per frame.\n");
    return 0;
}

int nulltest(const Options& options) {
    if (options.positional.size() != 3) {
        return fail("null needs two files to compare");
    }
    std::string error;
    const auto referenceFile = open(options.positional[1], error);
    if (!referenceFile) {
        return fail(error);
    }
    const auto otherFile = open(options.positional[2], error);
    if (!otherFile) {
        return fail(error);
    }
    const sa::io::AudioFileInfo& referenceInfo = referenceFile->info();
    const sa::io::AudioFileInfo& otherInfo = otherFile->info();
    if (std::abs(referenceInfo.sampleRate.hz() - otherInfo.sampleRate.hz()) > 0.5) {
        return fail("the two files are at different sample rates; convert one first");
    }

    sa::AudioBuffer reference{referenceInfo.layout, referenceInfo.frameCount};
    if (const auto read = referenceFile->read(0, reference.view()); !read) {
        return fail(std::string{read.error().what()});
    }
    sa::AudioBuffer other{otherInfo.layout, otherInfo.frameCount};
    if (const auto read = otherFile->read(0, other.view()); !read) {
        return fail(std::string{read.error().what()});
    }

    sa::analysis::NullSettings settings;
    settings.alignDelay = !options.has("no-align");
    settings.matchGain = !options.has("no-gain-match");
    settings.maxDelaySamples = static_cast<sa::SampleCount>(options.number("max-delay", 0.0));
    const auto channel = static_cast<int>(options.number("channel", 0.0));

    const auto result = sa::analysis::nullTest(reference.view(), other.view(),
                                               referenceInfo.sampleRate, settings, channel);
    if (!result) {
        return fail(std::string{result.error().what()});
    }
    const auto& found = result.value();

    const auto verdictName = [](sa::analysis::NullVerdict verdict) {
        switch (verdict) {
        case sa::analysis::NullVerdict::BitIdentical:
            return "bit-identical";
        case sa::analysis::NullVerdict::WithinFloatFloor:
            return "identical within the float floor";
        case sa::analysis::NullVerdict::Different:
            break;
        }
        return "different";
    };

    if (options.has("json")) {
        std::printf("{\n  \"verdict\": \"%s\",\n", verdictName(found.verdict));
        std::printf("  \"delaySamples\": %lld,\n", static_cast<long long>(found.delaySamples));
        std::printf("  \"gainDb\": %.4f,\n  \"polarityInverted\": %s,\n", found.gainDb,
                    found.polarityInverted ? "true" : "false");
        std::printf("  \"residualDb\": %.4f,\n  \"peakResidualDb\": %.4f,\n", found.residualDb,
                    found.peakResidualDb);
        std::printf("  \"worstTimeSeconds\": %.4f,\n  \"comparedFrames\": %lld,\n",
                    found.worstTimeSeconds, static_cast<long long>(found.comparedFrames));
        std::printf("  \"bands\": [\n");
        for (std::size_t i = 0; i < found.bands.size(); ++i) {
            const auto& band = found.bands[i];
            std::printf("    {\"centreHz\": %.1f, \"referenceDb\": %.2f, \"residualDb\": %.2f, "
                        "\"relativeDb\": %.2f}%s\n",
                        band.centreHz, band.referenceDb, band.residualDb, band.relativeDb,
                        i + 1 < found.bands.size() ? "," : "");
        }
        std::printf("  ]\n}\n");
        return 0;
    }

    std::printf("%s against %s\n",
                std::filesystem::path{options.positional[2]}.filename().string().c_str(),
                std::filesystem::path{options.positional[1]}.filename().string().c_str());
    std::printf("    verdict    %s\n", verdictName(found.verdict));
    std::printf("    delay      %lld samples\n", static_cast<long long>(found.delaySamples));
    std::printf("    level      %+.2f dB%s\n", found.gainDb,
                found.polarityInverted ? ", polarity inverted" : "");
    std::printf("    residual   %.2f dB below the reference\n", found.residualDb);
    if (found.verdict == sa::analysis::NullVerdict::Different) {
        std::printf("    worst at   %.3f s, peaking %.2f dB down\n", found.worstTimeSeconds,
                    found.peakResidualDb);
        // Reference and residual both, not just their ratio. A band where the
        // reference has nothing in it shows a huge positive ratio -- the
        // residual is louder than a silence -- which reads as the biggest
        // difference in the file when it is the opposite of one. Printing the
        // two levels makes that visible instead of alarming.
        std::printf("    by band:        reference    residual\n");
        for (const auto& band : found.bands) {
            std::printf("      %8.1f Hz   %7.1f dB  %7.1f dB", band.centreHz, band.referenceDb,
                        band.residualDb);
            if (band.referenceDb < -80.0) {
                std::printf("   nothing in the reference here\n");
            } else {
                std::printf("   %+6.1f dB\n", band.relativeDb);
            }
        }
    }
    return 0;
}

int contour(const Options& options) {
    if (options.positional.size() != 2) {
        return fail("contour needs one file");
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

    const double interval = options.number("interval", 0.1);
    const auto measured =
        sa::analysis::measureLoudnessContour(audio.view(), info.sampleRate, info.layout, interval);
    if (!measured) {
        return fail(std::string{measured.error().what()});
    }
    const auto& contour = measured.value();
    const auto name = std::filesystem::path{options.positional[1]}.filename().string();

    // An empty cell, not a number, where a window has not filled. A momentary
    // reading needs 400 ms behind it and a short-term one needs three seconds,
    // and a figure computed over half a window is not a smaller measurement --
    // it is a different one, and nothing downstream could tell.
    const auto cell = [](const std::optional<double>& value) {
        static char buffer[24];
        if (!value) {
            return std::string{};
        }
        std::snprintf(buffer, sizeof buffer, "%.2f", *value);
        return std::string{buffer};
    };

    if (options.has("csv")) {
        std::printf("seconds,momentaryLufs,shortTermLufs,truePeakDbtp,psrDb,crestDb\n");
        for (const auto& point : contour.points) {
            std::printf("%.4f,%s,%s,%s,%s,%s\n", point.timeSeconds,
                        cell(point.momentaryLufs).c_str(), cell(point.shortTermLufs).c_str(),
                        cell(point.truePeakDbtp).c_str(), cell(point.psrDb).c_str(),
                        cell(point.crestDb).c_str());
        }
        return 0;
    }

    if (options.has("json")) {
        std::printf("{\n  \"file\": \"%s\",\n  \"integratedLufs\": %.3f,\n", name.c_str(),
                    contour.integratedLufs);
        std::printf("  \"loudnessRangeLu\": %.2f,\n  \"truePeakDbtp\": %.3f,\n",
                    contour.loudnessRangeLu, contour.truePeakDbtp);
        const auto number = [](const std::optional<double>& value) {
            static char buffer[24];
            if (!value) {
                return std::string{"null"};
            }
            std::snprintf(buffer, sizeof buffer, "%.3f", *value);
            return std::string{buffer};
        };
        std::printf("  \"plrDb\": %s,\n  \"quietestShortTermLufs\": %s,\n",
                    number(contour.plrDb).c_str(), number(contour.quietestShortTermLufs).c_str());
        std::printf("  \"loudestShortTermLufs\": %s,\n  \"points\": %zu\n}\n",
                    number(contour.loudestShortTermLufs).c_str(), contour.points.size());
        return 0;
    }

    std::printf("%s\n", name.c_str());
    std::printf("    integrated   %.2f LUFS over %lld gated blocks\n", contour.integratedLufs,
                static_cast<long long>(contour.gatedBlockCount));
    std::printf("    range        %.2f LU\n", contour.loudnessRangeLu);
    std::printf("    true peak    %.2f dBTP\n", contour.truePeakDbtp);
    if (contour.plrDb) {
        std::printf("    PLR          %.2f dB above the integrated loudness\n", *contour.plrDb);
    }
    if (contour.quietestShortTermLufs && contour.loudestShortTermLufs) {
        std::printf("    short term   %.2f to %.2f LUFS\n", *contour.quietestShortTermLufs,
                    *contour.loudestShortTermLufs);
    }
    std::printf("    %zu points at %.3f s. --csv gives the whole contour.\n", contour.points.size(),
                contour.intervalSeconds);
    return 0;
}

int key(const Options& options) {
    if (options.positional.size() != 2) {
        return fail("key needs one file");
    }
    std::string error;
    const auto source = open(options.positional[1], error);
    if (!source) {
        return fail(error);
    }
    const sa::io::AudioFileInfo& info = source->info();

    // A minute is plenty to decide a key and bounds what a long file costs.
    // Taken from the start rather than the middle: an intro is part of the
    // piece, and picking a window by some rule about where the "real" music is
    // would be a guess dressed as an analysis.
    const auto wanted = static_cast<sa::SampleCount>(
        std::min<double>(static_cast<double>(info.frameCount), info.sampleRate.hz() * 60.0));
    sa::AudioBuffer audio{info.layout, wanted};
    if (const auto read = source->read(0, audio.view()); !read) {
        return fail(std::string{read.error().what()});
    }

    const auto channel = static_cast<int>(options.number("channel", 0.0));
    const auto estimate = sa::analysis::detectKey(audio.view(), info.sampleRate, {}, channel);
    if (!estimate) {
        return fail(std::string{estimate.error().what()});
    }
    const auto& found = estimate.value();
    const auto name = std::filesystem::path{options.positional[1]}.filename().string();

    if (options.has("json")) {
        std::printf("{\n  \"file\": \"%s\",\n", name.c_str());
        std::printf("  \"key\": \"%s\",\n", sa::analysis::keyName(found).c_str());
        std::printf("  \"tonic\": \"%.*s\",\n",
                    static_cast<int>(sa::analysis::pitchClassName(found.tonic).size()),
                    sa::analysis::pitchClassName(found.tonic).data());
        std::printf("  \"mode\": \"%s\",\n",
                    found.mode == sa::analysis::Mode::Major ? "major" : "minor");
        std::printf("  \"strength\": %.3f,\n  \"fit\": %.3f,\n  \"contrast\": %.3f,\n",
                    found.strength, found.fit, found.contrast);
        std::printf("  \"margin\": %.3f,\n  \"tuningOffsetCents\": %.1f,\n", found.margin,
                    found.tuningOffsetCents);
        std::printf("  \"runnerUp\": \"%.*s %s\",\n",
                    static_cast<int>(sa::analysis::pitchClassName(found.runnerUpTonic).size()),
                    sa::analysis::pitchClassName(found.runnerUpTonic).data(),
                    found.runnerUpMode == sa::analysis::Mode::Major ? "major" : "minor");
        std::printf("  \"chroma\": [");
        for (std::size_t i = 0; i < found.chroma.size(); ++i) {
            std::printf("%s%.4f", i == 0 ? "" : ", ", found.chroma[i]);
        }
        std::printf("]\n}\n");
        return 0;
    }

    std::printf("%s\n", name.c_str());
    std::printf("    key        %s\n", sa::analysis::keyName(found).c_str());
    std::printf("    strength   %.2f\n", found.strength);
    std::printf("    runner-up  %.*s %s, %.2f behind on fit\n",
                static_cast<int>(sa::analysis::pitchClassName(found.runnerUpTonic).size()),
                sa::analysis::pitchClassName(found.runnerUpTonic).data(),
                found.runnerUpMode == sa::analysis::Mode::Major ? "major" : "minor", found.margin);
    std::printf("    tuning     %+.0f cents from A = 440\n", found.tuningOffsetCents);
    if (found.contrast < 0.15) {
        std::printf("    note       the twelve notes are used near-evenly; there may be no key "
                    "here to find\n");
    }
    if (std::abs(found.tuningOffsetCents) > 25.0) {
        std::printf("    note       far from concert pitch; the answer above is worth less\n");
    }
    return 0;
}

/// The sweep settings that both halves of a measurement have to agree on.
///
/// The deconvolution is only valid against the exact sweep that was played, so
/// the two commands read the same flags into the same struct rather than each
/// having its own defaults to drift apart.
[[nodiscard]] sa::analysis::SweepSettings sweepSettingsFrom(const Options& options) {
    sa::analysis::SweepSettings settings;
    settings.startHz = options.number("start", settings.startHz);
    settings.endHz = options.number("end", settings.endHz);
    settings.seconds = options.number("seconds", settings.seconds);
    settings.fadeSeconds = options.number("fade", settings.fadeSeconds);
    const double level = options.number("level", -6.0);
    settings.amplitude = std::pow(10.0, level / 20.0);
    return settings;
}

int sweep(const Options& options) {
    if (options.positional.size() != 2) {
        return fail("sweep needs an output file");
    }
    const sa::SampleRate rate{options.number("rate", 48000.0)};
    const auto settings = sweepSettingsFrom(options);

    auto generated = sa::analysis::generateSweep(rate, settings);
    if (!generated) {
        return fail(std::string{generated.error().what()});
    }
    const sa::SampleCount frames = generated.value().frames();

    std::string error;
    // Float by default. The sweep is going to be played and recorded, and
    // quantising it on the way out puts a noise floor into the excitation for
    // no reason.
    const sa::engine::BufferSource source{std::move(generated.value()), rate};
    if (!write(source, options.positional[1], formatFrom(options, sa::io::SampleFormat::Float32),
               error)) {
        return fail(error);
    }

    std::printf("%.1f Hz to %.1f Hz over %.2f s at %.1f dBFS, %lld frames at %.0f Hz\n",
                settings.startHz, settings.endHz, static_cast<double>(frames) / rate.hz(),
                20.0 * std::log10(settings.amplitude), static_cast<long long>(frames), rate.hz());
    std::printf("Play this, record it, and pass the recording to `sa-cli deconvolve` with the "
                "same --start, --end and --seconds.\n");
    return 0;
}

int deconvolve(const Options& options) {
    if (options.positional.size() != 3) {
        return fail("deconvolve needs a recording and an output file");
    }
    std::string error;
    const auto source = open(options.positional[1], error);
    if (!source) {
        return fail(error);
    }
    const sa::io::AudioFileInfo& info = source->info();

    sa::AudioBuffer recorded{info.layout, info.frameCount};
    if (const auto read = source->read(0, recorded.view()); !read) {
        return fail(std::string{read.error().what()});
    }

    const auto settings = sweepSettingsFrom(options);
    const auto channel = static_cast<int>(options.number("channel", 0.0));
    const double keep = options.number("keep", 0.0);

    auto impulse =
        sa::analysis::deconvolveSweep(recorded.view(), info.sampleRate, settings, keep, channel);
    if (!impulse) {
        return fail(std::string{impulse.error().what()});
    }
    const sa::SampleCount frames = impulse.value().frames();

    // Report the impulse before writing it, because the useful number is the
    // one that says whether the measurement is worth keeping: how far the peak
    // stands above what is left at the end of the window.
    double peak = 0.0;
    for (sa::SampleCount i = 0; i < frames; ++i) {
        peak = std::max(peak, std::abs(static_cast<double>(impulse.value().channel(0)[i])));
    }
    double floorEnergy = 0.0;
    sa::SampleCount counted = 0;
    for (sa::SampleCount i = frames - std::min<sa::SampleCount>(frames, frames / 10); i < frames;
         ++i) {
        floorEnergy +=
            static_cast<double>(impulse.value().channel(0)[i]) * impulse.value().channel(0)[i];
        ++counted;
    }
    const double noiseFloor =
        counted > 0 ? std::sqrt(floorEnergy / static_cast<double>(counted)) : 0.0;

    const sa::engine::BufferSource result{std::move(impulse.value()), info.sampleRate};
    if (!write(result, options.positional[2], formatFrom(options, sa::io::SampleFormat::Float32),
               error)) {
        return fail(error);
    }

    std::printf("%lld frames (%.3f s) at %.0f Hz\n", static_cast<long long>(frames),
                static_cast<double>(frames) / info.sampleRate.hz(), info.sampleRate.hz());
    if (peak > 0.0 && noiseFloor > 0.0) {
        std::printf("peak %.1f dB above the last tenth of the window\n",
                    20.0 * std::log10(peak / noiseFloor));
    }
    std::printf("Harmonic distortion deconvolved to negative time and has been cut off.\n");
    std::printf("Measure it with `sa-cli room %s`.\n", options.positional[2].c_str());
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

/// A sink that dithers each block on its way to the file.
///
/// Dither belongs immediately before the bits are dropped, and the bits are
/// dropped by the writer, so this sits between the two. The ditherer is built
/// once and kept: noise shaping carries an error term from one sample to the
/// next, and a fresh ditherer per block would reset that at every boundary and
/// leave a periodic artefact at the block rate.
class WriterSink final : public sa::engine::AudioSink {
public:
    WriterSink(sa::io::WavWriter& writer, sa::ChannelLayout layout, sa::dsp::Ditherer* ditherer)
        : writer_{writer}, layout_{layout}, ditherer_{ditherer} {}

    [[nodiscard]] sa::Status write(sa::ConstAudioBufferView frames) override {
        if (frames.frames() <= 0) {
            return {};
        }
        if (ditherer_ == nullptr) {
            return writer_.write(frames)
                       ? sa::Status{}
                       : sa::Error{sa::ErrorCode::IoFailure, "the write failed partway through"};
        }
        // Dither needs somewhere it may write, and the converter's output is
        // not ours to modify.
        if (scratch_.frames() < frames.frames() ||
            scratch_.channelCount() != frames.channelCount()) {
            scratch_ = sa::AudioBuffer{layout_, frames.frames()};
        }
        sa::AudioBufferView block = scratch_.view().subRange(0, frames.frames());
        for (int channel = 0; channel < frames.channelCount(); ++channel) {
            std::copy_n(frames.channel(channel), frames.frames(), block.channel(channel));
        }
        ditherer_->process(block);
        return writer_.write(sa::ConstAudioBufferView{block})
                   ? sa::Status{}
                   : sa::Error{sa::ErrorCode::IoFailure, "the write failed partway through"};
    }

private:
    sa::io::WavWriter& writer_;
    sa::ChannelLayout layout_;
    sa::dsp::Ditherer* ditherer_ = nullptr;
    sa::AudioBuffer scratch_;
};

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
    const sa::SampleRate outputRate{wanted};

    // A ditherer, if one was asked for and the format has bits to drop. Built
    // here rather than inside the sink so that an unknown --dither name is
    // reported before anything is written.
    std::optional<sa::dsp::Ditherer> ditherer;
    if (!ditherFor(ditherer, format, info.channelCount(), options, error)) {
        return fail(error);
    }

    std::ofstream stream{options.positional[2], std::ios::binary};
    if (!stream) {
        return fail("could not write " + options.positional[2]);
    }
    sa::io::WavOptions wavOptions;
    wavOptions.format = format;
    auto writer = sa::io::WavWriter::create(stream, outputRate, info.layout, wavOptions);
    if (!writer) {
        return fail(std::string{writer.error().what()});
    }

    // Streamed, a block at a time, so a file's length costs disk rather than
    // memory. This used to pull the whole thing in and refuse outright past
    // two gigabytes of samples, which for a concert recording or an archive
    // transfer is a wall rather than an inconvenience.
    WriterSink sink{writer.value(), info.layout, ditherer ? &ditherer.value() : nullptr};
    sa::engine::ConversionSpec spec;
    spec.outputRate = outputRate;
    spec.quality = sa::dsp::ResamplerQuality::Best;

    const auto converted = sa::engine::convertStreaming(*source, sink, spec);
    if (!converted) {
        // Deliberately not finishing the writer: a WAV whose header was never
        // patched does not read as a complete file, which is what a failed
        // conversion should leave behind rather than a plausible short one.
        return fail(std::string{converted.error().what()});
    }
    if (!writer.value().finish()) {
        return fail("could not finish " + options.positional[2]);
    }

    if (std::abs(wanted - info.sampleRate.hz()) >= 0.5) {
        std::printf("%lld frames at %.0f Hz from %lld at %.0f\n",
                    static_cast<long long>(converted.value().framesWritten), outputRate.hz(),
                    static_cast<long long>(converted.value().framesRead), info.sampleRate.hz());
    }
    return 0;
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

int dereverbFile(const Options& options) {
    if (options.positional.size() != 3) {
        return fail("dereverb needs an input and an output");
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

    sa::spectral::DereverbSettings settings;
    settings.reductionDb = options.number("amount", settings.reductionDb);
    settings.decaySeconds = options.number("decay", settings.decaySeconds);
    settings.floorDb = options.number("floor", settings.floorDb);
    settings.lateOnsetSeconds = options.number("onset", settings.lateOnsetSeconds);

    if (const auto done = sa::spectral::reduceReverb(audio.view(), info.sampleRate, settings);
        !done) {
        return fail(std::string{done.error().what()});
    }

    const sa::engine::BufferSource result{std::move(audio), info.sampleRate};
    if (!write(result, options.positional[2], formatFrom(options, info.format), error)) {
        return fail(error);
    }

    // What it did, and what it did not. Saying the second part matters here
    // more than usual: this scales a tail down without changing how fast the
    // room decays, so a user who measures T30 before and after will find it
    // barely moved and conclude the tool did nothing.
    std::printf("removed up to %.1f dB of estimated late energy, assuming a %.2f s decay\n",
                settings.reductionDb, settings.decaySeconds);
    std::printf("This attenuates the tail; it does not shorten the room. Judge it on EDT, C50 "
                "and D50 rather than T30.\n");
    std::printf("A discrete echo is barely touched -- this is for a diffuse tail, not a "
                "slapback.\n");
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
    if (command == "tempo") {
        return tempo(options);
    }
    if (command == "pitch-of") {
        return pitch_of(options);
    }
    if (command == "null") {
        return nulltest(options);
    }
    if (command == "contour") {
        return contour(options);
    }
    if (command == "key") {
        return key(options);
    }
    if (command == "sweep") {
        return sweep(options);
    }
    if (command == "deconvolve") {
        return deconvolve(options);
    }
    if (command == "room") {
        return room(options);
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
    if (command == "dereverb") {
        return dereverbFile(options);
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
