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
| [ADRs](docs/adr/) | Architecture decision records for the choices that are expensive to reverse |

## Decisions already locked

| Decision | Choice | Why |
| -------- | ------ | --- |
| Audience | Analysis-first repair & mastering | Clearest differentiation; "analyser" is the moat |
| Stack | C++20 + Qt 6 (LGPL) + miniaudio + CLAP | Real-time safe; every dependency free in perpetuity with no revenue cap |
| Licensing | Closed-source, free at launch → freemium | **No licence purchases, ever** — see [ADR 0006](docs/adr/0006-permissive-only-dependencies.md) |
| ML | Core differentiator, on-device via ONNX Runtime | Privacy, no per-user cost, works offline |
| Dependencies | Permissive-only, CI-enforced | Free forever · closed-source OK · no revenue cap |

## Status

Planning. No code yet. Start with [04 — Roadmap](docs/04-roadmap.md), Phase 0.
