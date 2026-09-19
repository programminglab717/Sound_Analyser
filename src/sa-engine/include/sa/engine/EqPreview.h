#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/dsp/Biquad.h>
#include <sa/dsp/BiquadCascade.h>
#include <sa/dsp/ParametricEq.h>
#include <sa/engine/PreviewStage.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

/// A parametric equaliser the user can hear while dragging it.
///
/// sa::ui::EqCurve already turns a gesture into bands and draws the response
/// those bands produce, and its header records what was missing: dragging a
/// band changed a drawing, and the audio changed only when the curve was
/// applied to the document. This is the other half -- the same bands, hung on
/// the playback graph, so the drag is audible while it happens and nothing is
/// written anywhere.
///
/// The split between the two threads is the whole design. Bands are designed
/// into coefficients on the thread that changed them, where a band that cannot
/// be realised can be refused and the refusal can be explained to someone. What
/// crosses to the audio thread is the finished coefficients, which the filters
/// load without designing, validating or refusing anything. The audio thread
/// never sees a frequency or a Q.
///
/// Those coefficients are designed through sa::dsp::ParametricEq -- the same
/// designer the curve on screen reads its response from -- so the curve cannot
/// drift away from what the speakers do. A second design path that happened to
/// agree today is exactly how that drift starts.
///
///
/// What is not claimed
/// -------------------
/// This has never been driven by a real audio device. It is exercised by
/// calling process() the way a callback would, and the hook-up to a device
/// callback lives above this module.
///
/// It previews; it does not render. Nothing here writes to a document, and the
/// audio a preview produces is not the audio a later offline pass would write
/// -- the two agree to a tolerance, because a preview crossfades its changes
/// and carries filter state from whatever was playing before, and an offline
/// render starts from rest with one fixed setting.
namespace sa::engine {

/// Designed coefficients for one equaliser, as the audio thread wants them.
///
/// A fixed array rather than a list, so the whole value is trivially copyable
/// and the slot that carries it never allocates. Sections beyond `sectionCount`
/// are not read.
struct EqPreviewSettings {
    static constexpr int kMaxSections = dsp::ParametricEq::kMaxBands;

    std::array<dsp::BiquadCoefficients, static_cast<std::size_t>(kMaxSections)> sections{};
    int sectionCount = 0;

    /// Exact comparison of the coefficients, deliberately. The question being
    /// asked is "are these the numbers already loaded", so that a republished
    /// identical curve does not start a crossfade against itself; a tolerance
    /// would answer a different question and would let a real small change be
    /// dropped.
    [[nodiscard]] friend bool operator==(const EqPreviewSettings& a,
                                         const EqPreviewSettings& b) noexcept {
        if (a.sectionCount != b.sectionCount) {
            return false;
        }
        for (int i = 0; i < a.sectionCount; ++i) {
            const dsp::BiquadCoefficients& x = a.sections[static_cast<std::size_t>(i)];
            const dsp::BiquadCoefficients& y = b.sections[static_cast<std::size_t>(i)];
            if (x.b0 != y.b0 || x.b1 != y.b1 || x.b2 != y.b2 || x.a1 != y.a1 || x.a2 != y.a2) {
                return false;
            }
        }
        return true;
    }
};

/// The audio-thread half: one cascade per channel, and nothing else.
///
/// Trivially copyable on purpose -- PreviewStage duplicates one to crossfade a
/// change, and that duplication happens on the audio thread, where a copy that
/// ran a constructor or touched the heap would be a fault.
class EqPreviewProcessor {
public:
    using Settings = EqPreviewSettings;

    /// Audio thread. Loads coefficients that were designed elsewhere.
    ///
    /// When the section count is unchanged the coefficients are replaced in
    /// place, which leaves each filter's state alone -- the common case during
    /// a drag, and the one where keeping the state matters, because the band
    /// being moved is still ringing. A changed count cannot keep the state:
    /// after a shift the states no longer belong to the coefficients sitting
    /// next to them. PreviewStage's crossfade is what covers the difference.
    void configure(const Settings& settings) noexcept {
        // Clamped rather than trusted, so the calls below cannot take their
        // failure paths -- which build a message, which would allocate.
        const int count = std::clamp(settings.sectionCount, 0, dsp::BiquadCascade::kMaxSections);

        for (dsp::BiquadCascade& cascade : cascades_) {
            const bool sameShape = cascade.sectionCount() == count;
            if (!sameShape) {
                cascade.clear();
            }
            for (int i = 0; i < count; ++i) {
                const dsp::BiquadCoefficients& coefficients =
                    settings.sections[static_cast<std::size_t>(i)];
                const Status status =
                    sameShape ? cascade.setSection(i, coefficients) : cascade.append(coefficients);
                if (!status) {
                    break;
                }
            }
        }
    }

    void reset() noexcept {
        for (dsp::BiquadCascade& cascade : cascades_) {
            cascade.reset();
        }
    }

    /// Audio thread. Filters `block` in place, one cascade per channel.
    void process(AudioBufferView block) noexcept {
        const int channels = std::min(block.channelCount(), kMaxPreviewChannels);
        for (int channel = 0; channel < channels; ++channel) {
            cascades_[static_cast<std::size_t>(channel)].processInPlace(block.channel(channel),
                                                                        block.frames());
        }
    }

private:
    std::array<dsp::BiquadCascade, static_cast<std::size_t>(kMaxPreviewChannels)> cascades_{};
};

/// The equaliser preview, as the interface above it uses it.
///
/// Three calls do the whole job. From the GUI thread, prepare() once per
/// stream, then setBands() on every change and setBypassed() for the A/B. From
/// the audio callback, process() on the block about to be played.
class EqPreview {
public:
    static constexpr int kMaxBands = EqPreviewSettings::kMaxSections;

    /// GUI thread, with no block in flight -- typically while the device is
    /// stopped, because this allocates the crossfade scratch.
    ///
    /// Any bands already given are discarded: they were designed for the old
    /// rate and mean something else at the new one. Call setBands() again
    /// afterwards. The stage comes back bypassed, which is the state in which
    /// it costs the callback nothing.
    [[nodiscard]] Status prepare(SampleRate rate, int channelCount, SampleCount maxBlockFrames);

    /// GUI thread. Designs `bands` at the prepared rate and publishes them.
    ///
    /// Refuses, and changes nothing that is playing, when a band cannot be
    /// realised -- a frequency outside (0, Nyquist), a non-positive Q, a
    /// non-finite gain, more bands than the equaliser holds, or a design that
    /// comes out unstable. The last of those cannot happen with the cookbook
    /// shapes and valid parameters; it is checked because the alternative to
    /// checking is an unstable filter running on the audio thread, where
    /// nothing can be done about it.
    ///
    /// An empty band list is legal and means a flat equaliser.
    [[nodiscard]] Status setBands(std::span<const dsp::EqBand> bands);

    /// GUI thread. Switches the equaliser out of the signal path, and back in.
    ///
    /// A bypassed preview does not run a flat filter; it does not run. The
    /// output is the input sample for sample, and the callback's cost for this
    /// stage is one comparison.
    void setBypassed(bool bypassed) noexcept { stage_.setBypassed(bypassed); }

    [[nodiscard]] bool isBypassed() const noexcept { return stage_.isBypassed(); }

    /// Audio thread. Processes `block` in place. Allocation-free and
    /// lock-free; see PreviewStage.
    void process(AudioBufferView block) noexcept { stage_.process(block); }

    /// Audio thread, or the GUI thread while no block is in flight. Clears
    /// filter state -- what a seek needs, so that the audio after it does not
    /// carry the ring of the audio before it.
    void reset() noexcept { stage_.reset(); }

    [[nodiscard]] bool isPrepared() const noexcept { return stage_.isPrepared(); }

    [[nodiscard]] SampleRate sampleRate() const noexcept { return stage_.sampleRate(); }

    [[nodiscard]] int channelCount() const noexcept { return stage_.channelCount(); }

    /// Samples a change is spread over.
    [[nodiscard]] SampleCount crossfadeFrames() const noexcept { return stage_.crossfadeFrames(); }

    /// Blocks passed through untouched because they did not have the channel
    /// count prepare() was given. Non-zero means no preview was heard at all.
    [[nodiscard]] std::uint64_t refusedBlocks() const noexcept { return stage_.refusedBlocks(); }

    /// Audio thread. True while a change is being faded in.
    [[nodiscard]] bool isCrossfading() const noexcept { return stage_.isCrossfading(); }

    /// The coefficients last accepted. For tests and diagnostics: it is the
    /// GUI thread's copy, not a reading of the audio thread's.
    [[nodiscard]] const EqPreviewSettings& publishedSettings() const noexcept {
        return stage_.pendingSettings();
    }

private:
    PreviewStage<EqPreviewProcessor> stage_;
};

} // namespace sa::engine
