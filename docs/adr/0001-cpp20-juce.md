# ADR 0001 — C++20 + JUCE as the application stack

**Status:** ⚠️ **Superseded by [ADR 0006](0006-permissive-only-dependencies.md)** · 2026-09-18

> The C++20 decision stands. The **JUCE** half was reversed when a no-licence-purchases constraint was introduced: JUCE's free tier is capped at ~$20k/yr revenue, which our freemium plan is designed to exceed.

## Context
We are building a Windows-first audio editor with real-time playback, live
effect monitoring, and GPU-rendered spectrograms. Candidates considered: C++/JUCE,
C#/.NET with WinUI, Rust, and Python/Qt.

## Decision
**C++20 with JUCE.**

## Rationale
- **No garbage collector.** The audio callback must never pause. A GC pause of
  even a few milliseconds is an audible dropout. C# would have required a native
  C++ core for the engine anyway — leaving us with two languages and an interop
  boundary in the hottest path.
- **ASIO and WASAPI exclusive-mode** support is built in and battle-tested.
- **VST3/CLAP hosting** comes with the framework rather than being a project.
- **Cross-platform later is close to free**, which matters for the 2.0 macOS port.
- **Talent pool**: audio engineers already know JUCE. Hiring is easier.

## Consequences
- Slower UI iteration than a modern declarative framework. Mitigate by keeping
  `sa-ui` thin and the engine headlessly testable via `auscultate-cli`.
- JUCE licensing must be actively managed — see ADR 0002.
- Manual memory management demands discipline. Mitigate with sanitiser builds in
  CI and an audio-thread allocation assertion in debug builds.

## Revisit if
JUCE's licensing becomes untenable, or its UI layer blocks a core interaction we
cannot work around. Note that the engine modules (`sa-core` through `sa-ml`)
carry no JUCE dependency by design, so a UI-layer replacement would not be a
rewrite.
