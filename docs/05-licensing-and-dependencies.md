# 05 — Licensing & Dependencies

> **Not legal advice.** This is engineering analysis of publicly stated licence
> terms, current as of September 2026. Licence terms change — JUCE has revised
> its terms twice in recent years. **Have a lawyer review the dependency set
> before 1.0**, and re-verify every row below at that time.

**Our constraint:** closed-source, free at launch, freemium later. That means we
need terms permitting **proprietary distribution**, including once we start
charging.

---

## 1. Should we open-source instead? — **No.**

The question was raised because of licensing friction. It is worth answering
precisely, because the intuition is reasonable but the conclusion is wrong.

### What open-sourcing would fix
| Problem | Fixed by going open source? |
| --- | --- |
| FFTW is GPL | ✅ Yes — but BSD alternatives are already fine |
| Rubber Band is GPL/commercial | ✅ Yes — but SoundTouch (LGPL) is usable, or we build our own |
| FFmpeg GPL-only components | ✅ Yes — but we need almost none of them for audio |

### What open-sourcing would **not** fix
| Problem | Fixed? | Why not |
| --- | --- | --- |
| Demucs weights are research-only | ❌ **No** | A *use* restriction on the weights. Our source licence is irrelevant to it. |
| Open-Unmix UMXL is CC BY-NC-SA | ❌ **No** | Non-**commercial**, not non-proprietary. Orthogonal to our source licence. |
| MUSDB18-HQ-derived weights | ❌ **No** | Same — the dataset's NC term follows the weights, not the code. |

**This is the crux.** The blocking licensing problem in this product is ML model
weights, and weight licences restrict *what you may use them for*, not *whether
you publish your source*. Open-sourcing the application changes nothing about
them.

### What open-sourcing would break
JUCE's free tier is **AGPLv3**. Going open source means the whole application
becomes AGPLv3, and then:

- Anyone may take the source, strip the entitlement checks, recompile and
  redistribute. **The freemium model becomes unenforceable.**
- AGPL's network clause creates obligations if we ever add a hosted component.
- We lose the ability to license third-party proprietary components at all.

### Conclusion
Open-sourcing solves two problems we can already solve by swapping libraries,
fixes none of the problems that actually block us, and destroys the business
model. **Stay closed-source.** Every remaining issue below has a clean
closed-source path.

---

## 2. Framework

### JUCE — **use the free Starter tier**

JUCE is dual-licensed: **AGPLv3**, or a commercial licence in Starter / Indie /
Pro tiers.

| Tier | Cost | Closed-source | Revenue/funding limit |
| --- | --- | --- | --- |
| AGPLv3 | Free | ❌ Must open-source | None |
| **Starter** | **Free** | **✅ Permitted** | **~$20k/yr** |
| Indie | Paid | ✅ | Higher |
| Pro | Paid (≈$50/mo or ≈$1000 perpetual) | ✅ | None |

This fits our plan almost perfectly:

- **Stage 1 (free product, $0 revenue):** Starter tier, costs nothing, permits
  closed source. Since JUCE 8 there is **no splash-screen requirement** on the
  free tier.
- **Stage 2 (freemium):** upgrade to Indie or Pro once revenue approaches the
  cap. At that point it is a rounding error against revenue.

**Two traps to watch.**

1. The revenue limit counts *all* revenue and funding derived from use of the
   framework — **including donations, sponsorship and advertising**, not just
   product sales. A successful donation drive could breach the cap while the
   product is still nominally free.
2. The licence must be maintained **for as long as you distribute closed-source
   binaries containing JUCE**, not merely while developing.

> **Action:** JUCE's master `LICENSE.md` now references a **JUCE 9** EULA, so
> terms have moved again recently. `juce.com` is unreachable from this
> environment — verify the current Starter tier terms and cap directly at
> <https://juce.com/get-juce/> before writing the first line of code.

---

## 3. Libraries

✅ = safe for closed-source proprietary distribution.

| Dependency | Licence | OK? | Notes |
| --- | --- | --- | --- |
| **FFmpeg** | LGPL-2.1+ *(GPL if `--enable-gpl`)* | ✅ | **Never** build with `--enable-gpl` or `--enable-nonfree`. Link **dynamically**, ship as DLLs, honour LGPL relink rights. |
| libsndfile | LGPL-2.1 | ✅ | Dynamic link. |
| FLAC, Ogg, Vorbis, Opus | BSD | ✅ | |
| LAME (MP3 encode) | LGPL | ✅ | Dynamic link. MP3 patents have expired — no royalty exposure. |
| **AAC encoding** | — | ✅ | Use **Windows Media Foundation**'s built-in AAC encoder. Sidesteps FDK-AAC licensing entirely and ships with the OS. |
| **FFTW** | **GPL** or paid commercial | ❌ | **Common trap.** Use **PFFFT** (BSD-like) or **pocketfft** (BSD-3) instead. Both are fast enough. |
| Intel IPP / oneMKL | Intel proprietary (free redistribution) | ⚠️ | Fast, but adds a large redistributable and vendor lock-in. Optional accelerator, never the baseline. |
| libsamplerate | BSD-2 *(since v2.0)* | ✅ | Relicensed from GPL in 2021 — **ensure ≥ 2.0**. |
| r8brain-free-src | MIT | ✅ | Excellent quality; recommended default resampler. |
| **Rubber Band** | **GPL or paid commercial** | ⚠️ | Best-in-class time/pitch. Either buy the commercial licence, or use **SoundTouch** (LGPL), or build our own phase-locked vocoder + WSOLA. Budget the decision in Phase 2. |
| ONNX Runtime | MIT | ✅ | |
| Crashpad | Apache-2.0 | ✅ | |
| Catch2 / GoogleTest | BSL-1.0 / BSD-3 | ✅ | |
| **VST3 SDK** | GPLv3 **or** Steinberg proprietary | ✅ | Steinberg's proprietary option is free but requires signing their agreement. Do this before Phase 5. |
| **CLAP** | MIT | ✅ | Genuinely permissive. Prefer it; support VST3 for compatibility. |
| WiX / Inno Setup | MS-RL / custom permissive | ✅ | |

---

## 4. ML model weights — **the real landmine**

> **The rule:** a model's *code* licence and its *weights* licence are different
> documents. MIT code with research-only weights is common, and shipping the
> weights anyway is the single most likely way for this project to acquire a
> legal problem.

| Model | Code | Weights | Commercial? |
| --- | --- | --- | --- |
| **Demucs / htdemucs** | MIT | **Research/scientific use only (Meta)** | ❌ **No.** Converting to ONNX/CoreML does not change the weights' licence. |
| Open-Unmix **UMXL** | MIT | CC BY-NC-SA 4.0 | ❌ No — non-commercial. |
| Open-Unmix UMX / UMXHQ | MIT | Ambiguous; trained on MUSDB18-HQ (itself NC) | ⚠️ Needs legal review. Do not assume. |
| Spleeter | MIT | Reported MIT | ⚠️ Likely yes, quality is lower — verify in writing. |
| **Whisper** | MIT | **MIT** | ✅ **Yes** — safe. Our transcription foundation. |
| **Silero VAD** | MIT | MIT | ✅ Yes. |
| **RNNoise** | BSD | BSD | ✅ Yes — solid baseline denoiser. |
| DeepFilterNet | Dual — verify | Verify | ⚠️ Strong denoiser; confirm terms per release. |
| YAMNet / PANNs | Apache-2.0 / MIT | Verify per checkpoint | ⚠️ Mostly permissive. |

### Stem separation: three paths

1. **Ship without it.** Launch with transcription, VAD, ML de-noise and event
   tagging — all with clean licences. Stem separation arrives when resolved.
   *Recommended for v1.*
2. **License commercially.** Negotiate with Meta, or integrate a vendor
   (Audioshake, Music.AI/Moises, LALAL.AI). Fast, but introduces a per-user cost
   and — if it is an API — breaks the on-device privacy promise. If we take this
   route it must be an explicitly-opt-in, clearly-labelled cloud feature.
3. **Train our own** on a properly licensed or owned multitrack corpus.
   Expensive in data acquisition and compute, but it produces the only durable
   moat of the three: weights we own outright and can licence as we please.

**Recommendation:** path 1 for v1, begin path 3 as a background research track
during Phase 4, keep path 2 as the commercial fallback.

### Policy to adopt now
- **No model ships without a written licence determination** recorded in the
  model registry manifest (see [03 — Architecture](03-architecture.md) §7).
- The manifest carries a `licence` field, and CI fails on any model whose field
  is absent or marked `unverified`.
- Because models are downloaded rather than bundled, a model whose terms change
  can be withdrawn without shipping a new binary. This is a licensing safeguard,
  not just a size optimisation.

---

## 5. Obligations we take on

Even with a clean set, we owe things:

| Obligation | Trigger | What we do |
| --- | --- | --- |
| LGPL relink rights | FFmpeg, libsndfile, LAME | Ship them as DLLs, never statically link; publish the exact source versions used and our object files or a documented relink path |
| Attribution notices | BSD/MIT/Apache deps | An in-app "Third-party licences" screen, generated from the vcpkg manifest at build time, not maintained by hand |
| Apache-2.0 NOTICE | Crashpad and others | Reproduce NOTICE contents verbatim |
| Steinberg VST3 agreement | Plugin hosting | Sign before Phase 5; keep the countersigned copy |
| JUCE licence maintenance | For as long as we distribute | Diarise the renewal; monitor the revenue cap |

Generate the attribution screen from the dependency manifest in CI. Hand-curated
licence lists go stale the first time someone adds a dependency in a hurry.

---

## 6. Open questions to close before Phase 1

- [ ] Confirm current JUCE Starter terms and revenue cap at juce.com (JUCE 9 EULA)
- [ ] Decide Rubber Band: buy, substitute SoundTouch, or build
- [ ] Confirm Spleeter weights licence in writing, if pursuing path 1 + Spleeter
- [ ] Confirm DeepFilterNet weights terms
- [ ] Sign the Steinberg VST3 licence agreement
- [ ] Begin EV code-signing certificate application *(weeks of lead time)*
- [ ] Legal review of the full dependency set
- [ ] Decide the cloud-vs-local policy for any path-2 stem separation
