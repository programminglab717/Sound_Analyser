#include <sa/ui/AnalysisReadout.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace sa::ui {

namespace {

/// printf into a std::string. Everything printed here is a short fixed-width
/// figure, so a stack buffer of this size cannot be reached.
template <typename... Args>
[[nodiscard]] std::string formatted(const char* pattern, Args... arguments) {
    char buffer[160];
    const int written = std::snprintf(buffer, sizeof buffer, pattern, arguments...);
    return written > 0 ? std::string{buffer, static_cast<std::size_t>(written)} : std::string{};
}

/// Join caveats with a semicolon. Several reasons to doubt one answer are
/// several sentences, not a list of flags, and reading them as one line is how
/// someone decides whether to believe the number above them.
void addCaveat(std::string& into, const std::string& note) {
    if (note.empty()) {
        return;
    }
    if (!into.empty()) {
        into += "; ";
    }
    into += note;
}

[[nodiscard]] std::string modeName(analysis::Mode mode) {
    return mode == analysis::Mode::Major ? "major" : "minor";
}

/// The median hertz of the voiced frames, the same one estimatePitch() would
/// return: the upper of the two middle readings when there is an even number,
/// so the answer is always a frequency that was measured.
[[nodiscard]] double medianVoiced(const std::vector<analysis::PitchPoint>& contour,
                                  std::size_t& voicedCount) {
    std::vector<double> voiced;
    voiced.reserve(contour.size());
    for (const analysis::PitchPoint& point : contour) {
        if (point.voiced) {
            voiced.push_back(point.hz);
        }
    }
    voicedCount = voiced.size();
    if (voiced.empty()) {
        return 0.0;
    }
    std::sort(voiced.begin(), voiced.end());
    return voiced[voiced.size() / 2];
}

/// m:ss, for spans that are long enough to be worth reading in minutes.
[[nodiscard]] std::string minutesAndSeconds(double seconds) {
    const auto whole = static_cast<long long>(seconds);
    return formatted("%lld:%02lld", whole / 60, whole % 60);
}

} // namespace

Reading keyReading(const analysis::KeyEstimate& estimate, const std::string& error) {
    if (!error.empty()) {
        return Reading{"--", error, Certainty::None};
    }
    if (!estimate.valid) {
        return Reading{"--", "no key analysis has run", Certainty::None};
    }

    // The flat-chroma refusal comes first and on its own. A piece that uses all
    // twelve notes evenly has no key to name, and naming the best-fitting one
    // anyway -- with a warning -- would still put a key on screen for someone
    // to read off in a hurry.
    if (estimate.contrast < kKeylessContrast) {
        return Reading{"no key",
                       formatted("the twelve notes are used near-evenly (contrast %.2f); there "
                                 "may be no key here to find",
                                 estimate.contrast),
                       Certainty::None};
    }
    if (estimate.strength < kKeyRefusedBelow) {
        return Reading{"no key",
                       formatted("no key profile fits this well enough to name one (strength %.2f)",
                                 estimate.strength),
                       Certainty::None};
    }

    std::string caveat;
    if (estimate.strength < kKeyFirmAt) {
        addCaveat(caveat, formatted("a weak fit (strength %.2f)", estimate.strength));
    }
    if (std::abs(estimate.tuningOffsetCents) > kTuningFarCents) {
        // Stated as the thing that went wrong with the recording rather than
        // with the analysis, because that is what it is: every pitch class is
        // smeared into its neighbour before the profiles ever see it.
        addCaveat(caveat, formatted("%+.0f cents from A = 440, which smears the pitch classes "
                                    "and costs the answer above",
                                    estimate.tuningOffsetCents));
    }
    return Reading{analysis::keyName(estimate), caveat,
                   caveat.empty() ? Certainty::Firm : Certainty::Doubtful};
}

std::string runnerUpReading(const analysis::KeyEstimate& estimate) {
    if (!estimate.valid) {
        return "--";
    }
    const std::string_view name = analysis::pitchClassName(estimate.runnerUpTonic);
    return formatted("%.*s %s, %.2f behind", static_cast<int>(name.size()), name.data(),
                     modeName(estimate.runnerUpMode).c_str(), estimate.margin);
}

std::string tuningReading(const analysis::KeyEstimate& estimate) {
    if (!estimate.valid) {
        return "--";
    }
    return formatted("%+.0f cents", estimate.tuningOffsetCents);
}

Reading tempoReading(const analysis::BeatGrid& grid, const std::string& error) {
    if (!error.empty()) {
        return Reading{"--", error, Certainty::None};
    }
    // Not a failure and not a gap in the display. A held chord, a field
    // recording or a spoken word file has no tempo, and TempoTrack.h says so in
    // as many words.
    if (!grid.valid) {
        return Reading{"no tempo", "nothing in this is rhythmic enough to have one",
                       Certainty::None};
    }
    if (grid.confidence < kTempoDoubtfulBelow) {
        return Reading{formatted("%.1f BPM", grid.bpm),
                       formatted("the onsets repeat only weakly at this period (confidence "
                                 "%.2f); judge the beat grid against the transients",
                                 grid.confidence),
                       Certainty::Doubtful};
    }
    return Reading{formatted("%.1f BPM", grid.bpm), {}, Certainty::Firm};
}

Reading pitchReading(const std::vector<analysis::PitchPoint>& contour, bool requested,
                     const std::string& error) {
    if (!error.empty()) {
        return Reading{"--", error, Certainty::None};
    }
    if (!requested) {
        return Reading{"--", "no contour has been tracked", Certainty::None};
    }
    std::size_t voiced = 0;
    const double median = medianVoiced(contour, voiced);
    if (voiced == 0) {
        return Reading{"none", "nothing periodic was found in this passage", Certainty::None};
    }

    const double fraction =
        contour.empty() ? 0.0 : static_cast<double>(voiced) / static_cast<double>(contour.size());
    std::string caveat;
    if (fraction < kThinVoicing) {
        addCaveat(caveat, formatted("only %.0f%% of the frames were voiced, so the median above "
                                    "is taken over very little",
                                    fraction * 100.0));
    }
    return Reading{formatted("%.1f Hz", median), caveat,
                   caveat.empty() ? Certainty::Firm : Certainty::Doubtful};
}

Reading roomReading(const analysis::RoomAcoustics& room, bool requested, const std::string& error) {
    if (!error.empty()) {
        return Reading{"--", error, Certainty::None};
    }
    if (!requested) {
        return Reading{"--", "nothing has been measured as an impulse response", Certainty::None};
    }
    if (!room.valid) {
        return Reading{"--", "not an impulse response, or too short to measure", Certainty::None};
    }

    std::string caveat;
    if (!room.hasT20) {
        addCaveat(caveat, formatted("the decay fell only %.0f dB before its own noise floor, "
                                    "which supports neither T20 nor T30",
                                    room.usableRangeDb));
    } else if (!room.hasT30) {
        addCaveat(caveat, formatted("the decay fell %.0f dB before its own noise floor, which "
                                    "supports T20 but not T30",
                                    room.usableRangeDb));
    }
    return Reading{formatted("%.1f dB", room.usableRangeDb), caveat,
                   caveat.empty() ? Certainty::Firm : Certainty::Doubtful};
}

std::string roomSeconds(bool measured, double seconds) {
    return measured ? formatted("%.3f s", seconds) : std::string{"--"};
}

std::string coverageNote(const MusicalAnalysis& result) {
    if (result.requestedFrames <= result.analysedFrames || result.analysedFrames <= 0) {
        return {};
    }
    // Said in minutes and seconds because that is how the bound reads to
    // someone looking at a waveform, and said at all because a contour and a
    // beat grid that stop two minutes in otherwise look like a failure.
    return formatted(
        "first %s of %s",
        minutesAndSeconds(samplesToSeconds(result.analysedFrames, result.rate)).c_str(),
        minutesAndSeconds(samplesToSeconds(result.requestedFrames, result.rate)).c_str());
}

} // namespace sa::ui
