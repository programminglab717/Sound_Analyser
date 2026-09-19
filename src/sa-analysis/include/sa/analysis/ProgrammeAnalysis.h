#pragma once

#include <sa/analysis/LoudnessMeter.h>
#include <sa/analysis/SignalStatistics.h>
#include <sa/analysis/StereoField.h>
#include <sa/analysis/TruePeakMeter.h>
#include <sa/core/AudioBuffer.h>
#include <sa/core/ChannelLayout.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>

namespace sa::analysis {

/// Every number the metering panel shows for one programme.
///
/// The meters are independent and each has its own streaming API; this
/// exists because the offline caller wants all of them, and because PLR is the
/// one figure that needs two of them at once.
struct ProgrammeAnalysis {
    LoudnessMeasurement loudness;
    double truePeakDbtp = kDecibelFloor;
    SignalStatistics statistics;
    /// Left invalid for anything that is not a stereo pair, which is the
    /// honest answer rather than a set of numbers that would look like one.
    StereoField stereo;
};

/// Measure a whole buffer with every meter in one pass.
[[nodiscard]] Result<ProgrammeAnalysis>
analyseProgramme(ConstAudioBufferView audio, SampleRate rate, const ChannelLayout& layout,
                 int oversampling = TruePeakMeter::kDefaultOversampling);

} // namespace sa::analysis
