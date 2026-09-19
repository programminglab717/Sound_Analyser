#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/ChannelLayout.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/engine/ParameterSlot.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

/// A processor hung on the playback graph, so a setting is heard while it is
/// being changed.
///
/// Every processor in this tool is offline: choose settings, press OK, the
/// audio is rewritten, and only then can anyone hear whether it was right. That
/// is a poor way to dial in a compressor and an impossible way to dial in an
/// equaliser. This is the other arrangement -- the settings sit on the
/// playback path, the audio thread reads them, and nothing is written to the
/// document at all. Nothing here can alter a document; there is no document in
/// scope, deliberately.
///
///
/// The three problems, and what each costs
/// ---------------------------------------
/// **Crossing the thread boundary.** The settings change on the GUI thread
/// while the audio thread reads them. A mutex in the callback is not available
/// (docs/03-architecture.md §3), so the settings cross through a
/// ParameterSlot: the writer assembles a whole value and publishes it
/// wait-free, the reader collects the newest one with a relaxed load and, when
/// there is something to collect, one exchange. The reader is also the only
/// thread that ever *decides* anything -- there is no state the two sides
/// share beyond the slot.
///
/// **A changed coefficient is a step.** Loading new coefficients into a running
/// filter is a discontinuity in the output, and a discontinuity is a click. The
/// answer here is a crossfade: the incoming settings go into a second copy of
/// the processor, both run over the same audio for a few milliseconds, and the
/// output moves from one to the other one sample at a time. Interpolating the
/// coefficients themselves would be cheaper and is the other common answer, but
/// a path between two stable coefficient sets is not itself guaranteed stable,
/// and a filter that rings itself to infinity halfway through a drag is a worse
/// failure than the click it was avoiding. The crossfade cannot go unstable,
/// costs a second processor's arithmetic for five milliseconds, and works
/// unchanged for a processor whose parameters are not interpolatable at all.
///
/// The crossfade is linear, not equal-power. The two paths are the same audio
/// through slightly different filters, so they are strongly correlated and a
/// linear blend holds the level; an equal-power blend would lift the middle of
/// the fade by up to 3 dB, which is the audible artefact rather than the cure.
///
/// The incoming processor is *copied from* the outgoing one before its new
/// settings are loaded, so it starts with the filter state the audio has
/// already built up rather than from rest. Starting from rest and fading in
/// would still be click-free, but a band low enough that five milliseconds is a
/// fraction of a cycle would be faded in while still settling, and what arrives
/// at full weight would not be its steady response.
///
/// **Bypass has to be free.** A/B against the unprocessed signal is how anyone
/// judges a setting, so a bypassed stage does not run a unity filter, it does
/// not run anything: process() returns having touched no sample, and the output
/// is the input bit for bit. Engaging or releasing bypass crosses in the same
/// published value as the settings and is crossfaded like any other change, so
/// the A and the B do not each arrive with a click on the front.
///
///
/// When a change takes effect
/// --------------------------
/// At a block boundary, and at one crossfade at a time. A change arriving while
/// a crossfade is running is left in the slot until that fade completes -- the
/// slot holds the newest value, so nothing is lost, and the alternative
/// (restarting the fade from a partially mixed output) is precisely the step
/// the fade exists to avoid. A drag emitting updates every 16 ms against a 5 ms
/// fade therefore never waits; a drag emitting them faster than the fade is
/// rate-limited to the fade and still converges on the value the pointer
/// stopped at.
///
/// Settings already published when the stream starts are not a change, so the
/// first block after reset() adopts them outright without a fade. There is no
/// preceding audio for them to click against.
///
///
/// What is not claimed
/// -------------------
/// This has never driven a real audio device. It is exercised by calling
/// process() directly, which is what the audio callback does and nothing more,
/// but no claim is made here about any particular driver's block behaviour.
///
/// It does not change latency and does not pretend to: a processor that needed
/// look-ahead could not be dropped in unchanged, because nothing here delays
/// the dry path to match it.
///
/// It is not a chain. One stage carries one processor. Several stages in series
/// would work and would each fade independently, but nothing here orders them
/// or shares a fade between them.
namespace sa::engine {

/// Longest block this stage will accept in one piece. About 5.5 seconds at
/// 48 kHz, far past any usable callback size, and low enough that a nonsense
/// block size is refused at prepare() instead of demanding a huge allocation.
/// Blocks longer than this are processed in pieces rather than refused.
inline constexpr SampleCount kMaxPreviewBlockFrames = SampleCount{1} << 18;

/// Channels one stage can carry. Seven-point-one is the widest monitoring
/// layout anyone previews through, and the bound is what keeps the processors
/// inline and trivially copyable -- which is in turn what lets the audio thread
/// duplicate one for a crossfade without allocating.
inline constexpr int kMaxPreviewChannels = 8;

/// Crossfade length. Long enough that the largest realistic coefficient jump
/// spreads over hundreds of samples rather than one, short enough that a drag
/// still feels attached to the pointer.
inline constexpr double kPreviewCrossfadeSeconds = 0.005;

/// Floor for the above, for the benefit of very low sample rates, where 5 ms is
/// too few samples for the ramp to be a ramp.
inline constexpr SampleCount kMinPreviewCrossfadeFrames = 32;

/// What crosses to the audio thread: a processor's settings and whether the
/// stage is bypassed, as one value.
///
/// Bypass rides with the settings rather than in an atomic of its own so that
/// "these bands, and switch them in" is one publish and arrives as one change.
/// Two flags changing independently would let the audio thread observe a state
/// neither thread ever asked for.
template <typename Settings>
struct PreviewSettings {
    Settings processor{};

    /// A stage starts bypassed, so one that has been prepared but never told
    /// anything costs the callback nothing.
    bool bypassed = true;

    [[nodiscard]] friend bool operator==(const PreviewSettings& a,
                                         const PreviewSettings& b) noexcept {
        return a.bypassed == b.bypassed && a.processor == b.processor;
    }
};

/// The stage itself.
///
/// `Processor` supplies the audio-thread half of a previewable processor:
///
///     using Settings = ...;                            trivially copyable,
///                                                      equality-comparable
///     void configure(const Settings&) noexcept;        loads pre-computed data
///     void reset() noexcept;                           clears filter state
///     void process(AudioBufferView block) noexcept;    in place, all channels
///
/// configure() runs on the audio thread, so `Settings` must already hold
/// whatever the processor needs -- designed coefficients, not a frequency and a
/// Q. Designing, validating and refusing are the publisher's job, off the audio
/// thread, which is also the only place a refusal can be reported to anyone.
template <typename Processor>
class PreviewStage {
public:
    using Settings = typename Processor::Settings;
    using Published = PreviewSettings<Settings>;

    static_assert(std::is_trivially_copyable_v<Processor>,
                  "the audio thread duplicates a processor to crossfade, so copying one must be "
                  "a memcpy and not a constructor that might allocate");
    static_assert(std::is_nothrow_default_constructible_v<Processor>,
                  "processors are constructed before either thread exists and must not throw");

    PreviewStage() = default;

    /// Neither copyable nor movable: the audio callback holds a bare pointer to
    /// this object, so relocating it while the callback is live is a
    /// use-after-free.
    PreviewStage(const PreviewStage&) = delete;
    PreviewStage& operator=(const PreviewStage&) = delete;
    PreviewStage(PreviewStage&&) = delete;
    PreviewStage& operator=(PreviewStage&&) = delete;

    ~PreviewStage() = default;

    /// Control thread, with no block in flight. Sizes the crossfade scratch and
    /// fixes the shape of block this stage will accept.
    ///
    /// `maxBlockFrames` is a hint, not a contract: a longer block is processed
    /// in pieces of this size rather than refused, so a backend that once
    /// delivers more than it advertised is serviced. A block with a different
    /// *channel count* is another matter and is passed through untouched --
    /// see refusedBlocks().
    [[nodiscard]] Status prepare(SampleRate rate, int channelCount, SampleCount maxBlockFrames) {
        if (!rate.isValid()) {
            return Error{ErrorCode::InvalidArgument, "sample rate is not a usable audio rate"};
        }
        if (channelCount < 1 || channelCount > kMaxPreviewChannels) {
            return Error{ErrorCode::InvalidArgument, "preview carries 1 to 8 channels"};
        }
        if (maxBlockFrames < 1 || maxBlockFrames > kMaxPreviewBlockFrames) {
            return Error{ErrorCode::InvalidArgument, "block size is not a usable callback size"};
        }

        rate_ = rate;
        channelCount_ = channelCount;
        maxBlockFrames_ = maxBlockFrames;
        crossfadeFrames_ =
            std::max(kMinPreviewCrossfadeFrames, secondsToSamples(kPreviewCrossfadeSeconds, rate));
        scratch_.resize(ChannelLayout::discrete(channelCount), maxBlockFrames);

        // Anything published for the previous shape describes filters designed
        // for it, so it is superseded here rather than left in the slot to be
        // adopted at the new rate. The caller publishes again afterwards.
        pending_ = Published{};
        slot_.publish(pending_);
        active_ = Published{};
        prepared_ = true;
        reset();
        return {};
    }

    [[nodiscard]] bool isPrepared() const noexcept { return prepared_; }

    [[nodiscard]] SampleRate sampleRate() const noexcept { return rate_; }

    [[nodiscard]] int channelCount() const noexcept { return channelCount_; }

    /// Samples a change is spread over. Exposed because a test that bounds the
    /// step a change puts in the output has to know it.
    [[nodiscard]] SampleCount crossfadeFrames() const noexcept { return crossfadeFrames_; }

    /// Control thread. Replaces the processor settings, keeping bypass as it is.
    void publish(const Settings& settings) noexcept {
        pending_.processor = settings;
        slot_.publish(pending_);
    }

    /// Control thread. Switches the stage in or out, keeping the settings.
    void setBypassed(bool bypassed) noexcept {
        pending_.bypassed = bypassed;
        slot_.publish(pending_);
    }

    /// Control thread. What the last publish said, which is what the audio
    /// thread will be running within a block. Not a read of the audio thread's
    /// state -- asking that question across the boundary is what this class
    /// exists to avoid.
    [[nodiscard]] bool isBypassed() const noexcept { return pending_.bypassed; }

    [[nodiscard]] const Settings& pendingSettings() const noexcept { return pending_.processor; }

    /// Audio thread. Processes `block` in place.
    ///
    /// Allocation-free, lock-free and wait-free. A bypassed stage with no
    /// change waiting returns without touching a sample.
    void process(AudioBufferView block) noexcept {
        const SampleCount frames = block.frames();
        if (!prepared_ || frames <= 0 || block.channelCount() <= 0) {
            return;
        }
        if (block.channelCount() != channelCount_) {
            // No filter state exists for channels this stage was not prepared
            // for, and half-processing a block is worse than not processing it.
            // Counting it is how the mistake reaches anyone: reporting from
            // here cannot allocate a message.
            refusedBlocks_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        for (SampleCount done = 0; done < frames;) {
            const SampleCount count = std::min(maxBlockFrames_, frames - done);
            processPiece(block.subRange(done, count));
            done += count;
        }
    }

    /// Audio thread, or the control thread while no block is in flight. Clears
    /// filter state and abandons any crossfade, so what follows depends only on
    /// what comes next -- which is what a seek needs. The settings are kept.
    void reset() noexcept {
        for (Processor& processor : processors_) {
            processor.reset();
        }
        fadeRemaining_ = 0;
        firstBlock_ = true;
    }

    /// Blocks passed through untouched because their channel count was not the
    /// one prepare() was given. Non-zero means the stage and the device
    /// disagree about the stream, and the user is hearing no preview at all.
    [[nodiscard]] std::uint64_t refusedBlocks() const noexcept {
        return refusedBlocks_.load(std::memory_order_relaxed);
    }

    /// Audio thread. True while a change is being faded in.
    [[nodiscard]] bool isCrossfading() const noexcept { return fadeRemaining_ > 0; }

private:
    [[nodiscard]] std::size_t incomingIndex() const noexcept {
        return activeIndex_ ^ std::size_t{1};
    }

    void processPiece(AudioBufferView block) noexcept {
        if (fadeRemaining_ == 0) {
            Published incoming;
            if (slot_.fetch(incoming)) {
                takeUp(incoming);
            }
        }
        firstBlock_ = false;

        if (fadeRemaining_ > 0) {
            const SampleCount faded = std::min(block.frames(), fadeRemaining_);
            crossfade(block.subRange(0, faded));
            fadeRemaining_ -= faded;
            if (fadeRemaining_ == 0) {
                activeIndex_ = incomingIndex();
                active_ = target_;
            }
            if (faded < block.frames()) {
                runActive(block.subRange(faded, block.frames() - faded));
            }
            return;
        }

        runActive(block);
    }

    /// Decides what a freshly collected value means: nothing, an outright
    /// adoption, or a crossfade.
    void takeUp(const Published& incoming) noexcept {
        if (incoming == active_) {
            return;
        }
        // Nothing is audible on either side of a change made while bypassed, so
        // there is nothing to fade and no reason to run two processors.
        if (firstBlock_ || (active_.bypassed && incoming.bypassed)) {
            active_ = incoming;
            processors_[activeIndex_].reset();
            processors_[activeIndex_].configure(incoming.processor);
            return;
        }

        target_ = incoming;
        // The duplicate carries the state the audio has already built up, so
        // the incoming path is already settled when it reaches full weight.
        processors_[incomingIndex()] = processors_[activeIndex_];
        processors_[incomingIndex()].configure(incoming.processor);
        fadeRemaining_ = crossfadeFrames_;
    }

    void runActive(AudioBufferView block) noexcept {
        if (active_.bypassed) {
            return;
        }
        processors_[activeIndex_].process(block);
    }

    void crossfade(AudioBufferView block) noexcept {
        const SampleCount count = block.frames();
        const AudioBufferView spare = scratch_.view().subRange(0, count);

        for (int channel = 0; channel < channelCount_; ++channel) {
            std::copy_n(block.channel(channel), count, spare.channel(channel));
        }

        runActive(block);
        if (!target_.bypassed) {
            processors_[incomingIndex()].process(spare);
        }

        const SampleCount reached = crossfadeFrames_ - fadeRemaining_;
        const auto length = static_cast<float>(crossfadeFrames_);
        for (int channel = 0; channel < channelCount_; ++channel) {
            float* out = block.channel(channel);
            const float* in = spare.channel(channel);
            for (SampleCount i = 0; i < count; ++i) {
                // Divided rather than multiplied by a reciprocal, so the last
                // sample of the fade weighs exactly one and the outgoing path
                // is gone rather than a rounding error below audibility.
                const float weight = static_cast<float>(reached + i + 1) / length;
                out[i] = (1.0f - weight) * out[i] + weight * in[i];
            }
        }
    }

    ParameterSlot<Published> slot_;

    /// Control-thread mirror of what has been published. Held so that changing
    /// the settings and changing bypass can each leave the other alone.
    Published pending_{};

    /// Audio-thread state. None of it is read or written by any other thread.
    std::array<Processor, 2> processors_{};
    std::size_t activeIndex_ = 0;
    Published active_{};
    Published target_{};
    SampleCount fadeRemaining_ = 0;
    bool firstBlock_ = true;

    AudioBuffer scratch_;
    SampleRate rate_;
    int channelCount_ = 0;
    SampleCount maxBlockFrames_ = 0;
    SampleCount crossfadeFrames_ = kMinPreviewCrossfadeFrames;
    bool prepared_ = false;

    std::atomic<std::uint64_t> refusedBlocks_{0};
};

} // namespace sa::engine
