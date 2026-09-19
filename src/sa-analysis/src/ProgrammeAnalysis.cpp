#include <sa/analysis/ProgrammeAnalysis.h>

#include <utility>

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

    // Not an error when it is not a stereo pair: mono and 5.1 are ordinary
    // material, and refusing to measure anything else because one of four
    // meters does not apply would be the wrong shape entirely. The field
    // carries its own `valid`.
    if (auto stereo = StereoFieldMeter::measure(audio)) {
        result.stereo = std::move(stereo).value();
    }
    return result;
}

} // namespace sa::analysis
