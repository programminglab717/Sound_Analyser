#include <sa/analysis/ProgrammeAnalysis.h>

namespace sa::analysis {

Result<ProgrammeAnalysis> analyseProgramme(ConstAudioBufferView audio, SampleRate rate,
                                           const ChannelLayout& layout, int oversampling) {
    auto loudness = LoudnessMeter::measure(audio, rate, layout);
    if (!loudness) {
        return loudness.error();
    }

    auto truePeak = TruePeakMeter::measureDbtp(audio, oversampling);
    if (!truePeak) {
        return truePeak.error();
    }

    auto statistics =
        SignalStatisticsMeter::measure(audio, truePeak.value(), loudness.value().integratedLufs);
    if (!statistics) {
        return statistics.error();
    }

    ProgrammeAnalysis result;
    result.loudness = std::move(loudness).value();
    result.truePeakDbtp = truePeak.value();
    result.statistics = std::move(statistics).value();
    return result;
}

} // namespace sa::analysis
