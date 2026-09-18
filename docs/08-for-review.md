# 08 — What needs you

Everything in this file needs a person. Nothing here can be closed from inside
the container: it needs hardware, a listening test, a published document, or a
decision that is yours rather than mine.

Ordered by how much it would unblock.

---

## 1. Run it, and tell me what is wrong with it

**What to do.** Download `sound-analyser-windows` from the latest green CI run's
artefacts (Actions → the run → Artifacts at the bottom), unzip it anywhere, and
run `sound-analyser.exe`. Qt ships beside it, so there is nothing to install.

It is about 12.6 MB and it is built and tested by the same run that produces it:
all ten CI jobs green, including both MSVC configurations, before the package is
uploaded. `sa-cli.exe` is not in the package yet -- say if you want it and it is
a one-line change. It now has seven commands: analyse, convert, normalise,
denoise, render, stretch and pitch.

Then open something real — a recording of your own, not a test tone — and try:

| | |
| --- | --- |
| Open | File ▸ Open audio. WAV, AIFF, FLAC and MP3 all work |
| Look | Scroll wheel zooms, shift+wheel scrolls, middle-drag or alt-drag pans |
| Select | Drag in the waveform for a time span; drag in the spectrogram for a time *and frequency* box |
| Play | Space |
| Measure | The panel on the right; it follows the selection |
| Fix the level | Process ▸ Normalise to target, after picking a target in the panel |
| Filter | Process ▸ Filter, or Ctrl+F. High-pass at 80 Hz is the one to try first on anything with rumble in it |
| Limit | Process ▸ Limiter. It holds a true-peak ceiling rather than approaching it |
| Stretch | Process ▸ Time stretch. Asked for as a percentage of the current length, so 200 is twice as long. Pitch stays put |
| Retune | Process ▸ Pitch shift. Semitones, and fractions of one — 0.01 is a cent, which is what a tuning fix actually needs |
| Repair | Draw a box round a hum or a click, then Repair ▸ Attenuate or Heal |
| Denoise | Select a passage of noise alone ▸ Repair ▸ Learn noise profile, then select the whole thing ▸ Repair ▸ Reduce noise |
| Save | File ▸ Save session, reopen it, check nothing was lost |

**What I need back.** Not a bug list — impressions. Where did you expect
something to be and not find it? What looked wrong before you could say why?
What did you try that I have not built? Anything you had to think about for more
than a second is a design failure worth hearing about.

**Specifically, does sound come out?** No audio has ever left this machine. The
WASAPI backend compiles and its logic is tested, but it has never driven a sound
card. If playback is silent, or crackles, or the playhead drifts from what you
hear, that is the single most valuable thing you can tell me.

## 2. Metering conformance vectors

**Why it needs you.** The loudness meter is internally consistent and anchored
correctly — a 1 kHz stereo sine at −23 dBFS reads −23.0 LUFS — but no official
conformance material has been run against it. Until it has, "EBU R128 compliant"
is a claim we have not earned and I will not write it in the product.

**What would close it.** The EBU Tech 3341 and Tech 3342 test material, and
ITU-R BS.2217. These are published by the EBU and the ITU. If you can obtain
them legitimately, drop them somewhere I can read and I will run the suite
against them and report each case pass or fail.

**Related, and worse.** BS.1770-4 Annex 2 Table 3 specifies the exact
interpolation filter a true-peak meter must use. I have not transcribed it,
because writing a filter table from memory would be worse than admitting I do
not have it. Ours under-reads by up to 0.44 dB at 4x oversampling — measured,
documented in `TruePeakMeter.h`, and guarded by a test. With the published table
that becomes exact.

## 3. Decisions that are yours

| Decision | Why it cannot wait forever | My recommendation |
| --- | --- | --- |
| **The name.** "Sound Analyser" is a working title | It is in the window title, the executable, the session extension and the namespace | Decide before anyone outside sees it; renaming later costs a day and is never done cleanly |
| **What is free and what is paid** | The feature spec tags tiers, but those tags are guesses | Everything built so far should stay free. Charge for batch processing, plugin hosting, and measurement reporting — capability, not access |
| **Whether we ever ship a Mac or Linux build** | The device layer already has ALSA, and nothing above it is Windows-specific | Cheap to keep the option open, so keep it open; do not announce it |
| **Crash reporting** | Needs a privacy position before any code is written | Opt-in, on-device symbolisation, nothing sent without a prompt |

## 4. Things I would want a second opinion on

Not blocked, but a person's judgement would be better than mine.

- **How a stretch sounds to you.** The measurements say a stretched tone is
  the same tone and a shifted one has moved by exactly the right ratio, and
  both are exact. What measurements cannot tell me is how a *drum* sounds
  stretched to 130%, because a phase vocoder smears transients by construction
  and no number I can produce here says whether that is acceptable or
  embarrassing. Try it on something percussive and tell me.
- **The repair defaults.** Noise reduction defaults to 12 dB with 1.5x
  oversubtraction. Those produce clean results on my synthetic tests, but
  synthetic noise is stationary and real noise is not. Try it on a real bad
  recording and tell me whether it sounds thin, watery, or fine.
- **Whether the spectrogram is readable to you.** I chose magma by default and
  excluded rainbow deliberately (not perceptually uniform, and unreadable to
  red-green colour blindness). If viridis or greyscale reads better to you, that
  is data I cannot generate myself.
- **How much the window should do before it feels crowded.** There is a meters
  panel and five menus now. The next additions — an EQ, a spectrum analyser, a
  history list — need somewhere to live, and the answer is probably docking
  panels, which is a day of work I would rather not do twice.

---

## What does *not* need you

For completeness, so nothing sits here waiting that should not:

- GPU rendering. Measured as unnecessary: 11.59 ms against a 16.67 ms budget at
  1080p on CPU alone.
- Any licence purchase. There will never be one — six dependencies, all free in
  perpetuity for closed-source distribution, enforced by a CI gate that now
  checks fetched, found, vendored and vcpkg dependencies alike.
- MSVC compatibility. CI builds and tests every commit on real Visual Studio.
