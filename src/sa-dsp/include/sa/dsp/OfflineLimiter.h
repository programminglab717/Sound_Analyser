#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/dsp/Dynamics.h>
#include <sa/dsp/StereoLink.h>

namespace sa::dsp {

/// True-peak limiting with an oversampled signal path, for offline work.
///
/// The streaming Limiter detects on an oversampled view but *applies* its gain
/// at the base rate, and sample-rate gain modulation puts energy above Nyquist.
/// Measured on dense bright material: asked for -1.0 dBTP, the reconstruction
/// came out at -0.77, so the ceiling it promises is approached rather than held.
/// For a gain stage inside a chain that is fine. For the last thing before a
/// file, which a lossy encoder will reconstruct between the samples, it is not.
///
/// This interpolates the audio, limits at the higher rate where the
/// inter-sample peaks are ordinary samples, and decimates back. The ceiling
/// then holds in the reconstruction and not only on the grid.
///
/// Not real-time by design: it allocates, processes whole buffers, and costs
/// roughly `oversampling` times the work plus two conversions. Nothing in the
/// audio thread should call it.
struct OfflineLimitSettings {
    LimiterSettings limiter;
    StereoLinkSettings link;

    /// How far up the signal path goes. 4 catches the great majority of
    /// inter-sample peaks; 8 costs twice as much for a fraction of a decibel
    /// more. Above 8 the conversion filters cost more than the peaks they
    /// catch.
    int oversampling = 4;

    /// Link the stereo pair. Ignored for anything but two channels.
    bool linkStereo = true;

    /// After limiting, measure the result exactly and trim the whole thing by
    /// whatever it is still over by.
    ///
    /// This is what turns "approaches the ceiling" into "holds it". Limiting at
    /// a higher rate controls the peaks there, but decimating back is a
    /// linear-phase filter and it rings: measured on dense bright material,
    /// asking for -1.0 dBTP left -0.84 at 4x and -0.94 at 8x. Neither is a
    /// ceiling. The trim is a static gain of a fraction of a decibel, so it
    /// costs a little loudness and distorts nothing, and it is applied only
    /// when the measurement says it is needed.
    ///
    /// Off makes this cheaper and turns the ceiling back into a target. That is
    /// the right trade inside a chain and the wrong one before a file.
    bool trimToCeiling = true;
};

/// Limit `audio` in place. The buffer is processed whole, and its length does
/// not change.
[[nodiscard]] Status limitOffline(AudioBufferView audio, SampleRate rate,
                                  const OfflineLimitSettings& settings = {});

} // namespace sa::dsp
