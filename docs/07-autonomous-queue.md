# 07 — Autonomous work queue

Working order for unattended sessions. The rule for what belongs here: it must
be **fully verifiable in this environment** — Linux, no GPU, no display, no
audio hardware, no Windows — plus GitHub Actions for real MSVC builds.

Anything needing a product decision, a purchase, or hardware goes in
§Blocked instead, and waits.

---

## Working order

Each item: finish it, verify locally on all three presets, push, **confirm CI
green before starting the next**. CI is the only real-hardware check available,
and treating local green as sufficient is exactly how several bugs reached the
branch unnoticed.

1. **Drive Windows CI to green.** In progress. Nothing else starts until the
   suite passes on MSVC debug and release.
2. **True-peak limiting.** The feature spec lists "limiter (true-peak)" as P1
   and the current limiter guards sample peaks only, so inter-sample peaks can
   still exceed the ceiling after reconstruction. Needs oversampled detection;
   `sa-analysis` already has the interpolator.
3. **Stereo linking for dynamics.** Every processor is single-channel today, so
   two instances across a stereo pair shift the image on hard-panned transients.
   Needs a summed sidechain and a link amount.
4. **Lazy spectrogram tiles with LRU eviction.** The Phase 0 spike measured an
   eager full-file pyramid at 1.3 GB per hour of stereo, which is not viable.
   Generate for the visible range plus a margin, evict under a memory budget,
   persist keyed by content hash.
5. **High-quality resampler.** Needed for rate conversion on import and export,
   and by the time-stretch work below.
6. **Time-stretch and pitch-shift.** Rubber Band is GPL/commercial and excluded
   by ADR 0006, so this is in-house: phase-locked vocoder plus WSOLA.
7. **FLAC decoding.** dr_flac is Unlicense and passes the gate. Unlike WAV,
   there is no metadata reason to write our own.
8. **`sa-cli`.** A headless driver for the engine: analyse, convert, batch. The
   architecture doc calls for it early as a forcing function for keeping the
   engine independent of any UI.
9. **Spectral repair primitives.** De-hum, de-click, de-clip, noise-profile
   denoise. Phase 3 work, all testable headlessly.
10. **Acoustic measurement.** Octave and third-octave bands to IEC 61260,
    impulse response capture, RT60/EDT/C50/C80.
11. **Content analysis.** Pitch and F0 contour, tempo and beat grid, key.
12. **Forensics.** Lossy-codec cutoff detection, true bit-depth detection,
    A/B null test.

## Worth attempting, uncertain

- **EBU/ITU conformance vectors.** Phase 2's exit criterion needs them and they
  have never been run. Network works here, so fetching them may be possible —
  if it is, this jumps to the top of the list, because it converts the metering
  from "internally consistent" to "conformant" and that gap is currently the
  biggest overstatement risk in the project.
- **The Qt shell.** Could be written and compiled through CI without ever being
  seen. Whether that is worth doing blind is a judgement call: UI written
  without looking at it is usually wrong in ways tests do not catch. Not started
  unattended without a reason.

## Blocked — do not attempt unattended

| Item | Needs |
| --- | --- |
| GPU shader renderer | Real GPU across Intel/AMD/NVIDIA. Phase 0's last open risk. |
| WASAPI/ASIO backend | Audio hardware. The abstraction and null backend exist. |
| Visual verification of any UI | A display. |
| True-peak filter conformance | BS.1770-4 Annex 2 Table 3, transcribed from the published standard rather than memory. |
| Platform compliance targets | Checking against live platform documentation before they ship as presets. |
| Product name, pricing, licence sign-off | A person. |
| Code-signing certificate | A person and about $10/month. |

## Standing rules for unattended work

- **Commit and push after every completed item.** The container is ephemeral;
  uncommitted work is lost when it is reclaimed.
- **Check CI after every push.** Local green is not evidence about Windows.
- **Never weaken a test to make it pass.** If an assertion fails, either the
  code is wrong or the assertion was wrong — establish which, and say which in
  the commit message.
- **Record what was not verified.** Every commit message states what the change
  does not prove, in the same voice as the rest of this repository.
- **Stop and leave a note rather than guess** on anything in §Blocked, or on any
  decision that would be expensive to reverse.
