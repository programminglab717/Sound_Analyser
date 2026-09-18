# ADR 0007 — Spectrogram as a level pyramid uploaded to a shader

**Status:** Accepted · 2026-09-18
**Evidence:** [06 — Spike: spectrogram at 60 fps](../06-spike-spectrogram.md)

## Context

The product thesis makes the spectrogram the editing surface, so it must pan and
zoom at 60 fps over long files. The Phase 0 spike measured what that costs.

## Decision

1. **The spectrogram gets its own multi-resolution pyramid**, alongside the
   waveform's peak pyramid. Level 0 is the STFT; each level above halves time
   resolution.
2. **Upper levels max-combine adjacent frames, never average.**
3. **Magnitudes are stored as 8-bit log-magnitude**, quantised across the
   displayed dynamic range.
4. **The renderer uploads a level tile as a texture and scales in a shader.**
   The CPU never scales to screen resolution in the UI path.
5. **Tiles are generated lazily** for the visible region plus a margin, under an
   LRU memory budget.

## Rationale

**Max-combining** is the same reasoning as the peak pyramid's min/max: a click
that fades as the user zooms out is a display hiding the defect they opened the
tool to fix. Quantisation is monotonic in decibels, so taking the maximum of the
stored bytes is exactly the maximum of the magnitudes — the cheap operation is
also the correct one.

**Shader scaling** is not a preference. CPU scaling measured 11.59 ms at 1080p
and 47.58 ms at 4K against a 16.67 ms budget; uploading the level tile measured
0.21 ms. That is 55x, and — the part that matters — it is flat in screen
resolution, where CPU cost tracks pixel count and therefore gets worse as
monitors improve.

**8-bit** gives 0.47 dB per step over a 120 dB range, finer than a display
resolves, at a third the memory of 16-bit. Colour mapping and contrast live in
the shader, so changing either costs no recomputation.

**Lazy generation** because an eager full-file pyramid measured 1.3 GB for one
hour of stereo. Analysis throughput is not the constraint — 229x realtime means
3.8 seconds of audio per frame budget — so generating on demand comfortably
keeps ahead of a fast pan.

## Consequences

- An LRU tile cache with a memory budget and content-hash persistence becomes
  required infrastructure, not an optimisation.
- The eager `SpectrogramPyramid::build` shipped today is correct and is what the
  tests exercise, but must not be called on a long file from the UI.
- `SpectrogramPyramid::render` stays for tests, thumbnails and headless work,
  documented as not-for-the-UI. Keeping a CPU reference path is also what lets
  the GPU path be verified against something.
- The shader must handle log/mel/Bark frequency scaling, since the CPU no longer
  reshapes the data.
- Zooming in past level 0 cannot be answered from the pyramid; the renderer must
  fall back to analysing at a finer hop, mirroring `PeakPyramid::shouldReadSource`.

## Revisit if

Real GPU measurement across Intel, AMD and NVIDIA contradicts the upload-cost
model — still unverified, as no GPU was available for the spike.
