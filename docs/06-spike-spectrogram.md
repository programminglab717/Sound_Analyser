# 06 — Spike: spectrogram at 60 fps

**Status:** Complete · 2026-09-18 · **Thesis survives, with three design constraints confirmed.**

---

## The question

The product thesis is that *the spectrogram is the editing surface*
([01 — Product brief](01-product-brief.md) §2). That collapses if scrolling
stutters. The roadmap therefore put this in Phase 0 rather than Phase 2: if it
fails, we want to know before building on it.

> Can we pan and zoom a long spectrogram at 60 fps — a 16.67 ms frame budget?

## Method

Built `sa-dsp` (FFT, windows, STFT) and `sa-spectral` (`SpectrogramPyramid`),
then measured the two costs that decide the answer:

- **Build** — analysing audio that is not cached yet. Sets how far ahead of a
  fast pan we can stay.
- **Render** — the per-frame cost while panning over cached data.

Benchmark: `benchmarks/SpectrogramSpike.cpp`, run via
`cmake --build build/release --target sa-spectrogram-spike`.
60 s of 48 kHz mono broadband material with transients, fftSize 2048, hop 512,
1025 bins. Medians of repeated runs.

## Results

```
BUILD (cache miss)
  build 60 s of audio                             262 ms
  throughput                                      229x realtime
  memory for 60 s                                11.0 MB
  audio analysable per frame budget               3.8 s

RENDER (cache hit — per-frame cost while panning)
  1920x1080, whole 60 s (zoomed fully out)      11.59 ms   PASS
  1920x1080, 10 s window                         9.40 ms   PASS
  1920x1080, 1 s window                          8.90 ms   PASS
  1920x1080, 100 ms window                       8.59 ms   PASS
  3840x2160 (4K), whole 60 s                    47.58 ms   FAIL

GPU PATH (upload level tile as texture; shader scales)
  upload for 1920 columns (2814 frames)          0.21 ms   PASS
  upload for 3840 columns (5628 frames)          0.43 ms   PASS

WAVEFORM (peak pyramid, for comparison)
  build 60 s                                        4 ms
  query 1920 columns, whole 60 s                 0.04 ms   PASS
```

## Findings

### 1. STFT throughput is not the bottleneck ✅

229x realtime means **3.8 seconds of audio analysed per frame budget**. Even a
pan crossing several screen-widths per second stays comfortably ahead of the
playhead. This is the number the thesis actually depended on, and it has
roughly two orders of magnitude of headroom.

Conservative, too: single-threaded, on a shared container CPU, using our own
radix-2 FFT. PFFFT would add 2–3x, and the work is embarrassingly parallel
across frames.

### 2. Rendering must be a shader — now with a number ⚠️

CPU scaling to screen resolution costs **11.59 ms at 1080p** and **47.58 ms at
4K**. The cost tracks *pixel count*, not audio length, so it gets worse as
monitors get better — exactly the wrong direction.

Uploading the level tile and letting a shader scale costs **0.21 ms**. That is
**55x faster**, and flat in screen resolution.

[03 — Architecture](03-architecture.md) §8 already asserted "rendering is a
shader, not a loop." This is the evidence. `render()` stays in the codebase for
tests, thumbnails and headless work, and is documented as not-for-the-UI.

### 3. The pyramid must be lazy ⚠️

An eager full-file pyramid needs **1.3 GB for one hour of stereo**. Not viable.

Production must generate tiles lazily for the visible region plus a margin,
with LRU eviction under a memory budget, persisted between sessions keyed by
content hash — as [03 — Architecture](03-architecture.md) §4 describes. The
eager `build()` shipped here is correct and is what the tests exercise; it is
not what the UI should call on a long file.

## Design decisions confirmed

| Decision | Evidence |
| --- | --- |
| Spectrogram gets its own pyramid, like the waveform | Whole-file views would otherwise need every fine frame |
| Upper levels **max-combine**, never average | A one-frame transient survives to every level — tested explicitly |
| Magnitudes quantised to 8-bit log-magnitude | 0.47 dB per step over 120 dB, finer than a display resolves, a third the memory of 16-bit |
| Colour mapping happens in the shader | Contrast and palette changes cost no recomputation |
| STFT runs **once**; levels fold from the level below | Avoids re-analysing per zoom level |

## What this spike did *not* prove

- **No GPU was measured.** There is no display or GPU in this environment. What
  is measured is the CPU side of the GPU path — preparing the texture upload.
  The shader itself still needs verifying on real hardware across Intel, AMD
  and NVIDIA, which is Phase 0's remaining risk.
- **Lazy tile generation with LRU is designed, not built.** The eager pyramid is
  what exists today.
- **Numbers are from a shared container CPU**, single-threaded. Treat them as a
  conservative floor, not a spec.

## Verdict

**The thesis survives.** Analysis throughput has ~100x headroom, which was the
open question. The two problems found are both architectural and both already
anticipated in the architecture doc — this spike converts them from assertions
into measured facts, before any UI code depends on the wrong answer.

Remaining Phase 0 risk is the GPU renderer itself, which needs hardware.
