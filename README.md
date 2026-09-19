# Sound Analyser

**An analysis-first audio repair and mastering workstation for Windows.**

> Working title. The product is a Windows desktop application that treats the
> spectrogram — not the waveform — as the primary editing surface, and pairs a
> standards-compliant measurement engine with on-device ML.

---

## The one-paragraph pitch

Every audio editor shows you a waveform and hides analysis in a side panel. That
is backwards for the work people actually struggle with: rescuing a bad
recording. Sound Analyser inverts it — you *see* the noise, the hum, the click,
the room, and you edit it directly where you see it. It is free to use, it runs
entirely on your machine, and its measurements are accurate enough to deliver
against a broadcast spec.

## Positioning

|                     | Sound Analyser | iZotope RX | Adobe Audition | Audacity | SpectraLayers |
| ------------------- | -------------- | ---------- | -------------- | -------- | ------------- |
| Price               | **Free → freemium** | $400–1200 | Subscription | Free | ~$300 |
| Spectral editing    | **Core**       | Strong     | Moderate       | None     | Core          |
| Standards metering  | **Core**       | Good       | Good           | Minimal  | Minimal       |
| Acoustic measurement| **Yes**        | No         | No             | No       | No            |
| On-device ML        | **Yes**        | Yes        | Some           | No       | Some          |
| Open source         | No             | No         | No             | Yes      | No            |

The wedge: **there is no good free spectral repair tool.** Audacity is free but
its analysis is primitive. RX is excellent but costs more than most of its
would-be users earn from audio. We take the free slot with a genuinely
professional analysis engine, then monetise capability (batch, advanced ML,
plugin hosting, measurement reporting) rather than access.

## Documentation

| Doc | What it covers |
| --- | --- |
| [01 — Product brief](docs/01-product-brief.md) | Vision, personas, competitive landscape, business model phasing |
| [02 — Feature specification](docs/02-feature-spec.md) | The complete feature catalogue, phased and prioritised |
| [03 — Architecture](docs/03-architecture.md) | Module layout, threading model, data structures, performance strategy |
| [04 — Roadmap](docs/04-roadmap.md) | Six delivery phases with exit criteria, team shape, risk register |
| [05 — Licensing & dependencies](docs/05-licensing-and-dependencies.md) | Dependency-by-dependency legal analysis, and the traps |
| [06 — Spike: spectrogram at 60 fps](docs/06-spike-spectrogram.md) | The Phase 0 gating risk, measured |
| [07 — Work queue](docs/07-autonomous-queue.md) | What is next, what is blocked, and on what |
| [08 — What needs you](docs/08-for-review.md) | The list that cannot be closed without a person |
| [ADRs](docs/adr/) | Architecture decision records for the choices that are expensive to reverse |

## Decisions already locked

| Decision | Choice | Why |
| -------- | ------ | --- |
| Audience | Analysis-first repair & mastering | Clearest differentiation; "analyser" is the moat |
| Stack | C++20 + Qt 6 (LGPL, dynamic) | Real-time safe; every dependency free in perpetuity with no revenue cap. The audio device layer, the FFT and the file I/O were written rather than taken, which is why the dependency count is six |
| Licensing | Closed-source, free at launch → freemium | **No licence purchases, ever** — see [ADR 0006](docs/adr/0006-permissive-only-dependencies.md) |
| ML | Core differentiator, on-device via ONNX Runtime | Privacy, no per-user cost, works offline |
| Dependencies | Permissive-only, CI-enforced | Free forever · closed-source OK · no revenue cap |

## Status

**Phase 0 and the core of Phase 1 are done: the application runs, edits, repairs,
measures, plays and saves.** See [04 — Roadmap](docs/04-roadmap.md) and
[BUILDING.md](docs/BUILDING.md).

| Area | State |
| --- | --- |
| Build, presets, warnings-as-errors, CI on Windows and Linux | ✅ Done — ASan, UBSan and TSan all gate every commit |
| Licence allowlist gate (`tools/check_licences.py`) | ✅ Done, negative-tested three ways |
| `sa-core`: buffers, channel layouts, time types, `Result`, RT instrumentation | ✅ Done |
| `sa-dsp`: FFT, windows, STFT, biquads, EQ, dynamics, resampling | ✅ Done — STFT round trip is a CI gate |
| `sa-io`: WAV, AIFF, FLAC, MP3, peak pyramid, WAV writer | ✅ Done |
| `sa-analysis`: LUFS, true peak, statistics, compliance targets | ✅ Done — see the caveat below |
| `sa-spectral`: spectrogram pyramid, attenuate and heal | ✅ Done |
| `sa-engine`: non-destructive document, edits, undo, sessions | ✅ Done |
| `sa-device`: WASAPI, ALSA, null backend | ✅ Done — never run on real hardware |
| `sa-transport`: playback with a playhead | ✅ Done |
| `sa-ui`: waveform, spectrogram, rulers, meters, editing, repair | ✅ Done |
| Resampling: any ratio, streaming, real-time safe | ✅ Done |
| Noise profile learning and spectral denoise | ✅ Done |
| Click detection and repair, by linear prediction | ✅ Done |
| Declipping: restoring peaks a converter took off | ✅ Done |
| Mastering: filters, true-peak limiting, normalisation | ✅ Done |
| `sa-cli`: headless batch driver | ✅ Done — nine commands, tested end to end |
| Time-stretch and pitch-shift | ✅ Done — phase vocoder with identity phase locking |
| Markers and regions in the interface; a draggable EQ curve | ⬜ Next |
| GPU shader renderer | ⬜ An optimisation, not a requirement — the CPU path fits in the frame budget |

**581 tests passing on GCC 13, under ASan/UBSan with leak detection, and under
ThreadSanitizer.** CI runs the same suite on MSVC 19 (Visual Studio 18), and
the most recent run was green on every job -- both Windows configurations
included -- and produced a packaged Windows build as an artifact.

Four things the tests check that are worth naming, because each one covers a
whole chain rather than a unit:

- **Measure, normalise, export, re-measure lands on −23.000 LUFS** against EBU
  R128's −23.0. The K-weighting, the gating, the gain verb, the render and the
  WAV writer all have to be right for that to happen.
- **Editing is compared sample by sample.** Ten operations are driven through the
  window headlessly, exported, and checked against what they should have
  produced. All ten are bit-exact.
- **Processing is checked against theory, not against itself.** A second-order
  Butterworth has a magnitude anyone can write down; the filter is held to that
  figure to a tenth of a decibel rather than to "it got quieter", and to a
  twentieth of one in the band it was not pointed at. The same standard applies
  to the limiter, which is judged by a band-limited reconstruction that shares
  no code with the detector it was built against.
- **Races are looked for, not waited for.** A rare crash in the interface turned
  out to be a background spectrogram build reading the document while an edit
  rewrote it; ThreadSanitizer named the exact pair of lines. Analysis sources
  now hold their own snapshot, which makes the whole class impossible rather
  than unlikely, and a TSan run is a CI gate so the next one is found the same
  way.

### What is not true yet

Stated here rather than buried, because the gap between what a tool claims and
what it has been shown to do is the whole difference between a measurement and a
number.

- **The metering is not certified.** No official EBU or ITU conformance vectors
  have been run. The numbers are internally consistent and anchored at 1 kHz --
  a 1 kHz stereo sine at −23 dBFS reads −23.0 LUFS — but they are not conformant
  until those vectors are run.

  The true peak a file is *shown* is now exact rather than interpolated: it is
  measured by band-limited reconstruction, which has no filter to droop, and it
  agrees with an independent implementation to two decimal places. The
  real-time meter, used where an answer has to arrive within a block, is still
  a windowed-sinc substitute for BS.1770-4 Annex 2 Table 3 and reads up to
  0.44 dB low at 4× on bright transients. Both facts are written down where the
  code is.
- **No audio has come out of real hardware.** WASAPI and ALSA are written and
  tested against a real thread on a real clock, and the ALSA path streams through
  ALSA itself, but nothing here has driven a sound card.
- **Nothing has been compiled by MSVC except through CI.** That is what CI is
  for, and it is green, but no one has built this on a developer's Windows
  machine.

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```
