#pragma once

#include <sa/core/AudioBuffer.h>

namespace sa {

/// Something the player runs inside the audio callback, on the block about to
/// reach the device.
///
/// It lives here rather than beside any particular processor because the
/// transport must not know what is being previewed. sa-transport and sa-engine
/// are siblings -- neither depends on the other -- so an equaliser preview
/// reaches playback by being handed in as one of these, not by the player
/// including it.
///
/// WHAT IMPLEMENTING THIS PROMISES
///
/// process() is called on the audio thread. It must not allocate, take a lock,
/// block, or do anything else with an unbounded worst case: everything the
/// callback touches has to finish inside one block period or the user hears a
/// gap. A processor that needs to be reconfigured from the interface does that
/// by publishing to itself through a lock-free hand-off and reading the result
/// here, which is what PreviewStage exists to do.
///
/// LIFETIME
///
/// A processor is handed to Player::play and must stay alive until playback
/// stops. stop() joins the worker and stops the device, so after it returns the
/// callback is not running and the processor can be destroyed. Passing one that
/// dies sooner is a use-after-free on the audio thread.
class AudioProcessor {
public:
    AudioProcessor() = default;
    virtual ~AudioProcessor() = default;

    AudioProcessor(const AudioProcessor&) = delete;
    AudioProcessor& operator=(const AudioProcessor&) = delete;
    AudioProcessor(AudioProcessor&&) = delete;
    AudioProcessor& operator=(AudioProcessor&&) = delete;

    /// Process `block` in place. Audio thread; see the class comment for what
    /// that forbids.
    virtual void process(AudioBufferView block) noexcept = 0;
};

} // namespace sa
