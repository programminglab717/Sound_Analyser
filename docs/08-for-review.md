# 08 — What needs you

Everything in this file needs a person. Nothing here can be closed from inside
the container: it needs hardware, a listening test, a published document, or a
decision that is yours rather than mine.

Ordered by how much it would unblock.

---

## 1. Run it, and tell me what is wrong with it

**What to do.** Download `sound-analyser-windows` from the latest green CI run's
artefacts (Actions → the run → Artifacts at the bottom), unzip it anywhere, and
run `sound-analyser.exe`. Qt ships beside it, so there is nothing to install.

It is about 12.6 MB and it is built and tested by the same run that produces it:
every CI job green -- both MSVC configurations, AddressSanitizer and
ThreadSanitizer among them -- before the package is uploaded. `sa-cli.exe` is
in the folder beside it: the headless driver, with fourteen commands: analyse,
provenance, convert, normalise, denoise, declick, declip, dehum, compress,
gate, channels, render, stretch and pitch.

Then open something real — a recording of your own, not a test tone — and try:

| | |
| --- | --- |
| Open | File ▸ Open audio. WAV, AIFF, FLAC and MP3 all work |
| Look | Scroll wheel zooms, shift+wheel scrolls, middle-drag or alt-drag pans |
| Select | Drag in the waveform for a time span; drag in the spectrogram for a time *and frequency* box |
| Play | Space |
| Measure | The panel on the right; it follows the selection |
| Check it sums | The STEREO block in the panel. Correlation near +1 is safe, 0 is wide, negative is trouble; "mono sum" is how many dB the programme loses when something plays it in mono, and anything much past -3 is cancellation rather than arithmetic |
| Read the spectrum | The curve under the meters, also following the selection. The filled shape is the average, the line above it is the loudest any moment got. Hover for a frequency and a level |
| Fix the level | Process ▸ Normalise to target, after picking a target in the panel |
| Filter | Process ▸ Filter, or Ctrl+F. High-pass at 80 Hz is the one to try first on anything with rumble in it |
| Limit | Process ▸ Limiter. It holds a true-peak ceiling rather than approaching it |
| Even it out | Process ▸ Compressor, or Process ▸ Gate for the quiet between phrases. Both obey the selection, and both get a run-up over the audio before it so the passage does not open with a burst of the untreated signal |
| Stretch | Process ▸ Time stretch. Asked for as a percentage of the current length, so 200 is twice as long. Pitch stays put |
| Retune | Process ▸ Pitch shift. Semitones, and fractions of one — 0.01 is a cent, which is what a tuning fix actually needs |
| Repair | Draw a box round a hum or a click, then Repair ▸ Attenuate or Heal |
| Denoise | Select a passage of noise alone ▸ Repair ▸ Learn noise profile, then select the whole thing ▸ Repair ▸ Reduce noise |
| Declick | Repair ▸ Remove clicks. It says how many it found. On a clean recording the right answer is none, and it gives that answer |
| Declip | Repair ▸ Restore clipped peaks. It says how many it put back, and how far it had to bring the file down so they fit |
| Dehum | Repair ▸ Remove mains hum. It finds the frequency itself — 50 or 60, and to a hundredth of a Hertz — and says what it found |
| Mark | Ctrl+M drops a marker at the caret, or over the selection if there is one. Alt+Left and Alt+Right walk between them, and landing on a region selects it |
| Flip it about | Process ▸ Reverse, Invert polarity, Swap channels, Sum to mono. Each one obeys the selection, so they work on a passage as well as the whole file |
| Fade | Process ▸ Fade in or Fade out over a selection. Process ▸ Fade shape picks the curve: linear unless you change it, and equal power is the one that does not leave a hole when two fades meet |
| Compare | Select a passage you like, Ctrl+R to keep its spectrum, then select another. The dashed line is the one you kept, and hovering gives the difference in dB. It survives opening a different file, so you can chase a reference record |
| Deliver | File ▸ Export format picks 16-bit, 24-bit or float, and File ▸ Dither says what to do about the bits a 16-bit export drops. Triangular is on by default and is the right answer almost always. A 24-bit or float export is never dithered, so a file exported untouched comes back byte for byte |
| Save | File ▸ Save session, reopen it, check nothing was lost |

**What I need back.** Not a bug list — impressions. Where did you expect
something to be and not find it? What looked wrong before you could say why?
What did you try that I have not built? Anything you had to think about for more
than a second is a design failure worth hearing about.

**Specifically, does sound come out?** No audio has ever left this machine. The
WASAPI backend compiles and its logic is tested, but it has never driven a sound
card. If playback is silent, or crackles, or the playhead drifts from what you
hear, that is the single most valuable thing you can tell me.

## 2. Metering conformance vectors

**Why it needs you.** The loudness meter is internally consistent and anchored
correctly — a 1 kHz stereo sine at −23 dBFS reads −23.0 LUFS — but no official
conformance material has been run against it. Until it has, "EBU R128 compliant"
is a claim we have not earned and I will not write it in the product.

**What would close it.** The EBU Tech 3341 and Tech 3342 test material, and
ITU-R BS.2217. These are published by the EBU and the ITU. If you can obtain
them legitimately, drop them somewhere I can read and I will run the suite
against them and report each case pass or fail.

**Related, and worse.** BS.1770-4 Annex 2 Table 3 specifies the exact
interpolation filter a true-peak meter must use. I have not transcribed it,
because writing a filter table from memory would be worse than admitting I do
not have it. Ours under-reads by up to 0.44 dB at 4x oversampling — measured,
documented in `TruePeakMeter.h`, and guarded by a test. With the published table
that becomes exact.

## 3. Decisions that are yours

| Decision | Why it cannot wait forever | My recommendation |
| --- | --- | --- |
| **The name.** "Sound Analyser" is a working title | It is in the window title, the executable, the session extension and the namespace | Decide before anyone outside sees it; renaming later costs a day and is never done cleanly |
| **What is free and what is paid** | The feature spec tags tiers, but those tags are guesses | Everything built so far should stay free. Charge for batch processing, plugin hosting, and measurement reporting — capability, not access |
| **Whether we ever ship a Mac or Linux build** | The device layer already has ALSA, and nothing above it is Windows-specific | Cheap to keep the option open, so keep it open; do not announce it |
| **Crash reporting** | Needs a privacy position before any code is written | Opt-in, on-device symbolisation, nothing sent without a prompt |

## 4. Things I would want a second opinion on

Not blocked, but a person's judgement would be better than mine.

- **How a long recording feels.** A file past about ninety minutes used to get
  no spectrogram at all -- it would have needed more than a gigabyte of cache.
  It now gets a coarser one instead, at 42 or 85 ms per column rather than 21,
  and says so. Whether that reads as a sensible trade or as a blurry picture is
  a judgement I cannot make from here. The real answer is building the picture
  in tiles as you scroll, which is a day of work and is on the list.

- **Whether the de-hummer is cautious enough, or too cautious.** It only
  removes a partial where a steady sinusoid clearly dominates, and leaves the
  rest, so on a recording where hum sits under a loud bass line it will take
  out the exposed harmonics and leave the buried ones. That is the safe
  direction to err in, but I do not know whether it is the one you want. If you
  have anything with real hum on it, that is the test.

- **Whether declipping helps on something you actually have.** On synthetic
  material it puts the peaks back to within a decibel of where they started
  even when nearly half the samples were pinned. It also has a limit I can
  state exactly: a recording that is a single sustained tone, clipped, cannot
  be restored at all, because the flat top is then the shape rather than damage
  to it. Real music is not a sustained tone and does not hit this, but if you
  have a genuinely clipped file I would like to know what it does with it.

- **Whether the declicker is finding the right things.** On synthetic damage
  it takes the error down by 32 dB and leaves clean material bit-identical,
  which is the strongest statement I can make from inside a container. What I
  cannot tell you is how it behaves on a real vinyl transfer, where the clicks
  are not impulses and the music is not three sine waves. If you have anything
  with real surface noise on it, that is the test. The sensitivity control is
  the one to move: lower finds more and repairs more that did not need it.

- **How a stretch sounds to you.** The measurements say a stretched tone is
  the same tone and a shifted one has moved by exactly the right ratio, and
  both are exact. What measurements cannot tell me is how a *drum* sounds
  stretched to 130%, because a phase vocoder smears transients by construction
  and no number I can produce here says whether that is acceptable or
  embarrassing. Try it on something percussive and tell me.
- **Whether `sa-cli provenance` is right about your files.** It reads what
  the audio says about its own history rather than what the header claims:
  how many bits a file really uses out of the depth it declares, which is
  exact, and whether something with a very steep filter took the top off the
  band, which is evidence rather than proof. Run it over a folder you know
  the history of -- a few originals, a few things that have been through an
  MP3, a few 16-bit masters delivered as 24 -- and tell me where it is wrong.
  It is the one feature here I would most expect to be confidently wrong
  about real material.

- **Whether the noise-shaped dither is worth having.** It is a plain
  second-order shaper, not one of the published psychoacoustic curves, which
  I did not write down because writing a filter design from memory would be
  worse than saying so. It measurably beats flat triangular on distortion
  (13.9 dB against 12.0), and whether the hiss it puts in the top octave is
  a fair price is a listening question, not a measuring one.

- **Whether the stereo meters say enough.** Four numbers, no goniometer and
  no per-band correlation, so a mix whose bass alone is out of phase reads
  as merely "wide" rather than as the specific fault it is. Per-band
  correlation is the obvious next step and I have not built it, because I do
  not know whether the four numbers already tell you what you need.

- **Whether the compressor and gate defaults are close to useful.** -20 dB
  at 4:1 with 10 and 100 ms, and a gate opening at -40 dB with 80 dB of
  depth. Those are textbook starting points rather than anything tuned on
  real material, and the first thing a person notices about a compressor is
  whether its defaults are in the right country.

- **The repair defaults.** Noise reduction defaults to 12 dB with 1.5x
  oversubtraction. Those produce clean results on my synthetic tests, but
  synthetic noise is stationary and real noise is not. Try it on a real bad
  recording and tell me whether it sounds thin, watery, or fine.
- **Whether the spectrum reference is the comparison you wanted.** It keeps
  the average curve and draws later ones against it, with the difference in
  decibels under the cursor. What it does not do is tell you what to change:
  no "you are 3 dB light at 4 kHz, here is the filter". That next step is
  worth building if this one is the right shape, and worth abandoning if it
  is not.

- **Whether the spectrogram is readable to you.** I chose magma by default and
  excluded rainbow deliberately (not perceptually uniform, and unreadable to
  red-green colour blindness). If viridis or greyscale reads better to you, that
  is data I cannot generate myself.
- **How much the window should do before it feels crowded.** There is a meters
  panel and five menus now. The next additions — an EQ, a spectrum analyser, a
  history list — need somewhere to live, and the answer is probably docking
  panels, which is a day of work I would rather not do twice.

---

## What does *not* need you

For completeness, so nothing sits here waiting that should not:

- GPU rendering. Measured as unnecessary: 11.59 ms against a 16.67 ms budget at
  1080p on CPU alone.
- Any licence purchase. There will never be one — six dependencies, all free in
  perpetuity for closed-source distribution, enforced by a CI gate that now
  checks fetched, found, vendored and vcpkg dependencies alike.
- MSVC compatibility. CI builds and tests every commit on real Visual Studio.
