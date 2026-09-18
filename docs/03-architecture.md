# 03 — Architecture

**Stack:** C++20 · Qt 6 (LGPL) · miniaudio · CLAP · CMake · vcpkg · ONNX Runtime ·
Windows-first, portable-by-construction.

> Every dependency is free in perpetuity for closed-source distribution with no
> revenue cap — see [ADR 0006](adr/0006-permissive-only-dependencies.md).

---

## 1. Principles

1. **The audio thread is sacred.** No allocation, no locks, no I/O, no
   exceptions, ever. This single rule is why the stack is C++ and not C#.
2. **Nothing is loaded whole.** A two-hour 96 kHz/32-bit stereo file is ~5.5 GB.
   Everything streams, everything is chunked, everything is cancellable.
3. **Draw from caches, never from files.** Pan and zoom must never touch a
   decoder. This is the difference between our spectrogram and Audacity's.
4. **DSP is a library, not an app feature.** Every processor is headlessly
   testable with golden-file references, with no UI-framework dependency.
5. **Correctness is provable.** Measurement code is validated against published
   standards test vectors in CI, not by ear.
6. **Third-party code cannot crash us.** Plugins and file parsers are hostile
   until proven otherwise.

## 2. Module map

Each module is a separate static library with its own tests. Dependencies point
downward only; `sa-dsp` and `sa-analysis` must never include a UI header. That
rule is what keeps the UI toolkit a replaceable decision rather than a rewrite.

`sa-analysis` sits **above** `sa-dsp` rather than beside it: filters are a
primitive, and measuring with them is built on top. That was settled when both
modules independently grew an identical `BiquadCoefficients`. Two structurally
identical types meaning the same thing are worse than a dependency, because
every boundary between them needs a conversion shim that exists only to launder
a name.

```
┌──────────────────────────────────────────────────────────────┐
│ sa-app        shell, entitlements, updater, crash reporting   │
├──────────────────────────────────────────────────────────────┤
│ sa-ui         Qt 6 shell · OpenGL spectrogram renderer        │
├──────────────────────────────────────────────────────────────┤
│ sa-engine     document model · EDL · undo · render scheduler  │
├───────────────────────────┬──────────────────────────────────┤
│ sa-spectral               │ sa-host   CLAP, out-of-process    │
│ STFT/ISTFT · layers       │                                   │
├───────────────┬───────────┴───────────┬──────────────────────┤
│ sa-dsp        │ sa-analysis           │ sa-ml                 │
│ filters, FFT, │ loudness, acoustics,  │ ONNX Runtime,         │
│ dynamics,     │ MIR, forensics        │ EP selection,         │
│ time/pitch    │                       │ model registry        │
├───────────────┴───────────────────────┴──────────────────────┤
│ sa-io         codecs · streaming reader · peak & spec caches  │
├──────────────────────────────────────────────────────────────┤
│ sa-core       buffers · channel layouts · time units · errors │
└──────────────────────────────────────────────────────────────┘
                 sa-cli  ──▶ links sa-engine and below (no UI)
```

`sa-cli` existing from early on is a forcing function: if the engine can be
driven headlessly, the UI is genuinely decoupled, and batch/scripting in P5
becomes almost free.

## 3. Threading model

Four classes of thread, with strict rules about what crosses between them.

| Thread | Priority | Rules |
| --- | --- | --- |
| **Audio callback** | Real-time | No malloc, no lock, no I/O, no exceptions. Reads from lock-free ring buffers only. |
| **Disk/streamer** | High | Decodes ahead of playhead into ring buffers. Never blocks the audio thread. |
| **Worker pool** | Normal | Work-stealing pool, `hardware_concurrency - 1`. All analysis, rendering, cache building. Every job cancellable and progress-reporting. |
| **UI** | Normal | Qt main thread. Never blocks on a job; subscribes to results. |

**Crossing rules**
- Audio → UI: lock-free SPSC queue of POD messages (levels, playhead, events).
- UI → Audio: a command queue of pre-allocated, pre-validated parameter updates.
  Parameter objects are constructed on a worker thread and passed by pointer;
  the audio thread only swaps pointers, never builds anything.
- Anything the audio thread stops referencing is handed to a **deferred deleter**
  thread. Destructors never run on the audio thread.

**Enforcement, not intention.** Debug builds install a global allocation hook
that asserts if `operator new` is reached from the audio thread. Without an
automated check, this discipline decays within weeks.

## 4. Data model

### Sample buffers
Planar (non-interleaved) `float32` internally, 32-byte aligned for AVX2.
Interleaving happens only at device and codec boundaries. `float64` is available
for analysis accumulators where precision matters (loudness integration, long
FFT sums) but is not the transport format.

### Document model — non-destructive by default

```
Document
 └── Timeline
      ├── Source[]        immutable, content-hashed decoded media
      ├── Clip[]          { source, sourceRange, timelinePos, gainEnvelope, fades }
      ├── ProcessNode[]   ordered, non-destructive, per-clip or per-range
      ├── SpectralLayer[] time-frequency masks + their own process chains
      └── Marker/Region[]
```

An edit appends to the EDL; it does not rewrite audio. Consequences:

- Undo is O(1) and costs no disk.
- Sessions reopen with every decision still adjustable.
- "Destructive" operations are a `flatten()` that renders a range and replaces
  it with a new Source — so the destructive workflow people expect still exists,
  it is just implemented on top of the non-destructive one.

Render results are cached per `(clipId, nodeChainHash, range)`, so changing node
3 of 5 only re-renders from node 3 onward.

### The cache layer — *the performance story*

Two caches, both keyed by content hash, both in a sidecar directory, both
disposable:

**Peak pyramid.** Multi-resolution min/max/RMS triplets, each level 1/2 the
resolution of the last, from 1:256 down to whole-file. Drawing any zoom level at
any width is a bounded read from the nearest level. Built on import in the
background, with the visible region prioritised — the waveform appears
progressively rather than after a freeze.

**Spectrogram tile cache.** STFT magnitudes as 2-D tiles (e.g. 512 frames ×
512 bins), stored quantised to 8- or 16-bit log-magnitude, compressed. Tiles are
computed lazily for the visible viewport plus a margin, evicted by LRU, and
persisted between sessions.

This is what makes a two-hour file scroll at 60 fps. It is also the single
highest-risk piece of engineering in the project, which is why it starts in
Phase 0 rather than Phase 2.

### Out-of-core access
Sources are memory-mapped where the format allows direct addressing (WAV,
uncompressed AIFF/CAF); compressed formats are decoded to a temporary cache file
on import and then memory-mapped. An LRU page cache bounds resident memory
regardless of file size. **We must never require the file to fit in RAM.**

## 5. Spectral engine

The heart of the differentiator, and the hardest thing to get right.

- **Analysis/synthesis:** windowed STFT with constant-overlap-add. Default Hann,
  75% overlap, satisfying the COLA condition so that an untouched round-trip is
  bit-transparent within float epsilon. **This round-trip is a CI test** — if
  reconstruction error rises, we have introduced an artefact.
- **Editing operates on a mask**, not on the audio. A spectral edit produces a
  complex gain mask over the T/F plane; masks compose, so multiple edits stay
  independently adjustable rather than compounding artefacts.
- **Phase is the hard part.** Naive magnitude editing with retained phase
  produces the metallic "spectral smear" that makes amateur tools sound amateur.
  Mitigations: phase-locked vocoder for time/pitch, phase-gradient heap
  integration for inpainting, and preferring *attenuation* over *deletion*
  wherever the UI can steer users toward it.
- **Layers** are masks plus their own process chain. Extracting a sound to a
  layer is `layer = mask ⊙ original`, `residual = (1 − mask) ⊙ original`, so the
  sum always reconstructs the original exactly. That invariant is what makes
  layer editing safe.

## 6. Plugin hosting — out of process

Third-party plugins crash. A plugin crash must never destroy unsaved
work.

```
Editor process  ──shared memory (audio) ──▶  sa-plughost.exe (one per vendor)
                ──named pipe (control)  ──▶  [CLAP plugin instance]
```

**CLAP first, VST3 later.** CLAP is MIT with nothing to sign and a cleaner API.
The VST3 SDK is free but requires a signed Steinberg agreement, so it is deferred
until users ask for it.

The host process is restartable and its state is reconstructable. This costs
roughly two weeks of extra engineering and is the clearest single signal that
separates a professional tool from a hobby one. Latency cost is one buffer of
round-trip, which is acceptable for offline and tolerable for monitoring.

## 7. ML runtime

```
sa-ml
 ├── ModelRegistry     manifest: id, version, SHA-256, size, licence, EP hints
 ├── ModelDownloader   on-demand fetch, hash-verified, resumable, cached
 ├── SessionPool       warm ORT sessions, bounded by memory budget
 └── ExecutionPlanner  EP preference chain + graceful degradation
```

**Execution provider chain:** Windows ML / DirectML (GPU + NPU) → CUDA where an
NVIDIA GPU is present → CPU with AVX2. Note that the DirectML execution provider
is now in sustained engineering and Windows ML is Microsoft's forward path for
GPU/NPU inference on Windows — so the EP choice must sit behind our own
abstraction, never be hard-coded at call sites.

**Models are downloaded, not bundled.** Three reasons, in order of importance:

1. Licensing agility — a model whose terms change can be swapped without
   shipping a new binary.
2. Installer size — bundling would add hundreds of megabytes for features many
   users never touch.
3. Model updates ship independently of app releases.

Every model must also have a **CPU fallback path with a stated time budget**.
A feature that only works on an RTX card is a feature most of our audience does
not have.

## 8. Rendering

Spectrogram rendering is a shader, not a loop. Magnitude tiles upload as
single-channel textures; a fragment shader applies log scaling, dynamic range
windowing and the colourmap. Consequences: colourmap and contrast changes are
free (no recompute), and zoom interpolation happens in hardware.

`QOpenGLWidget` is the baseline for reach. Direct2D/D3D11 is a possible
optimisation later; it should not be a Phase-0 dependency.

## 9. Testing strategy

| Layer | Method |
| --- | --- |
| DSP primitives | Golden-file: input WAV → expected output within tolerance |
| Spectral engine | Round-trip reconstruction error bounded; regression-tested |
| Loudness & metering | **Conformance against EBU R128 / ITU-R BS.1770 published test vectors** |
| Band filters | IEC 61260 tolerance masks |
| Codec I/O | Property-based round-trip: encode → decode → compare |
| Real-time safety | Automated allocation/lock detection on the audio thread |
| Performance | Benchmark suite in CI with hard regression thresholds |
| File parsers | **Continuous fuzzing** — see §10 |
| Long-run | 24-hour playback soak; 10 GB file handling |
| UI | Snapshot tests for the spectrogram renderer |

Standards conformance is the one that matters most commercially. "Our LUFS
matches the EBU reference set" is a claim competitors in the free tier cannot
make, and it is the foundation of the Measurer persona.

## 10. Security

Audio file parsers are a genuine attack surface — libsndfile and FFmpeg carry a
long history of CVEs, and our users open files sent to them by strangers.

- Fuzz every parser continuously (libFuzzer / OSS-Fuzz-style corpus).
- Build with `/guard:cf`, ASLR, DEP, CFG; run ASan/UBSan builds in CI.
- Consider decoding untrusted formats in a low-privilege child process,
  reusing the `sa-plughost` sandbox mechanism.
- Keep a dependency SBOM and subscribe to upstream CVE feeds; a pinned old
  FFmpeg is a liability, not stability.
- Model downloads are hash-verified over TLS with pinned roots.

## 11. Build & tooling

- **CMake** ≥ 3.26, presets checked in, Ninja generator.
- **vcpkg** in manifest mode for reproducible third-party pinning.
- **MSVC** as primary; also build with clang-cl in CI to catch latent UB.
- **CI:** Windows runners — build, unit, conformance, benchmark, ASan. Target
  under 15 minutes for the fast lane.
- **Licence gate:** CI **fails the build** on any dependency whose licence is not
  on the allowlist, and on any static link of an LGPL library. Policy enforced by
  the build, not by review.
- **Distribution:** Inno Setup or WiX installer. Code signing has no free option —
  unsigned through beta, then Azure Trusted Signing (~$10/mo) before 1.0. See
  [05 — Licensing](05-licensing-and-dependencies.md) §3.

## 12. Deliberate non-decisions

Recorded so that we revisit them on evidence rather than drift into them:

- **Direct2D vs OpenGL** — start OpenGL, measure, revisit if it limits us.
- **Lua vs Python for scripting** — Lua embeds cleanly with no runtime
  dependency; Python has the ecosystem the analysis audience already knows.
  Decide at Phase 5 with user input.
- **Session file format** — JSON while the schema churns, a binary format later
  if load times demand it. Do not optimise this early.
- **macOS port** — Qt and miniaudio both port cleanly, but "cheap" is not "free".
  Not before 1.0 ships on Windows; revisit for 1.x.
- **UI toolkit** — Qt 6 LGPL chosen over Dear ImGui for text handling and
  accessibility. Revisit only if the LGPL obligations prove burdensome.
