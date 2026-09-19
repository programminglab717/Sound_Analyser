# 05 — Licensing & Dependencies

> **Not legal advice.** Engineering analysis of publicly stated licence terms,
> current as of September 2026. Terms change — JUCE has revised its twice in
> recent years. **Have a lawyer review the dependency set before 1.0.**

---

## 0. The constraint

**No licence purchases, ever. Every dependency must be free in perpetuity for
closed-source commercial distribution, or we build it ourselves.**

This is stricter than "free to start." It rules out anything with a revenue cap,
a royalty, a seat fee, or a paid tier we would eventually be forced into. Three
tests every dependency must pass:

1. **Free forever?** Not free-until-you-succeed.
2. **Closed-source permitted?** Rules out GPL/AGPL.
3. **No revenue, seat or unit cap?** Rules out capped free tiers.

A dependency failing any test is replaced, or built in-house.

---

## 1. What this constraint kills: **JUCE**

JUCE's free **Starter** tier permits closed-source distribution, but it is
capped at roughly **$20k/yr**, and the cap is unusually broad:

- It counts **gross revenue** (before expenses), not profit.
- It counts revenue from **all** uses of the framework, across all products.
- It counts **donations, sponsorship and advertising** — revenue from a free or
  pay-what-you-want product counts toward it.
- On exceeding it you must **purchase a licence or immediately cease
  distributing**.

Our stated plan is freemium. **The plan's success condition is the licence
tier's breach condition.** Under a no-purchases constraint, JUCE is not a free
dependency — it is a deferred bill that falls due at the exact moment the
product starts working, and the alternative to paying it is pulling the product
from distribution.

### Why we switch now rather than later

The asymmetry is decisive and it is entirely about timing:

| | Cost if we switch **now** | Cost if we switch **later** |
| --- | --- | --- |
| Engineering | ~6–10 weeks of foundation work | Rip out the UI and audio layers of a mature app |
| Timing | Week 0. No code exists yet. | Month 12+, at the moment of commercial traction |
| Risk | Bounded, known | Unbounded, under revenue pressure, or stop shipping |

There is no code yet. This decision will never again be as cheap as it is today.

> **Reversible if the constraint softens.** If a ~$1000 perpetual JUCE licence
> later becomes acceptable — paid out of the revenue that triggered it — the
> engine modules (`sa-core` … `sa-ml`) carry no UI-framework dependency by
> design, so only `sa-ui` and the device layer would change. We are not burning
> the bridge, only declining to camp on it.

---

## 2. The replacement stack

Everything below is free in perpetuity for closed-source commercial use, with no
revenue cap.

| Layer | JUCE provided | Free replacement | Licence | Notes |
| --- | --- | --- | --- | --- |
| **App shell / UI** | JUCE Components | **Qt 6** *(dynamically linked)* | **LGPLv3** | Free forever for closed source. Obligations in §5. Alternative: **Dear ImGui** (MIT, zero obligations) — see §2.1 |
| **GPU rendering** | JUCE OpenGL | OpenGL via `QOpenGLWidget` | — | We draw the spectrogram ourselves regardless |
| **Audio device I/O** | JUCE audio devices | **miniaudio** *(single-header)* | **Public domain / MIT-0** | WASAPI shared + exclusive, DirectSound, WinMM. Alternatives: RtAudio (MIT), PortAudio (MIT) |
| **ASIO** | JUCE ASIO wrapper | **Skip for v1** | — | ASIO SDK is free but needs a signed Steinberg agreement. WASAPI exclusive reaches 3–10 ms, ample for an editor. Revisit only on user demand |
| **Plugin hosting** | JUCE VST3 hosting | **CLAP** | **MIT** | Fully clean. VST3 needs a free signed Steinberg agreement — add later if users demand it |
| **DSP building blocks** | `juce::dsp` | **Build in-house** | ours | Filters, dynamics, envelopes. Straightforward, well-documented DSP; ours forever |
| **Audio file I/O** | JUCE formats | **dr_libs** (public domain) + our own readers and writers | PD / ours | dr_flac and dr_mp3 are single-header public domain. WAV and AIFF are parsed and written in-house; FLAC is decoded by dr_flac and encoded in-house — see §2.2 |
| **Containers/codecs** | — | FFmpeg **LGPL build** | LGPL-2.1+ | Dynamic link, never `--enable-gpl` |
| **FFT** | `juce::dsp::FFT` | **PFFFT** or **pocketfft** | BSD | Never FFTW (GPL) |
| **Resampling** | — | **r8brain-free-src** | MIT | High quality |
| **Time/pitch** | — | **Build in-house** (phase-locked vocoder + WSOLA) | ours | Rubber Band is GPL/commercial → excluded. SoundTouch (LGPL) is the fallback if we run short of time |
| **ML inference** | — | **ONNX Runtime** | MIT | |
| **Crash reporting** | — | **Crashpad** | Apache-2.0 | |
| **Tests** | — | Catch2 / GoogleTest | BSL-1.0 / BSD | |
| **Installer** | — | Inno Setup / WiX | permissive | |

### 2.1 UI toolkit: Qt LGPL vs Dear ImGui

Both are free forever. The trade is real and worth deciding deliberately.

| | **Qt 6 (LGPLv3)** | **Dear ImGui (MIT)** |
| --- | --- | --- |
| Licence obligations | Dynamic linking, relink rights, source offer | **None** |
| Redistributable | ~30–50 MB | ~2 MB |
| Accessibility / screen readers | **Good** | **Poor** — a real gap |
| Text editing + IME | **Strong** — matters for the transcript editor | Basic |
| Native dialogs, high-DPI, multi-monitor | Built in | Roll your own / Win32 |
| Custom GPU canvas | Good (`QOpenGLWidget`) | **Excellent — its native idiom** |
| Forms, settings, batch config | **Strong** | Tedious at scale |

**Recommendation: Qt 6 under LGPLv3.** The transcript editor and the
accessibility commitment in the product brief both need real text and real
widget semantics, and ImGui is weak at exactly those. The LGPL obligations are
mechanical and we already planned the attribution screen.

Use **Dear ImGui** instead if the LGPL obligations are judged unacceptable, or
if we later decide to drop the accessibility and transcript-editing goals — it
is genuinely the better fit for the spectral canvas alone.

> Avoid Qt's **GPL-only modules**: Qt Charts, Qt Data Visualization, Qt Virtual
> Keyboard. We draw our own charts anyway. Also note Qt LTS releases are
> commercial-only for a window — use current releases or build from source.

### 2.2 FLAC encoding: libFLAC assessed, then written in-house

The editor reads FLAC through **dr_flac** (Unlicense/MIT-0, vendored). Writing
one needed a separate decision, because dr_libs has no encoder.

**libFLAC passes the policy.** The Xiph reference library — `libFLAC` and
`libFLAC++`, `src/libFLAC/` in the xiph/flac repository — is **BSD-3-Clause**,
which is on the allowlist and is free in perpetuity for closed-source
distribution with no revenue, seat or unit cap. It is not the whole repository:
the `flac` and `metaflac` command-line tools, the test suite and the build
plumbing are **GPL-2.0-or-later**, so vendoring would have to be surgical, and a
mistake there is a GPL obligation rather than a build error. There is no patent
position to take; the format is unencumbered and Xiph grants its patents
explicitly.

**We wrote the encoder instead, and the reason is size rather than licence.**
libFLAC is roughly forty C source files and its own build system. Next to
`third_party/dr_libs`, which is two single headers decoding three formats, it
would be the largest third-party surface in the tree by an order of magnitude —
for an encoder whose format is fully documented and whose correctness is
checkable exactly. A FLAC is lossless, so a round trip has a pass/fail answer
and no judgement in it: encode, decode with an unrelated decoder, compare the
samples bit for bit. That is the test `sa-io` runs, against dr_flac.

This is the same reasoning that produced our own WAV and AIFF parsers, our own
FFT and our own resampler, and it is deliberately **not** the reasoning applied
to FLAC *decoding*: decoding is a hostile-input problem and a hand-rolled
entropy decoder is a worse attack surface than a decade-fuzzed one. Encoding has
no such surface — the input is a buffer this program produced.

**What it costs.** No LPC stage: subframes come from the constant, verbatim and
four fixed polynomial predictors, with wasted-bit detection, stereo
decorrelation and a partitioned-Rice search. Output is conformant FLAC and is
several percent larger than `flac -8` would produce. If that ever matters more
than the dependency does, libFLAC remains available on these terms and this
section is the determination it would be adopted under.

---

## 3. Remaining costs that are **not** licences

The constraint is about licences, but three real costs remain. Two are solvable
free; one is not.

| Cost | Free path? | Recommendation |
| --- | --- | --- |
| **Model hosting / CDN** | ✅ Yes | GitHub Releases (2 GB/file), Hugging Face, or Cloudflare R2 free tier. Fully solved at zero cost |
| **Stem-separation model training** | ❌ No | Thousands of dollars of GPU time plus a licensed multitrack corpus. **Off the table.** Ship without stem separation — see §4 |
| **Windows code signing** | ⚠️ **No free option exists** | See below |

### Code signing — the one unavoidable cost

There is **no free code-signing path that Windows trusts.** Unsigned installers
trigger SmartScreen warnings and AV false positives, which measurably costs
installs.

Options, cheapest first:

1. **Ship unsigned for alpha/beta.** Free. Document the SmartScreen workaround.
   Acceptable while the audience is early adopters; not acceptable at 1.0.
2. **Azure Trusted Signing — roughly $10/month.** The cheapest legitimate path
   by a wide margin. Requires organisation identity verification.
3. Traditional OV/EV certificate — $200–500/yr. No reason to pay this now.

**Recommendation:** unsigned through beta, then ~$10/month before public 1.0.
If that is genuinely impossible, ship unsigned and accept the install friction —
but budget it as a known conversion cost, not an oversight.

---

## 4. ML model weights

> **The rule:** a model's *code* licence and its *weights* licence are different
> documents. MIT code with research-only weights is common.

| Model | Code | Weights | Usable? |
| --- | --- | --- | --- |
| **Whisper** | MIT | **MIT** | ✅ **Yes** — transcription foundation |
| **Silero VAD** | MIT | MIT | ✅ Yes |
| **RNNoise** | BSD | BSD | ✅ Yes — baseline denoiser |
| DeepFilterNet | Verify per release | Verify | ⚠️ Strong denoiser; confirm in writing |
| YAMNet / PANNs | Apache-2.0 / MIT | Verify per checkpoint | ⚠️ Mostly permissive |
| Spleeter | MIT | Reported MIT | ⚠️ Lower quality; confirm in writing |
| **Demucs / htdemucs** | MIT | **Research use only (Meta)** | ❌ **No** — ONNX conversion does not change this |
| Open-Unmix **UMXL** | MIT | CC BY-NC-SA 4.0 | ❌ No |
| Open-Unmix UMX/UMXHQ | MIT | Ambiguous (MUSDB18-HQ is NC) | ⚠️ Do not assume |

### Stem separation: ship without it

All three previous paths are now closed. Licensing costs money; training costs
money; the good free weights are non-commercial. **Ship v1 without stem
separation.**

This is a smaller loss than it looks. Stem separation is a *music production*
feature, and our audience is repair and mastering. Every feature it would have
supported — denoise, de-reverb, transcription, event tagging, voice isolation
from steady background — has a clean-licence path already.

Revisit only if Spleeter's weights are confirmed MIT in writing, or a
permissively-licensed model of adequate quality appears.

### Policy
- **No model ships without a written licence determination** recorded in the
  model registry manifest.
- CI fails on any model whose `licence` field is absent or `unverified`.
- Because models are downloaded rather than bundled, a model whose terms change
  can be withdrawn without shipping a new binary.

---

## 5. Obligations we take on

Free is not the same as unconditional. What we owe:

| Obligation | From | What we do |
| --- | --- | --- |
| **Dynamic linking only** | Qt, FFmpeg, libsndfile | Ship as DLLs. **Never static link.** Enforce in CI |
| **Relink rights** | Qt (LGPLv3), FFmpeg | Publish exact dependency versions; provide object files or a documented relink path |
| **Source offer** | Qt, FFmpeg, libsndfile | Host the unmodified source of the exact versions used, or a written offer |
| **Attribution notices** | All permissive deps | In-app "Third-party licences" screen, **generated from the vcpkg manifest at build time** |
| **Apache NOTICE** | Crashpad | Reproduce verbatim |
| **No GPL-only modules** | Qt Charts, Qt DataVis, Qt Virtual Keyboard | CI check on the linked module list |
| **Anti-tivoization** | LGPLv3 | No mechanism preventing users replacing our DLLs |

Generate the attribution screen from the manifest in CI. Hand-maintained licence
lists go stale the first time someone adds a dependency in a hurry.

### Enforce it mechanically
Add a CI gate that fails the build on any dependency whose licence is not on an
allowlist (MIT, BSD, Apache-2.0, public domain, MIT-0, BSL-1.0, LGPL-with-dynamic-
linking). A human reviewing dependency licences will eventually miss one; the
build should not.

---

## 6. What we now build ourselves

Accepted in-house scope created by this constraint. All of it is well-trodden
DSP with published literature — this is work, not research.

| Component | Effort | Notes |
| --- | --- | --- |
| DSP primitives (filters, dynamics, envelopes) | 3–4 wks | Replaces `juce::dsp` |
| Time-stretch / pitch-shift | 3–4 wks | Phase-locked vocoder + WSOLA. SoundTouch (LGPL) is the fallback |
| Audio device abstraction over miniaudio | 1–2 wks | Thinner than JUCE's, and ours |
| UI shell on Qt (docking, panels, transport) | 3–4 wks | |
| Plugin hosting via CLAP | 2 wks | Simpler than VST3 — CLAP is a better-designed API |

**Total added to Phase 0–1: roughly 6–10 weeks.** Offsetting benefits: no
licence exposure ever, no revenue cap, a DSP library we own outright, and
freedom to relicense or open-source later without untangling anyone else's terms.

---

## 7. Open questions to close before Phase 1

- [ ] Final call: **Qt 6 LGPL** vs **Dear ImGui** *(recommendation: Qt)*
- [ ] Confirm DeepFilterNet weights terms in writing
- [ ] Confirm Spleeter weights terms if separation is ever revisited
- [ ] Decide code-signing posture: unsigned beta → ~$10/mo at 1.0, or unsigned throughout
- [ ] Stand up the CI licence-allowlist gate **before** the dependency list grows
- [ ] Legal review of the full dependency set
- [ ] Confirm no GPL-only Qt module is in the linked set
