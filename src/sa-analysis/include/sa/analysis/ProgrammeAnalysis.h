#pragma once

#include <sa/analysis/LoudnessMeter.h>
#include <sa/analysis/SignalStatistics.h>
#include <sa/analysis/TruePeakMeter.h>
#include <sa/core/AudioBuffer.h>
#include <sa/core/ChannelLayout.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>

namespace sa::analysis {

/// Every number the metering panel shows for one programme.
///
/// The three meters are independent and each has its own streaming API; this
/// exists because the offline caller wants all of them, and because PLR is the
/// one figure that needs two of them at once.
struct ProgrammeAnalysis {
    LoudnessMeasurement loudness;
    double truePeakDbtp = kDecibelFloor;
    SignalStatistics statistics;
};

/// Measure a whole buffer with all three meters in one pass.
[[nodiscard]] Result<ProgrammeAnalysis> analyseProgramme(
    ConstAudioBufferView audio, SampleRate rate, const ChannelLayout& layout,
    int oversampling = TruePeakMeter::kDefaultOversampling);

} // namespace sa::analysis
