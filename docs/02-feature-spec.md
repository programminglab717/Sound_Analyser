# 02 — Feature Specification

Phase tags map to [04 — Roadmap](04-roadmap.md).
**P0** = foundations · **P1** = editor · **P2** = analyser · **P3** = spectral repair ·
**P4** = ML · **P5** = pro & polish · **v2** = after 1.0

Tier tags reflect the Stage-2 freemium split: **[F]** free forever, **[Pro]** paid.
Everything is free during Stage 1.

---

## A. Core editing

| Feature | Phase | Tier | Notes |
| --- | --- | --- | --- |
| Non-destructive edit model, unlimited undo | P1 | F | EDL over immutable sources; destructive ops are "flatten" |
| Visual undo history timeline | P1 | F | Branching history, click any point to return |
| Waveform view, sample-accurate zoom | P0 | F | Down to individual samples with interpolated draw |
| Dual waveform + spectrogram view | P2 | F | Linked scroll/zoom, adjustable split |
| Cut / copy / paste / trim / split / delete | P1 | F | |
| Fades, crossfades, fade shape editor | P1 | F | Linear, log, exp, S-curve, custom Bézier |
| Clip gain and gain envelopes | P1 | F | Drawable automation over time |
| Markers, ranges, named regions, region list | P1 | F | Export regions as separate files |
| Pencil / sample redraw tool | P1 | F | Manual click repair |
| Silence, reverse, invert, trim-to-selection | P1 | F | |
| Channel operations (split, merge, swap, M/S) | P1 | F | |
| Multichannel: mono, stereo, 5.1, 7.1 | P1 | F | Arbitrary channel counts in the buffer model from day one |
| Ambisonics (1st–3rd order) | v2 | Pro | |
| Sample rates to 384 kHz; 16/24/32-int, 32/64-float | P0 | F | |
| Multitrack session | v2 | Pro | Deliberately deferred — see brief §3 |
| Video track for post work (reference only) | v2 | Pro | Show picture, edit audio against it |

## B. Spectral editing — *the differentiator*

| Feature | Phase | Tier | Notes |
| --- | --- | --- | --- |
| GPU spectrogram canvas | P2 | F | Configurable FFT size, window, overlap |
| Linear / log / Mel / Bark / ERB frequency scales | P2 | F | |
| Constant-Q (CQT) view | P3 | F | Musically meaningful — constant resolution per octave |
| Colourmap library incl. colourblind-safe | P2 | F | Viridis/magma defaults; never rainbow-only |
| Time–frequency selection: rectangle, lasso, brush | P3 | F | Select a sound, not a time range |
| Magic wand (contiguous-energy selection) | P3 | F | Flood-fill in the T/F plane above a threshold |
| Harmonic selection | P3 | F | Click a partial → select the whole harmonic stack |
| Frequency-band selection across full duration | P3 | F | |
| Spectral attenuate / gain / mute | P3 | F | |
| Spectral **heal** (inpaint from surroundings) | P3 | F | The headline repair gesture |
| Spectral copy / paste / move in time *and* pitch | P3 | Pro | |
| Spectral layers (Photoshop-style, per-layer edit) | P3 | Pro | Separate a sound onto its own layer, process independently |
| Layer blend modes and solo/mute | P3 | Pro | |
| Real-time resynthesis preview + instant A/B | P3 | F | Non-negotiable for trust in destructive-looking ops |

## C. Restoration & repair — *the money features*

| Feature | Phase | Tier | Notes |
| --- | --- | --- | --- |
| Noise profile capture + spectral subtraction | P3 | F | With MCRA/IMCRA adaptive noise estimation |
| De-hum / de-buzz | P3 | F | 50/60 Hz + harmonics, drift-tracking notch bank |
| De-click / de-crackle | P3 | F | Vinyl, digital clicks, packet dropouts |
| De-clip (clipped-sample reconstruction) | P3 | F | AR/sparse reconstruction of flat tops |
| Dropout / gap repair | P3 | F | Interpolate missing audio |
| De-ess (sibilance) | P3 | F | |
| De-plosive | P3 | F | Low-frequency pop removal |
| Hiss removal | P3 | F | |
| Mouth de-click, breath control | P3 | Pro | |
| Wind / handling noise reduction | P3 | Pro | |
| De-reverb (classical) | P3 | Pro | |
| **ML de-noise / speech enhancement** | P4 | Pro | |
| **ML de-reverb** | P4 | Pro | |
| Room-tone generate & fill | P3 | Pro | Synthesise matching ambience for gaps |
| Ambience match between takes | P4 | Pro | |
| Wow & flutter correction | v2 | Pro | Tape/vinyl pitch-drift — strong differentiator, nobody free does it |
| Azimuth / phase correction for tape transfers | v2 | Pro | |
| Dialogue auto-levelling | P4 | Pro | |

## D. Analysis & measurement — *the "analyser" half*

### Metering
| Feature | Phase | Tier |
| --- | --- | --- |
| Real-time & offline FFT analyser (window, averaging, peak-hold) | P2 | F |
| Loudness: integrated / short-term / momentary LUFS, LRA | P2 | F |
| True Peak (oversampled dBTP), ITU-R BS.1770-4 / EBU R128 | P2 | F |
| Platform compliance targets with pass/fail + one-click conform | P2 | F |
| Peak, RMS, crest factor, DC offset, PLR / PSR, DR | P2 | F |
| Phase correlation meter, goniometer / vectorscope | P2 | F |
| Stereo width & mono-compatibility check | P2 | F |
| Mid/Side analysis | P2 | F |
| Loudness-over-time graph with target overlay | P2 | F |

Compliance presets to ship: Spotify −14, Apple Music −16, YouTube −14,
Amazon Music −14, Tidal −14, podcast −16, EBU R128 −23, ATSC A/85 −24,
Netflix −27 dialog-gated, AES streaming recommendation.

### Acoustic & electrical measurement
| Feature | Phase | Tier |
| --- | --- | --- |
| Octave / third-octave band analysis (IEC 61260) | P2 | F |
| Calibrated SPL metering (A/C/Z weighting, fast/slow/impulse) | P2 | Pro |
| Microphone calibration workflow | P2 | Pro |
| Impulse response capture (sine sweep + deconvolution) | P2 | Pro |
| RT60, EDT, C50, C80, D50, STI | P2 | Pro |
| THD, THD+N, IMD, frequency response sweeps | P2 | Pro |
| Waterfall / cumulative spectral decay plot | v2 | Pro |
| PDF / CSV measurement reports with branding | P2 | Pro |

### Content analysis (MIR)
| Feature | Phase | Tier |
| --- | --- | --- |
| Pitch detection & F0 contour (pYIN / CREPE) | P2 | F |
| Tempo / BPM detection, beat grid, onset detection | P2 | F |
| Key & scale detection | P2 | F |
| Chromagram, MFCC, spectral centroid/rolloff/flux/flatness | P2 | F |
| Silence detection & VAD | P2 | F |
| Music / speech discrimination | P4 | F |

### Forensics & provenance — *a genuinely loved niche*
| Feature | Phase | Tier |
| --- | --- | --- |
| Lossy-codec cutoff detection ("is this really lossless?") | P2 | F |
| True bit-depth detection ("is this really 24-bit?") | P2 | F |
| Upsampling / fake hi-res detection | P2 | F |
| Re-encode / generation-loss detection | v2 | Pro |
| A/B null test (invert & sum two files) | P2 | F |
| Clipping & inter-sample peak locator | P2 | F |
| Edit-point / splice detection | v2 | Pro |

### Batch & reporting
| Feature | Phase | Tier |
| --- | --- | --- |
| Batch analysis across a folder → CSV / JSON | P2 | Pro |
| Scheduled / watch-folder analysis | P5 | Pro |
| Comparison view across many files | P2 | Pro |

## E. AI / ML

| Feature | Phase | Tier | Licensing risk |
| --- | --- | --- | --- |
| Speech transcription, word-level timestamps (Whisper) | P4 | F | **Clear** — MIT |
| Transcript-driven editing (edit text → edits audio) | P4 | Pro | Clear |
| Speaker diarisation | P4 | Pro | Verify per model |
| Filler-word / silence auto-removal | P4 | Pro | Clear |
| Voice activity detection (Silero) | P4 | F | **Clear** — MIT |
| ML de-noise (RNNoise / DeepFilterNet class) | P4 | Pro | Mostly clear — verify |
| Audio event classification & tagging | P4 | Pro | Mostly clear (YAMNet/PANNs) |
| "Find sounds like this" similarity search | v2 | Pro | Clear (own embeddings) |
| Auto-chaptering / topic segmentation | P4 | Pro | Clear |
| **Stem separation (vocals/drums/bass/other)** | P4 | Pro | **BLOCKED** — see below |
| Bandwidth extension (restore lost highs) | v2 | Pro | Verify |

> ### ⚠ Stem separation is licence-blocked
> Demucs / htdemucs **weights** are released for scientific use only — the MIT
> licence covers the *code*, not the weights, and converting to ONNX does not
> change that. Open-Unmix UMXL is CC BY-NC-SA. Most strong models are trained on
> MUSDB18-HQ, which is itself non-commercial.
> **Open-sourcing our app does not fix this** — these are *use* restrictions, not
> copyleft. Full analysis and the three viable paths are in
> [05 — Licensing](05-licensing-and-dependencies.md) §4.

## F. Processing & effects

| Feature | Phase | Tier | Notes |
| --- | --- | --- | --- |
| Parametric EQ + analyser overlay | P1 | F | |
| Linear-phase EQ | P2 | F | Essential for mastering without phase smear |
| Dynamic EQ | P3 | Pro | |
| Match EQ (fingerprint a reference, match to it) | P3 | Pro | High-demand feature |
| Compressor, limiter (true-peak), gate, expander | P1 | F | |
| Multiband compressor, transient shaper | P3 | Pro | |
| Time stretch & pitch shift, formant-preserving | P2 | F | See licensing note on Rubber Band |
| Pitch correction / pitch contour editing | P3 | Pro | |
| Varispeed (tape-style, coupled pitch+time) | P1 | F | |
| Convolution reverb (use captured IRs) | P3 | Pro | Pairs with IR capture |
| Algorithmic reverb, delay, chorus, flanger, phaser | P3 | F | |
| Saturation / harmonic exciter, tape emulation | P3 | Pro | |
| Stereo imaging, M/S processing | P2 | F | |
| High-quality resampling (SRC) | P0 | F | |
| Dither with noise shaping (TPDF, POW-r-class) | P1 | F | Correct dither on export is a mark of seriousness |
| Loudness normalisation to target | P2 | F | |
| **VST3 / CLAP plugin hosting** | P5 | Pro | Out-of-process — see architecture §6 |

## G. I/O, formats, workflow

### Import
WAV, BWF, RF64, W64, AIFF/AIFC, FLAC, MP3, AAC/M4A, ALAC, Ogg Vorbis, Opus,
WMA, CAF, AU, raw/headerless PCM, DSD (DFF/DSF) — **P0–P1**.
Audio extraction from MP4, MOV, MKV, AVI, WebM — **P1**.

### Export
All of the above plus per-platform loudness-normalised presets — **P1**.

### Metadata
ID3v1/v2, BWF `bext` chunk, iXML, RIFF INFO, Vorbis comments, embedded
timecode, cue/marker chunks, cover art — **P1**.

### Workflow
| Feature | Phase | Tier |
| --- | --- | --- |
| Session files, autosave, crash recovery | P1 | F |
| Recording: multichannel, punch-in, timed, pre-record buffer | P1 | F |
| ASIO, WASAPI shared + exclusive, WDM | P0 | F |
| Low-latency monitoring with live effect preview | P1 | F |
| Batch processor with saved chains & presets | P5 | Pro |
| Watch folders | P5 | Pro |
| Scripting (Lua) + headless CLI | P5 | Pro |
| Preset system, workspace layouts | P1 | F |
| **Keymap compatibility packs** (Audition, RX, Audacity, Pro Tools) | P5 | F |

> Keymap packs are an underrated adoption lever: the single largest switching
> cost for a professional is muscle memory, and this removes it for near-zero
> engineering effort.

## H. Platform & UX

| Feature | Phase | Tier |
| --- | --- | --- |
| Dark & light themes, high-DPI, multi-monitor | P0 | F |
| GPU-accelerated rendering throughout | P0 | F |
| Accessibility: screen reader, full keyboard operation | P5 | F |
| Colourblind-safe spectrogram palettes | P2 | F |
| Localisation infrastructure | P5 | F |
| Opt-in telemetry with visible field list | P5 | F |
| Consent-gated crash reporting (never includes audio) | P5 | F |
| Auto-update with staged rollout | P5 | F |
| Signed installer (EV certificate) | P5 | F |

## Explicitly out of scope for 1.0

MIDI · virtual instruments · notation · mixing console · arrangement timeline ·
CD burning · cloud collaboration · mobile · streaming/broadcast output ·
audio-to-MIDI transcription · generative audio.

Each is defensible later. None is defensible *now* — they trade the thing that
makes us distinctive for breadth we cannot win on.
