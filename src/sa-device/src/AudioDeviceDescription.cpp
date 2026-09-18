#include <sa/device/AudioDeviceDescription.h>

#include <cmath>
#include <limits>

namespace sa::device {

bool AudioDeviceDescription::supportsSampleRate(SampleRate rate) const noexcept {
    if (!rate.isValid()) {
        return false;
    }
    if (sampleRates.empty()) {
        return true;
    }
    for (const SampleRate candidate : sampleRates) {
        if (candidate == rate) {
            return true;
        }
    }
    return false;
}

SampleRate AudioDeviceDescription::closestSampleRate(SampleRate preferred) const noexcept {
    if (sampleRates.empty() || !preferred.isValid()) {
        return preferred;
    }

    SampleRate best = sampleRates.front();
    double bestDistance = std::numeric_limits<double>::max();
    for (const SampleRate candidate : sampleRates) {
        if (!candidate.isValid()) {
            continue;
        }
        // Octaves, not hertz: a request for 44100 should land on 48000 rather
        // than 96000, and in hertz those two are nearly equidistant.
        const double distance = std::abs(std::log2(candidate.hz() / preferred.hz()));
        if (distance < bestDistance) {
            bestDistance = distance;
            best = candidate;
        }
    }
    return best;
}

} // namespace sa::device
