# 04 — Roadmap

**Assumptions.** A team of 4–5 engineers: two senior C++/DSP, one UI, one
ML/applied-research, one part-time build/QA, plus part-time product design.
Timeline to 1.0: **12–15 months.** A solo developer should read the same phases
and multiply by roughly three — the ordering still holds, and every phase still
ends in something usable.

Phases overlap deliberately. The dates are relative weeks, not commitments.

---

## Phase 0 — Foundations · weeks 1–6

Prove the hard parts before building on them.

- Repo, CMake presets, vcpkg manifest, Windows CI, coding standard, clang-format
- `sa-core`: buffers, channel layouts, time units, error handling
- `sa-io`: read path for WAV/FLAC/MP3, streaming reader, memory-mapped access
- Device layer: WASAPI shared + exclusive, ASIO
- **Peak pyramid cache** and progressive waveform draw
- **Spectrogram tile cache + GPU renderer spike** — start now, it is the riskiest
  component in the product
- Golden-file test harness and benchmark harness

**Exit criteria**
- Open a 2-hour 96 kHz WAV in under 1 second
- Scroll and zoom the waveform at 60 fps
- Glitch-free playback at a 5 ms buffer on a mid-range machine
- Spectrogram spike renders a 1-hour file with smooth pan/zoom

> If the spectrogram spike fails here, the product thesis needs rework — which is
> exactly why it is in Phase 0 and not Phase 2.

## Phase 1 — The Editor · weeks 7–18

Make it a tool someone would actually use daily.

- Document model, EDL, undo with visual history
- Selection, cut/copy/paste/trim/split, fades and crossfades, fade-shape editor
- Clip gain and gain envelopes, normalise, silence, reverse, channel ops
- Basic effects: parametric EQ, compressor, limiter, gate
- Recording: multichannel, punch-in, pre-record buffer
- Session save/load, autosave, crash recovery
- Full import/export format matrix, metadata (BWF, ID3, iXML), dither on export
- Video audio extraction

**Exit criterion:** a producer can complete a real podcast edit end to end
without reaching for another tool.

## Phase 2 — The Analyser · weeks 12–24 *(overlaps P1)*

The half the product is named for.

- Production spectrogram: linear/log/Mel/Bark/ERB, colourmaps, dual view
- FFT analyser, loudness (LUFS I/S/M, LRA), true peak, compliance targets
- Phase correlation, goniometer, stereo width, M/S, statistics
- Octave & third-octave bands, SPL with calibration workflow
- Impulse response capture, RT60/EDT/C50/C80/D50, THD+N
- Pitch, tempo, key, onset, chroma, MFCC
- Forensics: codec cutoff, true bit depth, upsampling detection, null test
- Batch analysis and report export (CSV/JSON/PDF)

**Exit criteria**
- **Passes the EBU R128 and ITU-R BS.1770 conformance test sets**
- Band filters within IEC 61260 tolerance masks
- Spectrogram of a 1-hour file pans and zooms at 60 fps

## Phase 3 — Spectral Repair · weeks 20–34

The differentiator, and the hardest research work.

- STFT/ISTFT engine with proven perfect reconstruction
- Time–frequency selection: rectangle, lasso, brush, magic wand, harmonic select
- Spectral attenuate / gain / heal / copy / paste
- Spectral layers with the exact-reconstruction invariant
- Repair modules: de-noise (profile + adaptive), de-hum, de-click, de-crackle,
  de-clip, de-ess, de-plosive, dropout repair, hiss removal
- Room-tone generation and fill
- Real-time A/B preview throughout

**Exit criterion:** on a blind panel of genuinely damaged recordings, output is
clearly better than Audacity and competitive with RX Elements.

## Phase 4 — Machine Learning · weeks 26–40

- `sa-ml`: ONNX Runtime integration, EP chain, model registry, hash-verified
  downloader, session pooling
- Whisper transcription with word-level timestamps
- Transcript-driven editing
- Speaker diarisation, filler-word and silence removal
- ML de-noise and ML de-reverb
- Audio event classification and tagging
- Stem separation **only if licensing is resolved** — see
  [05 — Licensing](05-licensing-and-dependencies.md) §4

**Exit criteria**
- Every shipped model runs on a CPU-only machine within a stated time budget
- GPU path delivers a meaningful speedup and degrades gracefully when absent
- No model ships without a written licence determination

## Phase 5 — Pro & Polish · weeks 36–52

- Out-of-process VST3/CLAP hosting
- Batch processor with saved chains, watch folders
- Lua scripting and headless CLI
- **Entitlement system wired through every feature** (all flags returning `true`)
- Installer, EV code signing, auto-update, consent-gated crash reporting
- Accessibility, localisation, keymap compatibility packs
- Opt-in telemetry with a visible field list
- Performance pass, memory pass, documentation, tutorials

**Exit criterion:** 1.0.

## After 1.0

| Release | Theme |
| --- | --- |
| 1.1 | Multitrack sessions; video reference track |
| 1.2 | Measurement Pro module: waterfall plots, STI, branded client reports |
| 1.3 | Forensics module: splice detection, generation-loss analysis |
| 1.4 | Wow & flutter correction, azimuth correction (archival transfer) |
| 1.5 | Surround and ambisonics |
| 2.0 | macOS port · **Stage 2 freemium switch-on** |

---

## Build the entitlement seam in Phase 5, not Phase 9

Even though every feature is free at launch, route every Pro-destined capability
through a single `Entitlements::has(Capability)` check that returns `true` for
everything. Retrofitting this across a mature codebase is weeks of error-prone
work and it is the kind of change that ships licence-check bugs to paying
customers. Doing it while the code is young costs days.

## Risk register

| # | Risk | Severity | Mitigation |
| --- | --- | --- | --- |
| 1 | **Scope** — this is three products in one | High | Phases ordered so each ships something usable; 1.0 scope cuts are pre-agreed (multitrack, surround, macOS) |
| 2 | **Spectral artefacts** — phase reconstruction is genuinely hard | High | Prototype in P0/P1; proven algorithms; reconstruction error as a CI gate; budget real research time |
| 3 | **Stem separation licensing** | High | Treat as gated. Ship v1 without it. See §4 of the licensing doc for the three paths |
| 4 | **Big-file performance** | High | Out-of-core and cache-first from day one — this cannot be retrofitted |
| 5 | **GPU driver variance** across Intel/AMD/NVIDIA | Medium | CPU fallback for every renderer path; test matrix across all three vendors |
| 6 | **Free-tier running costs** — CDN, model hosting, support | Medium | Budget explicitly; models on a cheap CDN; community-first support |
| 7 | **Monetisation backlash** at the Stage 2 switch | Medium | Announce freemium intent in the first release notes; grandfather early users permanently |
| 8 | **SmartScreen / AV false positives** | Medium | EV certificate from the first public build; submit to AV vendors pre-release |
| 9 | **JUCE revenue cap crossed unnoticed** | Low | Cap counts *all* revenue and funding including donations — monitor it; upgrading is cheap relative to the breach |
| 10 | **Parser CVE in a shipped dependency** | Low/High impact | Continuous fuzzing, SBOM, CVE feed subscription, fast-patch release process |

## What I would do in the first two weeks

1. Stand up the repo, CMake presets, vcpkg manifest, and Windows CI. Nothing
   else until a hello-world builds and tests green on a clean runner.
2. **Spike the spectrogram renderer.** One week, throwaway quality, one question:
   can we pan and zoom a one-hour file at 60 fps on integrated graphics? This
   answer determines whether the product thesis survives contact with reality.
3. In parallel, spike the streaming reader and peak pyramid against a 10 GB file.
4. Resolve the JUCE licence tier in writing, and start the EV certificate
   application — it involves identity verification and takes weeks of calendar
   time that is free to spend now and expensive to spend later.
