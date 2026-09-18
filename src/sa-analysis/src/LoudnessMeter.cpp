#include <sa/analysis/LoudnessMeter.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace sa::analysis {

namespace {

/// Index of the bin holding `loudness`, clamped into range.
int binIndexFor(double loudness, double minimumLufs, double binWidth, int binCount) noexcept {
    const double offset = (loudness - minimumLufs) / binWidth;
    if (!(offset > 0.0)) {
        return 0;
    }
    const int index = static_cast<int>(offset);
    return std::min(index, binCount - 1);
}

} // namespace

// --- Histogram --------------------------------------------------------------

void LoudnessMeter::Histogram::allocate() {
    bins.assign(static_cast<std::size_t>(kHistogramBinCount), HistogramBin{});
}

void LoudnessMeter::Histogram::clear() noexcept {
    std::fill(bins.begin(), bins.end(), HistogramBin{});
    totalEnergy = 0.0;
    totalCount = 0;
}

void LoudnessMeter::Histogram::add(double loudness, double energy) noexcept {
    const int index =
        binIndexFor(loudness, kHistogramMinimumLufs, kHistogramBinWidthLu, kHistogramBinCount);
    HistogramBin& bin = bins[static_cast<std::size_t>(index)];
    ++bin.count;
    bin.energy += energy;
    totalEnergy += energy;
    ++totalCount;
}

double LoudnessMeter::Histogram::meanEnergy() const noexcept {
    return totalCount > 0 ? totalEnergy / static_cast<double>(totalCount) : 0.0;
}

double LoudnessMeter::Histogram::meanEnergyAbove(double threshold) const noexcept {
    double energy = 0.0;
    std::int64_t count = 0;
    for (int index = 0; index < kHistogramBinCount; ++index) {
        // A bin is in or out as a whole, judged by its centre. Centring the
        // test rather than using an edge keeps the quantisation error
        // unbiased instead of always excluding or always including.
        const double centre =
            kHistogramMinimumLufs + (static_cast<double>(index) + 0.5) * kHistogramBinWidthLu;
        if (centre <= threshold) {
            continue;
        }
        const HistogramBin& bin = bins[static_cast<std::size_t>(index)];
        energy += bin.energy;
        count += bin.count;
    }
    return count > 0 ? energy / static_cast<double>(count) : 0.0;
}

std::int64_t LoudnessMeter::Histogram::countAbove(double threshold) const noexcept {
    std::int64_t count = 0;
    for (int index = 0; index < kHistogramBinCount; ++index) {
        const double centre =
            kHistogramMinimumLufs + (static_cast<double>(index) + 0.5) * kHistogramBinWidthLu;
        if (centre > threshold) {
            count += bins[static_cast<std::size_t>(index)].count;
        }
    }
    return count;
}

double LoudnessMeter::Histogram::percentileAbove(double threshold, double fraction) const noexcept {
    const std::int64_t total = countAbove(threshold);
    if (total <= 0) {
        return kDecibelFloor;
    }

    // Nearest-rank: the value at position fraction through the sorted list.
    // The histogram is already sorted by construction, so walking it in bin
    // order is walking the sorted values.
    const auto rank =
        static_cast<std::int64_t>(static_cast<double>(total - 1) * fraction + 0.5);

    std::int64_t seen = 0;
    for (int index = 0; index < kHistogramBinCount; ++index) {
        const double centre =
            kHistogramMinimumLufs + (static_cast<double>(index) + 0.5) * kHistogramBinWidthLu;
        if (centre <= threshold) {
            continue;
        }
        seen += bins[static_cast<std::size_t>(index)].count;
        if (seen > rank) {
            return centre;
        }
    }
    return kDecibelFloor;
}

// --- LoudnessMeter ----------------------------------------------------------

double LoudnessMeter::weightFor(Speaker speaker) noexcept {
    switch (speaker) {
    case Speaker::Lfe:
        // Not attenuated -- excluded. The LFE channel carries a great deal of
        // energy that contributes nothing to perceived programme loudness, and
        // BS.1770-4 leaves it out of the sum entirely.
        return 0.0;
    case Speaker::LeftSurround:
    case Speaker::RightSurround:
    case Speaker::LeftSurroundRear:
    case Speaker::RightSurroundRear:
        return kSurroundWeight;
    case Speaker::Unknown:
    case Speaker::Mono:
    case Speaker::Left:
    case Speaker::Right:
    case Speaker::Centre:
    case Speaker::LeftHeight:
    case Speaker::RightHeight:
        // BS.1770-4 tabulates weights for L/R/C/Ls/Rs only and gives no rule
        // for height channels; 1.0 is the unlisted-channel default and is NOT
        // something this module has verified against the standard. Unknown
        // covers discrete layouts, where full weight is the only safe guess.
        return 1.0;
    }
    return 1.0;
}

LoudnessMeter::LoudnessMeter(SampleRate rate, const ChannelLayout& layout,
                             const KWeightingCoefficients& coefficients)
    : rate_(rate), channelCount_(layout.count()),
      samplesPerSubBlock_(secondsToSamples(kSubBlockSeconds, rate)), coefficients_(coefficients) {
    const auto channels = static_cast<std::size_t>(channelCount_);

    weights_.resize(channels);
    for (int channel = 0; channel < channelCount_; ++channel) {
        weights_[static_cast<std::size_t>(channel)] = weightFor(layout.at(channel));
    }

    shelfState_.resize(channels);
    highPassState_.resize(channels);
    subBlockSums_.assign(channels, 0.0);
    history_.assign(static_cast<std::size_t>(kSubBlocksPerShortTerm) * channels, 0.0);

    blocks_.allocate();
    shortTermBlocks_.allocate();
}

Result<LoudnessMeter> LoudnessMeter::create(SampleRate rate, const ChannelLayout& layout) {
    auto coefficients = kWeightingFor(rate);
    if (!coefficients) {
        return coefficients.error();
    }
    // No upper bound needed: ChannelLayout already clamps at kMaxChannels.
    if (layout.count() <= 0) {
        return Error{ErrorCode::InvalidArgument, "loudness needs at least one channel"};
    }

    bool measurable = false;
    for (int channel = 0; channel < layout.count(); ++channel) {
        measurable = measurable || weightFor(layout.at(channel)) > 0.0;
    }
    if (!measurable) {
        return Error{ErrorCode::InvalidArgument,
                     "layout has no channels that contribute to loudness (LFE is excluded)"};
    }

    // A sub-block must hold at least one sample or the block grid collapses;
    // kMinimumSampleRateHz already guarantees 800.
    return LoudnessMeter{rate, layout, std::move(coefficients).value()};
}

Result<LoudnessMeasurement> LoudnessMeter::measure(ConstAudioBufferView audio, SampleRate rate,
                                                   const ChannelLayout& layout) {
    auto meter = create(rate, layout);
    if (!meter) {
        return meter.error();
    }
    // process() silently ignores a mismatched block because it cannot report
    // from the audio thread. The offline entry point has no such excuse.
    if (audio.channelCount() != layout.count()) {
        return Error{ErrorCode::InvalidArgument, "buffer channel count does not match the layout"};
    }

    LoudnessMeter& instance = meter.value();
    instance.process(audio);
    return instance.measurement();
}

double LoudnessMeter::channelWeight(int channel) const noexcept {
    if (channel < 0 || channel >= channelCount_) {
        return 0.0;
    }
    return weights_[static_cast<std::size_t>(channel)];
}

double LoudnessMeter::loudnessFromEnergy(double energy) noexcept {
    if (!(energy > 0.0)) {
        return kDecibelFloor;
    }
    return std::max(kDecibelFloor, kLoudnessOffsetDb + 10.0 * std::log10(energy));
}

double LoudnessMeter::windowEnergy(int subBlockCount) const noexcept {
    const auto channels = static_cast<std::size_t>(channelCount_);
    double energy = 0.0;

    for (int channel = 0; channel < channelCount_; ++channel) {
        const double weight = weights_[static_cast<std::size_t>(channel)];
        if (weight == 0.0) {
            continue;
        }
        double sum = 0.0;
        for (int back = 0; back < subBlockCount; ++back) {
            const auto slot = static_cast<std::size_t>(
                (completedSubBlocks_ - 1 - back) % kSubBlocksPerShortTerm);
            sum += history_[slot * channels + static_cast<std::size_t>(channel)];
        }
        energy += weight * sum;
    }

    const double samples =
        static_cast<double>(samplesPerSubBlock_) * static_cast<double>(subBlockCount);
    return energy / samples;
}

void LoudnessMeter::completeSubBlock() noexcept {
    const auto channels = static_cast<std::size_t>(channelCount_);
    const auto slot = static_cast<std::size_t>(completedSubBlocks_ % kSubBlocksPerShortTerm);

    for (int channel = 0; channel < channelCount_; ++channel) {
        const auto index = static_cast<std::size_t>(channel);
        history_[slot * channels + index] = subBlockSums_[index];
        subBlockSums_[index] = 0.0;
    }
    ++completedSubBlocks_;
    subBlockFill_ = 0;

    if (completedSubBlocks_ >= kSubBlocksPerBlock) {
        const double energy = windowEnergy(kSubBlocksPerBlock);
        momentaryLufs_ = loudnessFromEnergy(energy);
        maximumMomentaryLufs_ = std::max(maximumMomentaryLufs_, momentaryLufs_);
        if (momentaryLufs_ > kAbsoluteGateLufs) {
            blocks_.add(momentaryLufs_, energy);
        }
    }

    if (completedSubBlocks_ >= kSubBlocksPerShortTerm) {
        const double energy = windowEnergy(kSubBlocksPerShortTerm);
        shortTermLufs_ = loudnessFromEnergy(energy);
        maximumShortTermLufs_ = std::max(maximumShortTermLufs_, shortTermLufs_);

        const std::int64_t sinceFirst = completedSubBlocks_ - kSubBlocksPerShortTerm;
        if (sinceFirst % kShortTermHopSubBlocks == 0 && shortTermLufs_ > kAbsoluteGateLufs) {
            shortTermBlocks_.add(shortTermLufs_, energy);
        }
    }
}

void LoudnessMeter::process(ConstAudioBufferView block) noexcept {
    if (block.channelCount() != channelCount_ || block.frames() <= 0) {
        return;
    }

    SampleCount done = 0;
    while (done < block.frames()) {
        const SampleCount take =
            std::min(samplesPerSubBlock_ - subBlockFill_, block.frames() - done);

        for (int channel = 0; channel < channelCount_; ++channel) {
            const auto index = static_cast<std::size_t>(channel);
            if (weights_[index] == 0.0) {
                // LFE contributes nothing, so it does not need filtering. Its
                // state stays reset, which keeps reset() honest.
                continue;
            }

            const float* samples = block.channel(channel) + done;
            // Running state is hoisted into locals for the inner loop but the
            // additions stay in sample order, so chopping the input into
            // different block sizes cannot change a single bit of the result.
            BiquadState shelf = shelfState_[index];
            BiquadState highPass = highPassState_[index];
            double sum = subBlockSums_[index];

            for (SampleCount i = 0; i < take; ++i) {
                const double shelved =
                    shelf.process(coefficients_.shelf, static_cast<double>(samples[i]));
                const double weighted = highPass.process(coefficients_.highPass, shelved);
                sum += weighted * weighted;
            }

            shelfState_[index] = shelf;
            highPassState_[index] = highPass;
            subBlockSums_[index] = sum;
        }

        subBlockFill_ += take;
        done += take;
        framesProcessed_ += take;

        if (subBlockFill_ >= samplesPerSubBlock_) {
            completeSubBlock();
        }
    }
}

void LoudnessMeter::reset() noexcept {
    for (int channel = 0; channel < channelCount_; ++channel) {
        const auto index = static_cast<std::size_t>(channel);
        shelfState_[index].reset();
        highPassState_[index].reset();
        subBlockSums_[index] = 0.0;
    }
    std::fill(history_.begin(), history_.end(), 0.0);

    subBlockFill_ = 0;
    completedSubBlocks_ = 0;
    framesProcessed_ = 0;
    momentaryLufs_ = kDecibelFloor;
    shortTermLufs_ = kDecibelFloor;
    maximumMomentaryLufs_ = kDecibelFloor;
    maximumShortTermLufs_ = kDecibelFloor;

    blocks_.clear();
    shortTermBlocks_.clear();
}

double LoudnessMeter::integratedLufs() const noexcept {
    if (blocks_.totalCount == 0) {
        return kDecibelFloor;
    }

    // Stage 1 uses exact totals rather than the histogram, so the relative
    // threshold itself carries no quantisation error -- only the membership
    // test in stage 2 does.
    const double ungated = loudnessFromEnergy(blocks_.meanEnergy());
    const double threshold = ungated + kRelativeGateLu;

    const double gated = blocks_.meanEnergyAbove(threshold);
    return gated > 0.0 ? loudnessFromEnergy(gated) : kDecibelFloor;
}

double LoudnessMeter::loudnessRangeLu() const noexcept {
    if (shortTermBlocks_.totalCount == 0) {
        return 0.0;
    }

    const double ungated = loudnessFromEnergy(shortTermBlocks_.meanEnergy());
    const double threshold = ungated + kRangeGateLu;
    if (shortTermBlocks_.countAbove(threshold) == 0) {
        return 0.0;
    }

    const double low = shortTermBlocks_.percentileAbove(threshold, kRangeLowPercentile);
    const double high = shortTermBlocks_.percentileAbove(threshold, kRangeHighPercentile);
    return std::max(0.0, high - low);
}

LoudnessMeasurement LoudnessMeter::measurement() const noexcept {
    LoudnessMeasurement result;
    result.momentaryLufs = momentaryLufs_;
    result.shortTermLufs = shortTermLufs_;
    result.integratedLufs = integratedLufs();
    result.maximumMomentaryLufs = maximumMomentaryLufs_;
    result.maximumShortTermLufs = maximumShortTermLufs_;
    result.loudnessRangeLu = loudnessRangeLu();
    result.framesProcessed = framesProcessed_;
    result.gatedBlockCount = blocks_.totalCount;
    result.shortTermBlockCount = shortTermBlocks_.totalCount;
    return result;
}

} // namespace sa::analysis
