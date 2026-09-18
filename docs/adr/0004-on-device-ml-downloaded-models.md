# ADR 0004 — On-device ML, models downloaded rather than bundled

**Status:** Accepted · 2026-09-18

## Context
ML is a core differentiator: denoise, de-reverb, transcription, tagging,
eventually separation. Inference could run in the cloud or locally, and local
models could be bundled in the installer or fetched on demand.

## Decision
**On-device inference via ONNX Runtime**, with models **downloaded on first use**,
hash-verified and cached. Execution provider chain: Windows ML / DirectML →
CUDA → CPU (AVX2), behind our own abstraction.

## Rationale
- **Privacy is a feature, not a detail.** Users edit legal recordings,
  unreleased music, medical dictation, private interviews. "Your audio never
  leaves your machine" is a positioning asset we should not trade away.
- **No per-user marginal cost**, which is what makes a genuinely free tier
  sustainable.
- **Works offline.**
- Downloading rather than bundling keeps the installer small, lets models update
  independently of app releases, and — most importantly — **lets us withdraw a
  model whose licence terms change without shipping a new binary.**
- The EP abstraction matters because the DirectML execution provider is now in
  sustained engineering while Windows ML is Microsoft's forward path. Hard-coding
  an EP at call sites would age badly.

## Consequences
- We must host and serve model files, with the CDN cost that implies.
- First use of an ML feature requires a network connection, which needs clear UX.
- Every model needs a **CPU fallback with a stated time budget** — a feature that
  only works on a discrete GPU serves a minority of our audience.
- Model integrity becomes a security surface: hash verification over TLS with
  pinned roots is mandatory.

## Revisit if
A model too large to download comfortably becomes essential, or the cloud
privacy calculus changes for a specific opt-in feature (see the stem separation
fallback path in the licensing doc).
