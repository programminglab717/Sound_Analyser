# 07 — Autonomous work queue

Working order. The rule for what belongs here: it must be **verifiable** —
either locally (Linux, no GPU, no audio hardware, no Windows), through GitHub
Actions for real MSVC builds, or by the product owner, who is available for
testing and approvals.

That last clause is new and it moved a lot of work. Two things previously listed
as blocked were not:

- **The UI was never blocked.** Qt renders headlessly under
  `QT_QPA_PLATFORM=offscreen`, so a window can be built, screenshotted and
  *looked at* in this environment. `tools/ui_smoke_test.py` now reads the
  rendered pixels back in CI.
- **A GPU was never required.** The Phase 0 spike measured CPU rendering at
  11.59 ms against a 16.67 ms frame budget at 1080p. It fits with 30% to spare.
  The shader path is an optimisation for 4K and above, not an entry ticket.

Audio hardware is still not present here, but the owner can run a build, so
backends are written, unit-tested against the abstraction, and confirmed by
them.

---

## Working order

Each item: finish it, verify locally on all three presets, push, **confirm CI
green before starting the next**. CI is the only real-hardware check available,
and treating local green as sufficient is exactly how several bugs reached the
branch unnoticed.

0. **Drive Windows CI to green.** Done. The suite passes on MSVC debug and
   release, and on Linux debug, release and asan.
1. **A runnable vertical slice.** In progress, and now ahead of everything
   below, because a product nobody can run cannot be judged. Open, look,
   select, play, edit, save. The analysis layers are further along than the
   surface that exposes them, and that is the wrong way round.
2. **True-peak limiting.** The feature spec lists "limiter (true-peak)" as P1
   and the current limiter guards sample peaks only, so inter-sample peaks can
   still exceed the ceiling after reconstruction. Needs oversampled detection;
   `sa-analysis` already has the interpolator.
3. **Stereo linking for dynamics.** Every processor is single-channel today, so
   two instances across a stereo pair shift the image on hard-panned transients.
   Needs a summed sidechain and a link amount.
4. **Lazy spectrogram tiles with LRU eviction.** Done. A decimated overview,
   always resident and bounded by a memory budget, plus full-resolution tiles
   fetched for the view and evicted least-recently-used. The window draws from
   it; the length limit and the coarse-hop fallback are gone. Tiles are checked
   bit-identical to the eager pyramid over the same frames.

   Persisted across sessions too, keyed by content hash, which this item also
   asked for: reopening a file analysed before reads its overview off disk
   instead of rebuilding it. `%LOCALAPPDATA%` on Windows and
   `$XDG_CACHE_HOME` elsewhere, bounded at a gibibyte, least-recently-used
   eviction.

   The key is SHA-256 over every setting that changes a stored byte plus the
   file's size, timestamp, header and sixteen sampled blocks. That is a
   *sampled* hash and is documented as one: two files of the same length and
   timestamp differing only between the sampled blocks would collide, so it
   must never be used as an integrity check. The timestamp is in the key to
   narrow that window, at the cost of a needless rebuild after a restore or a
   copy -- a rebuild is cheap where wrong pixels are not.

   Every stored file is treated as untrusted input, because a crash or a full
   disk produces a half-written one without anyone attacking anything. Header
   CRC checked before any field in it is believed, payload CRC after, lengths
   checked against the real file size in 64-bit arithmetic before any
   allocation, and a file that fails any of it is deleted rather than used.
   Truncations, header and body corruption and zeroed files are all tested
   under ASan.

   The SHA-256 was checked here against the published vectors independently
   of the module's own tests -- a hash that is stable and wrong still works
   perfectly as a cache key, so nothing else in the suite would ever have
   noticed.

   Windows is entirely unproven: `%LOCALAPPDATA%`, the wide-character
   environment read, rename-over-existing semantics and the sharing-violation
   behaviour on deleting an open file are reasoned about in comments and
   compiled nowhere here.
5. **High-quality resampler.** Needed for rate conversion on import and export,
   and by the time-stretch work below.
6. **Time-stretch and pitch-shift.** Rubber Band is GPL/commercial and excluded
   by ADR 0006, so this is in-house: phase-locked vocoder plus WSOLA.
7. **FLAC decoding.** dr_flac is Unlicense and passes the gate. Unlike WAV,
   there is no metadata reason to write our own.
8. **`auscultate-cli`.** A headless driver for the engine: analyse, convert, batch. The
   architecture doc calls for it early as a forcing function for keeping the
   engine independent of any UI.
9. **Spectral repair primitives.** De-hum, de-click, de-clip, noise-profile
   denoise. Phase 3 work, all testable headlessly.
10. **Acoustic measurement.** Mostly done, with one part deliberately not
    claimed and one part not started.

    Done: octave and third-octave band energy; EDT, T20, T30, C50, C80, D50
    and centre time from an impulse response, per band as well as overall.
    Reachable as `auscultate-cli bands` and `auscultate-cli room`.

    Half of that gap is now closed. There **is** a real filter bank, in
    `sa-dsp`: Butterworth band-passes of configurable order, default six
    poles, on centres derived from the base-ten definition rather than
    tabulated. Measured, through the audio path: passband deviation at worst
    0.015 dB, band edges landing on -3.0103 dB to within 6e-8 dB, and a skirt
    of 18.48 dB per octave against the two-section filter's 12.27 -- a ratio
    of 1.506 where the two orders imply 1.5.

    Still **not claimed: IEC 61260**. The tolerance masks are not in this
    repository and were not available when the bank was written, so nothing
    has been compared against them and no part of this claims conformance to
    that standard. What is claimed is only what the tests measure: centres on
    1000 Hz x 10^(n/10) for third-octaves and 10^(3n/10) for octaves, edges a
    factor of 2^(+/-1/6) and 2^(+/-1/2) around them, each band a Butterworth
    band-pass of the stated order, and passband, edges and skirts behaving as
    that order implies. The phrase "IEC 61260" should not appear beside this
    code until the masks are here and the realised response has been checked
    against them.

    Done since: **impulse response capture**. `auscultate-cli sweep` writes an
    exponential sine sweep and `auscultate-cli deconvolve` turns a recording of it
    back into an impulse response, which `auscultate-cli room` then measures. Proved
    against synthetic material only: a sweep convolved with its own inverse is
    an impulse 53 dB above its own skirt; a three-tap room comes back with the
    taps on the right samples at the right levels and polarities; a 0.900 s
    decay reads 0.894 s through real files end to end; and third-harmonic
    distortion lands at T*ln(3)/ln(f2/f1) before the linear response, to the
    sample, at the level a cubic predicts.

    Not proved: any of it against a loudspeaker, a microphone or a room. That
    needs the owner, and §08 tells them what to watch.
11. **Content analysis.** Pitch and F0 contour, tempo and beat grid, key.

    Key: done, as `auscultate-cli key`. A chromagram folded onto twelve pitch classes,
    correlated against twenty-four profiles, with the correlation scaled by how
    shaped the chroma is -- a bare correlation reads 0.65 on material that uses
    all twelve notes evenly, which is why `strength` is not one. Right on all
    twenty-four keys of a synthetic four-chord progression, and not fooled by
    the relative minor. Not checked against real music; §08 asks the owner for
    a hit rate.

    The profiles are derived from theory and stated in the source, not the
    published probe-tone tables. Those would probably do better and are a
    small change once someone has the actual numbers to hand, which is not the
    same as recalling them.

    Pitch: done, as `auscultate-cli pitch-of`. YIN, with the cumulative mean
    normalisation, the absolute threshold and parabolic interpolation.
    Monophonic and says so. Recovers a known period to within 0.5% and reports
    nothing rather than a number on noise. Not checked against any published
    implementation or any recorded voice.

    Tempo and beat grid: done, as `auscultate-cli tempo`. Spectral flux for onsets,
    autocorrelation weighted towards the middle of the requested range to
    break the octave ambiguity, then a least-squares refit of the grid onto
    the onsets -- integer-lag autocorrelation alone is accurate to a fraction
    of a BPM, which over twelve seconds still drifts further than a hop.

    Of 60, 120 and 240 BPM the weighting prefers 120. Stated as a preference
    rather than a measurement, because that is what it is.

    Material with nothing rhythmic in it is reported as having no tempo. The
    test for that is onset concentration -- the share of the envelope in its
    loudest tenth of frames -- which separates rhythmic material at 0.77 and
    above from held tones and noise at 0.38 and below. An absolute flux floor
    does not work and was removed rather than left in: a 1024-sample window is
    too short to separate a low tone from its own negative frequency, so the
    magnitude spectrum genuinely pulses at the tone's rate, and that pulse is
    perfectly periodic.

    Thresholds are calibrated on synthetic material only. There is no real
    music here, and a dense, heavily compressed mix has a less peaky envelope
    than anything that can be synthesised, so the false-negative risk on real
    material is unmeasured. §08 asks the owner to try it.
12. **Forensics.** Done. Lossy-codec cutoff detection and true bit-depth
    detection are in `auscultate-cli provenance`, and the A/B null test is `auscultate-cli
    null`: align, gain-match, subtract, and report the residual with a
    per-octave breakdown. An exact copy, a delayed copy and a scaled copy all
    null to the float floor, so anything that does not null is a real
    difference. Nothing is checked against another implementation of a null
    test; the ground truth is constructed.

## Worth attempting, uncertain

- **EBU/ITU conformance vectors.** ~~Worth attempting~~ — attempted, and the
  answer is no from inside this container. Phase 2's exit criterion needs them
  and they have never been run.

  Tried on 2026-09-19. The agent proxy answers 403 to CONNECT for both
  `tech.ebu.ch` and `www.itu.int`, so the publishers are unreachable; the denial
  is policy rather than a transport failure, and retrying will not change it.
  `github.com` *is* reachable, and third-party repositories do carry copies of
  the EBU material — but pulling one into a closed-source product without
  establishing what licence it is offered under is exactly the kind of decision
  the standing rules say to stop on, and this project's licence gate exists
  because that kind of thing is expensive to reverse.

  **So this now needs a person**, and has moved to §Blocked. The route is for
  the owner to obtain Tech 3341, Tech 3342 and BS.2217 from the EBU and the ITU
  and drop them somewhere readable; the suite can then be run against them and
  reported case by case. Until that happens, nothing in the project claims
  conformance, which is the correct state rather than a gap to paper over.

## Blocked — needs someone or something not here

| Item | Needs |
| --- | --- |
| EBU/ITU conformance vectors | The published test material. Both publishers are blocked by the network policy here; see above. |
| GPU shader renderer | Real GPU across Intel/AMD/NVIDIA. No longer a Phase 0 risk — it is an optimisation. |
| ~~Confirming audio actually comes out~~ | **Done, 2026-09-20.** The owner ran the Windows build and heard the file play. What is still unobserved: rates other than the one tried, exclusive-mode devices, removal mid-playback, load. ALSA on real hardware is untried. |
| True-peak filter conformance | BS.1770-4 Annex 2 Table 3, transcribed from the published standard rather than memory. |
| Platform compliance targets | Checking against live platform documentation before they ship as presets. |
| ~~Product name~~ | **Decided: Auscultate**, published by Delta Creation Co. Trademark and domain checks are still outstanding and are the owner's — see below. |
| Pricing, licence sign-off | A person. |
| Code-signing certificate | ~~A person and about $10/month.~~ **Decided: no.** Ruled out by the no-spending constraint; see below. |

## Decided: the product is called Auscultate

Chosen by the owner on 2026-09-19 and threaded through the application, the
build targets, the packaging and these documents. Published by Delta Creation
Co.

The reasoning was search as much as meaning. "Sound Analyser" was the working
title and would have been close to unrankable: it is a generic phrase
competing with every tutorial and freeware page ever written, and no amount of
optimisation wins that. A coined or uncommon word ranks for itself within
weeks because nothing else uses it. *Auscultate* also happens to mean the
thing this product does -- listening to something closely through an
instrument -- so the name is not only distinctive but descriptive.

Binaries are `auscultate` for the application and `auscultate-cli` for the headless
driver.

**Still outstanding, and the owner's to do, before anything ships publicly:**

- A trademark search on the USPTO and EUIPO registers, in the software and
  audio classes.
- Domain availability.
- A plain search of the audio field for existing use.

None of those can be done from inside this container -- the network policy
here reaches GitHub and little else -- so they are recorded here rather than
quietly assumed. The name is threaded through the code either way: nothing has
shipped, so changing it again is a search and replace rather than a recall.

## Decided: shipping unsigned

The blocked list used to carry "code-signing certificate, about $10/month".
That is struck out, because this project does not spend money and no amount of
looking changes what is available.

**There is no free code-signing certificate authority.** Let's Encrypt signs
TLS and not code. Since 2023 code-signing keys must live on a hardware token
or in an HSM, which raised the floor rather than lowering it. SignPath's
foundation tier does sign for free but requires the project to be open source,
and this one is not. A self-signed certificate is worse than nothing: the same
warning plus an explicit "unknown publisher".

**What shipping unsigned actually costs.** Windows SmartScreen shows
"prevented an unrecognised app from starting" with *Don't run* as the default
button, and a good share of people stop there. The subtler cost is that
SmartScreen's reputation attaches to the file hash for an unsigned binary and
to the certificate for a signed one -- so every release starts from zero
warnings again, where a signed product would inherit the trust already earned.
That compounding is the real thing being given up, and it gets worse the more
often the product ships.

**So: ship unsigned, and get onto winget.** `winget install` is Microsoft's
own package manager, free to publish to, and an install through it does not
put the download warning in front of anyone. Scoop and Chocolatey are the
same bargain with smaller audiences. The download page says plainly what
warning to expect and why, because a user who was warned in advance clicks
through and a user who was not closes the tab.

This is a decision rather than a deferral, and it is the owner's: put to them
on 2026-09-19 with the trade above stated plainly, and confirmed. Revisit it
only if the licensing model changes -- an open-source release would qualify
for free signing, and that is a product decision, not a build one.

## Standing rules for unattended work

- **Commit and push after every completed item.** The container is ephemeral;
  uncommitted work is lost when it is reclaimed.
- **Check CI after every push.** Local green is not evidence about Windows.
  If the GitHub tools are unavailable in the firing session — a scheduled
  Routine may run without connector access — still push, and say plainly in the
  commit message that CI verification is pending rather than implying the change
  was checked. An unverified push recorded as unverified is fine; one reported
  as green is not.
- **Never weaken a test to make it pass.** If an assertion fails, either the
  code is wrong or the assertion was wrong — establish which, and say which in
  the commit message.
- **Record what was not verified.** Every commit message states what the change
  does not prove, in the same voice as the rest of this repository.
- **Stop and leave a note rather than guess** on anything in §Blocked, or on any
  decision that would be expensive to reverse.
