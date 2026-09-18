# ADR 0006 — Permissive-only dependencies; drop JUCE

**Status:** Accepted · 2026-09-18
**Supersedes:** [ADR 0001](0001-cpp20-juce.md), [ADR 0002](0002-closed-source-juce-starter.md)

## Context

A hard constraint was introduced: **no licence purchases at any point.** Every
dependency must be free in perpetuity for closed-source commercial distribution,
or built in-house.

ADR 0001 chose JUCE, and ADR 0002 planned to use its free Starter tier now and
upgrade to a paid tier at monetisation. That upgrade is now prohibited.

JUCE's Starter tier is capped at roughly **$20k/yr**, and the cap counts gross
revenue across all uses of the framework, including donations, sponsorship and
advertising. Exceeding it obliges us to purchase a licence **or cease
distributing**. Our freemium plan is explicitly designed to exceed it — the
plan's success condition is the licence tier's breach condition.

## Decision

1. **Every dependency must pass three tests:** free forever · closed-source
   permitted · no revenue, seat or unit cap.
2. **Drop JUCE.** Replace with **Qt 6 (LGPLv3, dynamically linked)** for the
   application shell, **miniaudio** (public domain) for device I/O, and **CLAP**
   (MIT) for plugin hosting.
3. **Keep C++20.** That half of ADR 0001 was never in question.
4. **Stay closed-source.** ADR 0002's reasoning is unaffected.
5. **Enforce the policy in CI** via a licence allowlist gate, not by review.

## Rationale

**The timing asymmetry is decisive.** There is no code yet. Switching now costs
6–10 weeks of foundation work; switching later means ripping out the UI and
audio layers of a mature application, under revenue pressure, at the moment of
commercial traction. This decision will never be as cheap as it is today.

**JUCE's value to *this* product is lower than usual.** The spectrogram,
waveform, meters and analysis panels are all custom GPU-rendered surfaces we
were writing ourselves regardless. What JUCE actually supplied here was device
I/O and plugin hosting — both of which have mature permissive equivalents.

**Qt LGPLv3 over Dear ImGui** because the transcript editor and the
accessibility commitment need real text handling and real widget semantics,
which is exactly where immediate-mode GUIs are weakest. The LGPL obligations are
mechanical and we had already planned the attribution screen.

**CLAP over VST3** because CLAP is MIT with no agreement to sign, and is a
better-designed API. VST3 support can be added later via Steinberg's free (but
signature-requiring) agreement if users demand it.

## Consequences

- **+6–10 weeks** in Phases 0–1 to build the DSP primitives, time/pitch engine,
  device abstraction and UI shell that JUCE would have supplied.
- **We own the DSP library outright** — no third-party terms to untangle if we
  ever relicense, open-source, or port.
- **No revenue cap, ever.** The constraint that prompted this is permanently
  satisfied rather than deferred.
- **LGPL compliance obligations** for Qt and FFmpeg: dynamic linking only,
  relink rights, source offer. CI must enforce the no-static-link rule.
- **Qt's GPL-only modules are off limits** (Charts, Data Visualization, Virtual
  Keyboard). CI checks the linked module list.
- **Stem separation is dropped from the roadmap** — licensing costs money,
  training costs money, and the good free weights are non-commercial.
- **ASIO is deferred.** WASAPI exclusive mode reaches 3–10 ms, which is ample
  for an editor.

## Revisit if

The no-purchases constraint softens — for example, if a ~$1000 perpetual JUCE
licence becomes acceptable once paid out of the revenue that triggered it. The
engine modules (`sa-core` … `sa-ml`) carry no UI-framework dependency by design,
so reversing this would touch only `sa-ui` and the device layer. We are
declining the bridge, not burning it.
