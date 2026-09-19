#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>

#include <cstdint>
#include <vector>

/// Dither, for when float audio has to become a fixed number of bits.
///
/// Rounding to the nearest integer is the obvious thing to do and it is wrong.
/// The error it makes is a deterministic function of the signal, so it is
/// correlated with the signal, and correlated error is distortion: on a fading
/// tone the quantisation products track the tone down and turn a clean decay
/// into a granular one, which is the characteristic sound of a badly made
/// 16-bit master. The error is also biased -- a constant that sits between two
/// codes is rounded to the same side every time, so its average comes out
/// wrong.
///
/// Adding a small amount of noise before rounding fixes both. With the right
/// noise the error becomes independent of the signal, which means it is heard
/// as a steady hiss rather than as something that moves with the music, and the
/// average of the output equals the input exactly however far below one code it
/// sits. That last property is the one to hold on to: a dithered signal can
/// carry information below its own least significant bit, and an undithered one
/// cannot.
///
/// The cost is about 4.8 dB of noise floor at 16 bits. It buys the difference
/// between a noise floor and a distortion floor, which is not a close call.
namespace sa::dsp {

enum class DitherType {
    /// Round and accept the distortion. Correct only when the destination is
    /// float, or when the audio is already at the destination's depth and
    /// bit-identical output is what is wanted.
    None,
    /// Triangular, two least-significant bits peak to peak. The standard
    /// choice, and the one that makes the error both independent of the signal
    /// and unbiased. Flat spectrum: the noise is spread evenly, so it is
    /// quietest overall but none of it is moved out of the way.
    Tpdf,
    /// Triangular dither with second-order error feedback, which pushes the
    /// noise towards the top of the band where the ear is least sensitive.
    ///
    /// Deliberately a plain (1 - z^-1)^2 shaper rather than one of the
    /// published psychoacoustic curves. Those are specific filter designs and
    /// writing one from memory would be worse than an honestly described
    /// substitute -- this is a real second-order shaper doing what a shaper
    /// does, and it is not claimed to be anyone's weighting curve. It raises
    /// total noise power while lowering it where it matters; at 16 bits and
    /// 44.1 kHz that is a good trade, at 24 bits it is pointless.
    TpdfNoiseShaped,
};

struct DitherSettings {
    DitherType type = DitherType::Tpdf;
    /// Bits in the destination. 16 and 24 are the ones that matter; anything
    /// from 2 to 32 is accepted so the module can be tested at depths where
    /// the effects are large enough to measure directly.
    int bits = 16;
    /// Fixed by default, so an export is reproducible: the same input written
    /// twice gives the same file, which matters for checksums and for tests.
    std::uint64_t seed = 0x5EED5EED5EED5EEDULL;
};

/// Streaming ditherer.
///
/// process() is allocation-free after construction. It leaves the audio ready
/// for the writer's own rounding: the values it produces round to exactly the
/// codes it chose, so nothing downstream has to know dither happened.
class Ditherer {
public:
    [[nodiscard]] static Result<Ditherer> create(const DitherSettings& settings, int channelCount);

    /// Dither `audio` in place. A channel count other than the configured one
    /// is ignored, as in the meters.
    void process(AudioBufferView audio) noexcept;

    void reset() noexcept;

    [[nodiscard]] const DitherSettings& settings() const noexcept { return settings_; }

    /// One code at the configured depth, as a fraction of full scale.
    [[nodiscard]] double step() const noexcept { return step_; }

private:
    Ditherer(const DitherSettings& settings, int channelCount);

    /// Uniform in [-0.5, 0.5). Two of these summed give the triangular
    /// distribution, which is why the generator is exposed rather than a
    /// triangular one: the sum is the definition.
    [[nodiscard]] double uniform() noexcept;

    DitherSettings settings_;
    int channelCount_ = 0;
    double step_ = 0.0;
    std::uint64_t state_ = 0;
    /// Two samples of quantisation error per channel, for the shaper.
    std::vector<double> error1_;
    std::vector<double> error2_;
};

} // namespace sa::dsp
