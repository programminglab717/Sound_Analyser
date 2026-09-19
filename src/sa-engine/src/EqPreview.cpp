#include <sa/engine/EqPreview.h>

#include <cstddef>

namespace sa::engine {

Status EqPreview::prepare(SampleRate rate, int channelCount, SampleCount maxBlockFrames) {
    return stage_.prepare(rate, channelCount, maxBlockFrames);
}

Status EqPreview::setBands(std::span<const dsp::EqBand> bands) {
    if (!stage_.isPrepared()) {
        return Error{ErrorCode::InvalidArgument, "the preview has no sample rate until prepared"};
    }
    if (bands.size() > static_cast<std::size_t>(kMaxBands)) {
        return Error{ErrorCode::OutOfRange, "more bands than the equaliser holds"};
    }

    // Designed through ParametricEq rather than band by band, so the preview
    // and the curve drawn over the analyser are the same arithmetic. A band
    // that cannot be realised stops this here, with the previously published
    // curve still playing -- a refusal must not leave the audio thread running
    // half a change.
    Result<dsp::ParametricEq> designer = dsp::ParametricEq::create(stage_.sampleRate());
    if (!designer) {
        return designer.error();
    }
    for (const dsp::EqBand& band : bands) {
        if (const Result<int> added = designer.value().addBand(band); !added) {
            return added.error();
        }
    }

    EqPreviewSettings settings;
    settings.sectionCount = static_cast<int>(bands.size());
    for (int i = 0; i < settings.sectionCount; ++i) {
        const dsp::BiquadCoefficients* coefficients =
            designer.value().cascade().sectionCoefficients(i);
        if (coefficients == nullptr) {
            return Error{ErrorCode::Unknown, "the designer lost a band it had accepted"};
        }
        if (!coefficients->isStable()) {
            return Error{ErrorCode::InvalidArgument, "a band designs to an unstable filter"};
        }
        settings.sections[static_cast<std::size_t>(i)] = *coefficients;
    }

    stage_.publish(settings);
    return {};
}

} // namespace sa::engine
