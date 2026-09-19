# Auscultate — User Manual

**Developed by Delta Creation Co.**

Auscultate is a Windows desktop application for looking at a recording closely,
repairing it, and measuring it. It treats the spectrogram as an editing surface
rather than as a picture, and it is unusually careful about the difference
between a number it has measured and a number it has guessed.

This manual describes what is in the window, what each measurement means, and —
the part most audio documentation leaves out — when a measurement should not be
believed.

---

## Contents

1. [Getting started](#1-getting-started)
2. [The window](#2-the-window)
3. [Editing](#3-editing)
4. [Repair](#4-repair)
5. [Analysis, and when to distrust it](#5-analysis-and-when-to-distrust-it)
6. [The command-line driver](#6-the-command-line-driver)
7. [What this product does not claim](#7-what-this-product-does-not-claim)
8. [Appendix A — Keyboard shortcuts](#appendix-a--keyboard-shortcuts)
9. [Appendix B — Driving the window from a script](#appendix-b--driving-the-window-from-a-script)

---

## 1. Getting started

### 1.1 Getting the program

Auscultate is distributed as a zip file produced by the project's continuous
integration. On the repository's **Actions** tab, open the most recent green
run, scroll to **Artifacts** at the bottom, and download `auscultate-windows`.

Inside the zip is a folder called `Auscultate` containing:

| File | What it is |
| --- | --- |
| `auscultate.exe` | The application |
| `auscultate-cli.exe` | The headless driver, for scripts and batch work (§6) |
| `Qt6Core.dll`, `Qt6Gui.dll`, `Qt6Widgets.dll` and friends | The Qt libraries the application runs on |

There is no installer and nothing to install. Unzip the folder wherever you
like — a desktop, a memory stick, a network share — and run `auscultate.exe`
from inside it. The Qt libraries must stay beside the executable; they are
shipped as separate, replaceable DLLs because Qt's licence requires it, and
moving the executable out of the folder will stop it starting.

To remove it, delete the folder. Nothing is written outside it except the files
you ask for and your own window preferences.

### 1.2 Windows will warn you about it, and here is why

The first time you run `auscultate.exe`, Windows will show a blue dialogue
saying **"Windows protected your PC"** and offer only a **Don't run** button.
Click **More info**, then **Run anyway**.

This happens because the executable is **not code-signed**. A code-signing
certificate is a commercial product bought from a certificate authority, and
this build does not carry one. Windows SmartScreen shows that warning for every
unsigned executable it has not seen many times before; it is a statement about
the absence of a certificate, not a detection of anything.

That is worth saying plainly rather than apologising for, because a user who
meets an unexplained warning reasonably assumes malware. Decide for yourself
whether you trust the source you downloaded it from — that is the judgement the
warning is actually asking you to make — and if you do, **More info ▸ Run
anyway** is the whole of it. Windows remembers the decision for that copy of the
file.

### 1.3 It does not use the network

Auscultate has no network code in it. It is not built against any networking
library, so it cannot make a connection, check for updates, phone home or
activate. Everything it writes — exports, sessions, preferences — goes to your
own disk. See `PRIVACY.md` for the detail.

### 1.4 Opening something

**File ▸ Open audio** (Ctrl+O). The following are decoded:

| Format | Notes |
| --- | --- |
| WAV | RIFF/WAVE and RF64 |
| AIFF | Including AIFF-C |
| FLAC | Native FLAC; FLAC inside an Ogg container is not decoded |
| MP3 | MPEG-1, MPEG-2 and MPEG-2.5, layers I to III |

Files are identified by their contents rather than by their extension, so a
file named `.wav` that is really an AIFF opens correctly instead of being
handed to the wrong parser.

Exports are always WAV. See §3.8.

**File ▸ Open session** (Ctrl+Shift+O) opens a `.sa` file — an arrangement you
saved earlier, described in §3.9.

### 1.5 Playback has never been tested on real audio hardware

Press **Space** to play. There is a caveat that belongs here rather than in a
footnote: **no audio has ever come out of a real sound card from this
program.** The Windows (WASAPI) and Linux (ALSA) output paths are written, and
they are tested against a real thread on a real clock, but the machines that
build and test this software have no audio hardware in them. Playback is
therefore the one part of the product whose behaviour is inferred rather than
observed.

If the application cannot open any output device it falls back to a device that
plays to nothing, and it says so in the status bar: *"No sound card was
available — playback will run silently"*. The playhead will move and you will
hear nothing. That message is there precisely so that a silent playback is not
mistaken for silent audio.

If sound does come out but crackles, or the playhead drifts away from what you
hear, that is worth reporting.

---

## 2. The window

The window is in two columns.

**On the left**, stacked over a shared time axis: the time ruler, the waveform,
and the spectrogram. All three show the same stretch of time, and a selection
made in one appears immediately in the others. They are one timeline drawn
three ways, not three views that have to be kept in step.

**On the right**, stacked: the loudness meters, the musical analysis panel, and
the spectrum with its equaliser curve. All three follow the selection: select a
passage and they describe that passage; select nothing and they describe the
whole document. The split between the three is draggable.

**Along the bottom**: a status bar. On the left it shows the file name, sample
rate, channel count and duration, and — when something is selected — the
selection's start, end and length. On the right it shows a readout of whatever
is under the pointer.

### 2.1 The time ruler

A shared scale above both views, labelled in minutes and seconds. It also
carries the markers (§3.7): a point marker is a tick with its label, and a
region marker is drawn across the span it covers. The playhead appears here as
well as in the two views below.

### 2.2 The waveform

Drawn from a peak pyramid rather than from samples, which is why a two-hour
recording draws as fast as a two-second one.

Each column shows two things: the outer envelope is the minimum and maximum
sample in that column, and the brighter inner body is the RMS. The gap between
them is the crest factor, so heavily limited material looks visibly solid and an
unprocessed acoustic recording looks visibly spiky. That is a shape worth
learning to read; it tells you at a glance what has been done to a file.

Two analyses can be drawn over it:

- **The beat grid** (Analyse ▸ Beat grid over the waveform, Ctrl+Shift+B; on by
  default). Vertical lines at the beats the tempo tracker found. Drawn solid
  when the tracker is confident and **dashed** when it is not. Its whole
  purpose is described in §5.7: a tempo is the one measurement in this product
  you can check by eye in a second.
- **The pitch contour** (Analyse ▸ Pitch contour over the waveform,
  Ctrl+Shift+P; off by default). A line on its own logarithmic frequency axis,
  with the scale drawn at the right-hand edge, covering the tracker's range of
  50 Hz to 1000 Hz. It breaks where nothing periodic was found rather than
  ruling a line through noise.

Bar lines are deliberately not drawn. The tempo tracker finds beats, not metre,
so it has no idea which beat is a downbeat and does not pretend to.

If an overlay stops part-way across the file, a vertical line marks where the
analysis ended. That is a bound, not a failure — see §5.11.

Hovering gives the time and the peak level in dBFS in the status bar.

### 2.3 The spectrogram

Time across, frequency up, level as colour. This is the surface the product is
built around: you see the hum, the click, the room, and you select them
directly.

- **Frequency scale** (View ▸ Frequency scale): logarithmic by default, linear
  available. A linear axis puts every fundamental anyone cares about in the
  bottom tenth of the display and gives the top half to the octave from 12 kHz
  to 24 kHz, where almost nothing happens; the logarithmic axis is right for
  nearly all work, and linear is there for when you are looking at harmonics of
  something high.
- **Dynamic range** (View ▸ Dynamic range): −60, −80, −96 or −120 dB. This is
  the level that reads as silence. −96 dB is the default. Lower it if a quiet
  recording looks blank; raise it if a noise floor is swamping the picture.
- **Colour map** (View ▸ Colour map): Magma, Viridis or Greyscale.

Hovering gives time, frequency and level in the status bar.

The picture is built in the background. A long file first appears at a coarse
resolution and sharpens where you are looking as detail arrives, which is why
scrolling a long recording shows a briefly softer picture rather than a blank
one. The status bar says *"building the spectrogram…"* while the first pass runs.

### 2.4 Moving around

Both views share these:

| Gesture | Effect |
| --- | --- |
| Wheel | Zoom, anchored where the pointer is |
| Shift + wheel | Scroll |
| Middle-drag, or Alt + left-drag | Pan |
| Left-drag | Select |
| Shift + left-click | Extend the existing selection from its far edge |
| Double-click | Select everything |
| **F** | Zoom to fit the whole document |
| **Ctrl+E** | Zoom to the selection |

### 2.5 Selecting

A left-drag in the **waveform** selects a span of time.

A left-drag in the **spectrogram** selects a span of time *and* a band of
frequency — a box. The time part of that selection is the same selection the
waveform shows; the frequency part is used by the spectral repairs
(Repair ▸ Attenuate, Repair ▸ Heal). A click, or a drag along the time axis with
no vertical extent, means all frequencies.

**Repair ▸ Select all frequencies** resets the band to the full spectrum without
disturbing the time selection.

A click with no drag leaves an empty selection — a caret. Most processing verbs
treat an empty selection as "the whole document", and the two panels on the
right do the same. Edits that need a span (cut, copy, trim, silence) are greyed
out until there is one.

**Escape** deselects.

### 2.6 The loudness meters

The top panel on the right. The large number is **integrated loudness in LUFS**;
everything below it is context for that number.

LUFS — Loudness Units relative to Full Scale — is a measurement of how loud
something actually sounds, as opposed to how big its samples are. The signal is
filtered to approximate the ear's frequency response, squared, averaged in 400
ms blocks, and the quiet blocks are excluded so that the pauses in a piece do
not drag its loudness down. The result is a single number that ranks two
different pieces of music the way a listener would, which a peak meter and an
RMS meter both fail to do. Every streaming service and every broadcaster now
specifies delivery in LUFS, which is why it is the largest thing in the window.

| Row | What it is |
| --- | --- |
| **Integrated** | Gated loudness of the whole selection. `--` when nothing cleared the gate |
| **Range** | The spread between the quiet and loud parts, in LU (10th to 95th percentile of the short-term values) |
| **Short term** | Loudness of the last 3 seconds |
| **Max short term** | The loudest 3-second window anywhere in the selection |
| **Max momentary** | The loudest 400 ms window anywhere in the selection |
| **True peak** | The highest level the waveform reaches *between* samples, in dBTP |
| **Sample peak** | The largest sample, in dBFS |
| **Peak to loudness** | True peak minus integrated loudness — the headroom that survives loudness normalisation |
| **Correlation**, **Width**, **Balance**, **Mono sum** | The stereo section; shown only for a stereo pair |
| **RMS**, **Crest factor**, **DC offset** | Plain, ungated statistics |
| **Target** | A delivery specification to check against, and the verdict |

`--` never means zero. It means *there is no measurement here*, which is a
different fact. An integrated figure of `--` means nothing cleared the absolute
gate — the selection is silent, or shorter than one 400 ms block — and the panel
refuses to print a floor value that a reader could mistake for a loudness.

Each measurement is explained in §5.

### 2.7 The analysis panel

The middle panel on the right. It answers four questions about the selection:
what key it is in, how fast it goes, what note is sounding, and what the room
did to it.

**KEY** and **TEMPO** always run. **PITCH** and **ROOM** appear only when you
ask for them, from the Analyse menu, because a pitch contour is expensive and
room acoustics are meaningless on anything that is not an impulse response.

Where a figure cannot be established the panel says so in words rather than
printing a number beside a quiet flag. A key that no profile fits is shown as
**no key** with the reason; a passage with no rhythm in it is shown as **no
tempo**; a room measure the decay had no range for is shown as `--`. Where there
is an answer but a reason to doubt it, the answer is shown in amber with the
reason underneath. This is the most valuable thing the panel does and §5
explains each refusal.

The panel scrolls, because the ROOM section is eight rows and only appears when
asked for.

### 2.8 The spectrum

The bottom panel on the right: the average spectrum of the selection, on a
logarithmic frequency axis.

Two curves, and the pair says more than either alone:

- **The filled shape** is the average — what the passage is made of.
- **The line above it** is the loudest any single frame reached at that
  frequency.

A resonance that rings moves both. A cymbal moves only the peak. Mains hum shows
as a spike in the average that the peak barely exceeds, because hum never
varies. The gap between the two curves is how much a frequency comes and goes.

Hovering gives a frequency and a level.

**A reference curve.** Judging tonal balance means asking "compared with what",
and the honest answer is usually another passage. Select something you like,
press **Ctrl+Shift+R** (View ▸ Set spectrum reference) to freeze its average,
then select something else: the frozen curve is drawn dashed behind the live
one, and hovering gives the difference in dB. The reference survives opening a
different file, so you can chase a reference record. **View ▸ Clear spectrum
reference** removes it.

Only the average is kept as a reference, not the peak curve — the peak curve
answers a question about one passage rather than a comparison between two.

**Third-octave bands.** Analyse ▸ Third-octave bands on the spectrum
(Ctrl+Shift+T) lays a bar chart over the curves: thirty-one bands, drawn to the
band edges that were actually integrated, so on the logarithmic axis they come
out evenly wide. See §5.9 for what they are and are not.

### 2.9 The equaliser

**Process ▸ Show EQ curve** (Ctrl+Shift+E) draws an editable EQ curve over the
spectrum panel. The point of putting it there rather than in a dialogue is that
a boost sits directly over the peak it is correcting, at every window size.

| Gesture | Effect |
| --- | --- |
| Drag a handle | Sideways for centre frequency, up and down for gain |
| Wheel over a handle | Q — forward narrows |
| Shift + drag | Q as well, upward narrows, for pointers with no usable wheel |
| Double-click on the curve | A new band there |
| Double-click on a handle | That band back to 0 dB |
| Right-click a handle, or Delete with it selected | The band goes |

Bands are limited to ±24 dB, and to a Q between 0.1 and 24.

Showing the curve and applying it are separate actions on purpose. Looking at a
proposed curve against the spectrum is most of what the curve is for, and a tool
that applied on sight would make that impossible.

**Process ▸ Apply EQ curve** (Ctrl+Shift+Q) applies it to the selection, or to
the whole document if nothing is selected, as one undoable edit. The curve
**stays on screen afterwards**, so pressing it twice applies it twice — the
status bar says so when it happens. **Process ▸ Reset EQ bands** clears the
curve.

**This is not a mixing EQ and there is no live monitoring.** Dragging a band
changes a drawing and nothing else; the audio changes when you apply the curve.
Hearing a setting while you drag it needs the curve hung on the playback graph,
which is a different piece of work and is not in this build.

### 2.10 Playback

| | |
| --- | --- |
| **Space** | Play, or stop if playing |
| **Shift+Escape** | Stop |

Playback runs from the caret, or over the selection if there is one, and stops
at the end of the selection. The playhead is drawn in all three left-hand views
and is driven from frames that have actually left the device rather than frames
that have been queued, so it does not run ahead of the sound.

The audio device is opened on first use rather than at startup, so launching the
program does not seize the sound card. If the default output device cannot be
opened the next device in the list is tried before giving up.

See §1.5 for the caveat about hardware.

---

## 3. Editing

### 3.1 The model, in one paragraph

Editing is non-destructive. Cutting a range does not move audio around: it
rewrites a list of clips that point into the files you opened. This is why undo
is cheap, why a session file is small, and why **Process ▸ Flatten** exists —
that one deliberately bakes a range down into a single piece of audio when you
want the arrangement simplified.

Nothing you do in the window changes the file you opened. The only things
written to disk are exports and sessions, and both are written where you ask for
them.

### 3.2 Cut, copy, paste, delete

| Action | Shortcut | Effect |
| --- | --- | --- |
| Cut | Ctrl+X | Copies the selection, then deletes it and closes the gap |
| Copy | Ctrl+C | Copies the selection |
| Paste | Ctrl+V | Opens a gap at the caret and lays the clipboard into it |
| Delete | Del | Removes the selection and closes the gap |
| Silence selection | Ctrl+L | Silences the selection **in place**, keeping the timing |
| Trim to selection | Ctrl+T | Throws away everything outside the selection |
| Select all | Ctrl+A | |
| Deselect | Escape | |

Delete and Silence are both wanted, by different people, often in the same
session: a music editor cannot have the bar move, and a dialogue editor wants
the pause gone.

Pasting requires the clipboard and the document to have the same number of
channels; the status bar says so if they do not.

### 3.3 Gain and normalising

**Process ▸ Gain** (Ctrl+G) asks for a change in decibels, from −96 to +24, and
applies it to the selection — or to the whole document if nothing is selected.
It is a change, not a setting: applying −3 dB twice leaves you 6 dB down.

**Process ▸ Normalise to target** (Ctrl+N) is different, and more useful. It
measures the selection, compares it with the delivery target chosen in the
TARGET section of the meters panel, and applies the gain that gets there.

Two things are worth knowing about it:

- It applies the gain that moves towards the target **without pushing the true
  peak through the target's ceiling**. Where that is less than the gain needed
  to hit the target exactly, the meters panel says so: *"apply 4.2 dB (6.8 dB
  would hit the target but breach the ceiling)"*. The remaining 2.6 dB would
  have to come from limiting rather than from turning up, and the panel does not
  quietly do that for you.
- If the measurement has not finished — it is deferred slightly, so that
  dragging a selection does not restart it on every mouse movement — the action
  waits for it rather than doing nothing.

The targets, and what they are:

| Target | Integrated | True-peak ceiling | Tolerance |
| --- | --- | --- | --- |
| Spotify | −14 LUFS | −1 dBTP | ±1 LU |
| Apple Music | −16 LUFS | −1 dBTP | ±1 LU |
| YouTube | −14 LUFS | −1 dBTP | ±1 LU |
| Amazon Music | −14 LUFS | −2 dBTP | ±1 LU |
| Tidal | −14 LUFS | −1 dBTP | ±1 LU |
| Podcast | −16 LUFS | −1 dBTP | ±1 LU |
| EBU R128 | −23 LUFS | −1 dBTP | ±0.5 LU |
| ATSC A/85 | −24 LUFS | −2 dBTP | ±2 dB |

EBU R128 is the default, because it is the broadcast baseline the others are
described against. The tolerances for EBU R128 and ATSC A/85 come from those
standards. **The streaming services publish a target but no tolerance** — they
simply normalise on playback — so the ±1 LU shown against them is this
program's display convention and not something the platform specifies.

### 3.4 Filtering, limiting and dynamics

All of these obey the selection, and all are undoable.

**Process ▸ Filter** (Ctrl+F) offers three shapes: high-pass (to lose rumble),
low-pass (to lose hiss), and a peak or dip at a frequency you name. These three
are what corrective editing actually reaches for; a high-pass at 80 Hz is the
single most-used filter in repair. Spectral attenuation (§4.1) can take a band
out, but it cannot roll one off smoothly, and rumble wants a slope rather than a
hole.

Filtering part of a file leaves a step at each end of the range — inside it the
removed band is gone, outside it is still there. The program gives the filter a
run-up outside the selection so it starts settled rather than clicking, and
blends across a couple of periods of the corner frequency at each edge so the
step is spread over the wavelength that caused it. That costs the very edges of
the range: an 80 Hz high-pass gives up about 25 ms at each end, a 12 kHz
low-pass a fraction of a millisecond. An edge with nothing beyond it has no step
to hide and is filtered to the last sample.

**Process ▸ Limiter** (Ctrl+Shift+L) asks for a ceiling in dBTP and holds it. It
uses the offline limiter, which oversamples the signal path, then measures the
result exactly and trims what is left over — so the ceiling is held rather than
approached. It has look-ahead, so it is given a run-up and a run-out and the
delay is undone afterwards.

**Process ▸ Compressor** and **Process ▸ Gate** open a small form. Defaults:

| Compressor | | Gate | |
| --- | --- | --- | --- |
| Threshold | −20 dB | Threshold | −40 dB |
| Ratio | 4 : 1 | Hysteresis | 3 dB |
| Attack | 10 ms | Attack | 1 ms |
| Release | 100 ms | Hold | 10 ms |
| Knee | 6 dB | Release | 100 ms |
| Makeup gain | 0 dB | Depth | −80 dB |

A stereo pair shares one sidechain, so the image cannot move under compression.
Both get a run-up over the audio before the selection, so a passage does not
open with a burst of the untreated signal, and both blend at the edges so the
gain they settled on does not meet the untouched audio as a step.

The gate's depth is finite on purpose. A gate that mutes completely makes its
own action more obvious than the noise it removed.

### 3.5 Time and pitch

**Process ▸ Time stretch** asks for a new length *as a percentage of the current
one* — 200 is twice as long, 50 is half — rather than as a factor, because the
job is usually "this take has to fit that slot" and a factor makes you do the
division in your head. Pitch stays put. Everything after the stretched range
moves, which is the point of a stretch.

**Process ▸ Pitch shift** asks for semitones, fractions allowed; 0.01 of a
semitone is a cent, which is what a tuning fix actually needs. The length does
not change and nothing downstream moves.

A stretch is accepted between 10 and 1000 percent, and a shift up to three
octaves either way.

Both are a phase vocoder, which is thousands of transforms; on a long selection
the window says *"Stretching…"* and will not respond until it is done.

**A warning you will occasionally see.** Rebuilding a waveform from
reconstructed phases does not reproduce the original crest, so material that was
already close to the ceiling can come back over it — by about a tenth of a
decibel on the material this was measured on. When that happens the status bar
says *"it now peaks over full scale, so limit it before exporting"*. A tenth of
a decibel is the difference between a clean export and a clipped one, so you
should hear it from the program rather than from the file.

### 3.6 Channel operations

**Process ▸ Reverse**, **Invert polarity**, **Swap left and right** and **Sum to
mono**. Each obeys the selection, so they work on a passage as well as on a
whole file, and each is exact to the sample. Swap and Sum to mono need a stereo
file and say so if the document is not one.

### 3.7 Fades, flatten and markers

**Process ▸ Fade in** / **Fade out** fade across the whole selection.
**Process ▸ Fade shape** picks the curve once and leaves it: linear (the
default), equal power, logarithmic, exponential or S-curve. Equal power is the
one that does not leave a hole where two fades meet on uncorrelated material.

A fade runs from a clip's own edge, so a range spanning several clips is
flattened first — carrying one curve across several clips is not expressible,
and letting each clip restart the fade would be audible as a series of dips.

**Process ▸ Flatten** replaces the selected range with a single clip holding
what it currently sounds like, baking in the gains and fades inside it. It is
the one deliberately destructive operation in the model.

**Markers** (the Markers menu):

| Action | Shortcut |
| --- | --- |
| Add marker | Ctrl+M |
| Rename nearest marker | |
| Delete nearest marker | |
| Next marker | Alt+Right |
| Previous marker | Alt+Left |
| Clear all markers | |

Ctrl+M drops a point marker at the caret, or a **region** marker over the
selection if there is one — the length is what distinguishes them, and you have
already shown the program which you meant. Walking onto a region marker selects
it, because a region marker exists to be acted on. Markers are kept in time
order however they were added, and they are saved in sessions.

### 3.8 Exporting

**File ▸ Export** (Ctrl+Shift+S) writes the whole document.
**File ▸ Export selection** writes the selection. Both write WAV.

**File ▸ Export format** is a standing setting, not a question asked on every
save: 16-bit, 24-bit, or 32-bit float. 24-bit is the default.

**File ▸ Dither** chooses what to do about the bits a 16-bit export drops: none,
triangular, or triangular with noise shaping. Triangular is the default and is
the right answer almost always.

Dither matters because rounding float audio to a fixed number of bits without it
produces error that is correlated with the signal — which is distortion, heard
on a fading tone as a granular decay rather than a clean one. Adding a small
amount of noise first makes the error independent of the signal, so it is heard
as a steady hiss instead, and lets the audio carry information below its own
least significant bit. It costs about 4.8 dB of noise floor at 16 bits, which is
not a close trade. Noise shaping moves that noise towards the top of the band
where the ear is least sensitive.

**In the window, dither is applied only to a 16-bit export.** A 24-bit or float
export is never dithered, whatever the menu says, and that is deliberate: the
noise it would remove at 24 bits sits 144 dB down, and dithering a 24-bit export
would mean that opening a file and exporting it untouched gives back a different
file. As it stands, a round trip at the file's own depth comes back byte for
byte.

(The command-line driver behaves differently and honours `--dither` at 24 bits
as well — see §6.3. There it is typed out per invocation rather than standing,
so an explicit request is honoured.)

### 3.9 Sessions

**File ▸ Save session** (Ctrl+S) writes a `.sa` file: a JSON description of the
arrangement, referencing the audio files by path, relative to the session file
where possible so a project folder can be moved or handed on.

Audio that was generated while editing — a paste, a flatten, a repair — exists
only in memory, so saving a session **consolidates** it: the generated audio is
written out beside the session file, and the status bar says how many files were
written. Without that a session would reopen intact and silent, which looks like
it worked.

If a referenced file has moved, the session still opens: the clips are there,
backed by silence of the right length, and the status bar reports how many
sources are missing. An arrangement is not destroyed because one file moved, but
you are told, because otherwise you would mix a session with a hole in it.

### 3.10 Undo

**Ctrl+Z** and **Ctrl+Y**. The Edit menu names the edit each one would reverse
or reapply — *"Undo remove clicks"*, *"Redo equal power fade in"* — rather than
just "Undo".

History is linear: committing an edit after an undo discards the redo branch.

---

## 4. Repair

Everything here obeys the selection, is undoable, and reports what it found.
Several of them report *nothing found*, which on clean material is the right
answer and not a failure.

### 4.1 Attenuate and heal

These two are the reason the spectrogram is an editing surface. Draw a box round
the thing you do not want — a siren bleeding through a take, a squeak, a cough,
a click — and act on it.

**Repair ▸ Attenuate selection** (Ctrl+R) asks how many dB to take out of the
box and takes them out. This is the workhorse. Because the mask is tapered at
both the frequency and the time edges, what you hear is an absence rather than
an event; a rectangular hole in a spectrogram rings at its frequency edges and
clicks at its time edges.

**Repair ▸ Heal selection** (Ctrl+H) replaces the box with content interpolated
across it in time: magnitude interpolated between the frames on either side, and
phase advanced from the left edge at each bin's own frequency, so a steady tone
running through the gap continues in phase rather than restarting.

**Heal reconstructs; it does not invent.** Where the neighbouring frames carry
nothing — a gap wider than the event, or a region at the very start of the file
— the result fades towards silence rather than fabricating material. A plausible
fabrication in a repair tool is worse than an audible absence.

### 4.2 Noise reduction

Two steps, in this order:

1. Select a passage of **noise alone** — a gap between phrases, the run-up
   before the take, anything with nothing in it but the thing you want gone —
   and press **Repair ▸ Learn noise profile from selection** (Ctrl+Shift+N).
2. Select what you want cleaned and press **Repair ▸ Reduce noise** (Ctrl+D). It
   asks how many dB to push the noise floor down; 12 is the default.

Broadband denoise cannot work from a model of noise in general, because "noise"
is whatever you did not want: tape hiss, a fan, a preamp, rain on a window. It
works from a model of *this* noise. That is why you have to choose the passage —
and why **a profile learned from a passage containing speech will remove
speech.**

The reduction figure is a limit rather than a subtraction: a bin can be
attenuated by at most that much, so setting it high does not turn quiet passages
into holes.

Reduce noise cleans the whole spectrum of the selected time span, not just the
frequency band of the box. A noise profile describes a whole spectrum, and
applying it to a slice of one would leave the rest of the hiss in place with a
step where the band ended.

### 4.3 Clicks

**Repair ▸ Remove clicks** (Ctrl+Shift+C) fits a model to the material, finds
the samples that model cannot account for, and interpolates across them.

It asks for a **sensitivity**: how far above the passage's own noise a sample
has to be before it counts as damage, from 2 to 20, default 5. Lower finds more
and repairs more that did not need it. Measured on clean material, 5 finds
nothing in a tone, in noise or in a mixture; at 3 it begins finding the loudest
few percent of ordinary noise.

It reports how many clicks it repaired, and — importantly — **how many stretches
were too long to repair and were left alone.** That second number matters: a
stretch too long to be a click is a dropout, and wants a different tool. Three
clicks and three thousand call for different next steps, which is why the count
is shown rather than a tick.

### 4.4 Clipping

**Repair ▸ Restore clipped peaks** finds flat tops where a converter or a
limiter took a peak off, fits a model round each one and puts the peak back.

It reports how many it restored, and **how far it had to bring the file down so
that the restored peaks fit** — a repair that quietly changes the level of a
master is not a repair you can trust. It also reports flat stretches that were
too long to be peaks and were left alone.

If it finds nothing, it says so.

### 4.5 Mains hum

**Repair ▸ Remove mains hum** looks for a harmonic series near 50 Hz and near 60
Hz — the two that exist — measures each partial's amplitude and phase, and
subtracts them.

It finds the frequency itself, to a hundredth of a hertz, because a 50 Hz mains
can be at 49.93 and a tenth of a hertz matters by the fortieth harmonic. It
reports the frequency it found, how many partials it removed and how much energy
that took out.

Finding no hum is success, not failure, and it says so.

### 4.6 De-essing

**Repair ▸ De-ess** compresses the sibilance band and leaves the rest of the
voice alone. The form asks for the frequency sibilance starts at (5000 Hz by
default), a threshold (−30 dB), a ratio (6 : 1) and the most it may ever remove
(12 dB).

It reports **how much it took off and on what fraction of the selection**, and
that second figure is the one to read: "nothing happened" and "it worked" look
identical in a waveform, and the fraction acted on is what says whether the
threshold is anywhere near right. If it acted on 90% of the passage the
threshold is too low; if it acted on 1% it is too high.

The 12 dB limit is a stop rather than a setting. Past about that much a de-esser
stops removing sibilance and starts removing the consonant, which is worse than
the problem.

---

## 5. Analysis, and when to distrust it

This is the part of the product that is worth reading carefully. Every
measurement below has conditions under which it is meaningless, and the program
has been built to say so rather than to print a number anyway. What follows is
what each figure means and what makes it untrustworthy.

### 5.1 Loudness (LUFS)

**What it is.** Described in §2.6. The signal is K-weighted — a shelving filter
and a high-pass that together approximate the ear's frequency response —
squared, and averaged over 400 ms blocks. Blocks below an absolute gate of −70
LUFS are discarded, and then blocks more than 10 LU below the average of what
remains are discarded too. The gating is what makes the number match a
listener's judgement: without it, the silence between movements would drag a
symphony's loudness below a pop record's.

**Momentary** is 400 ms, **short-term** is 3 seconds, **integrated** is the
gated average of everything.

**When to distrust it.**

- **It is not certified.** No official EBU or ITU conformance vectors have been
  run against this meter, because those files are not available to the project.
  The arithmetic is checked against the published definitions and anchored at
  1 kHz — a 1 kHz stereo sine at −23 dBFS reads −23.0 LUFS — and measuring,
  normalising, exporting and re-measuring lands on −23.000 LUFS against EBU
  R128's −23.0. But internally consistent is not the same as conformant. See §7.
- **A window that has not filled reports nothing.** A momentary reading needs
  400 ms behind it; a short-term reading needs three seconds. A figure taken
  over half a window is a different measurement, not a smaller one, and it is
  reported as `--` rather than as a number.
- **Integrated loudness of a very short selection is `--`,** because nothing
  cleared the gate. This is correct, not broken.
- **The loudness range needs material to have a range.** On a two-second
  selection it means nothing.

### 5.2 True peak

**What it is.** The highest level the *reconstructed* waveform reaches, not the
highest sample. Sample peak is not what a digital-to-analogue converter or a
lossy encoder sees: reconstructing the signal between the samples routinely
finds another 1 to 3 dB, and a master that measures 0.0 dBFS can clip a D/A
stage or an MP3 decoder hard. Reported in dBTP.

**When to distrust it.**

- **The figure shown for a file is exact** — it is measured by band-limited
  reconstruction, which has no filter to droop, and it agrees with an
  independent implementation to two decimal places. The panel and the
  command-line driver both use it.
- **The real-time meter, used where an answer has to arrive within a block,
  under-reads by up to 0.44 dB** on bright transients at 4× oversampling. This
  is a windowed-sinc substitute for the filter table BS.1770-4 specifies, which
  is not reproduced in this project. Under-reading is the dangerous direction —
  it tells you that you are under the ceiling when you are over it — so it is
  written down here rather than buried.
- Very large files fall back to the streaming meter rather than the exact one.
  The command-line driver's JSON and CSV output says which was used
  (`truePeakExact`).

### 5.3 Peak to loudness (PLR) and PSR

**What it is.** True peak minus integrated loudness: the headroom that survives
loudness normalisation, and the number that decides whether a platform will turn
your master down and leave it sounding flat.

**When to distrust it.** PLR and PSR are **industry practice, not standards.**
Implementations differ over which windows they use. This program uses three
seconds for both PSR operands. A reading here is reproducible against itself and
is **not comparable with a tool that chose otherwise.**

### 5.4 Crest factor, RMS and DC offset

**Crest factor** is sample peak over RMS, in dB: a measure of how much transient
is left. A heavily limited master sits near 6 dB, an unprocessed acoustic
recording near 20.

**DC offset** is the mean sample value of whichever channel is furthest from
zero, sign kept. It is not averaged across channels, because +0.1 on the left
and −0.1 on the right would cancel to a clean zero and hide precisely the fault
being looked for.

These are ungated and unweighted. They describe the samples, not the loudness.

### 5.5 Stereo field

Four numbers that between them answer the question anyone mastering asks: is
this going to survive being played in mono. It will be played in mono somewhere
— a phone speaker, a club sum, a supermarket ceiling — and a mix with the bass
slightly out of phase loses its bass entirely when it happens. The fault is
inaudible in stereo, which is why it needs a meter rather than ears.

| | |
| --- | --- |
| **Correlation** | −1 to +1. +1 is two identical channels, which is mono. 0 is what a wide natural recording reads. Negative is the warning |
| **Width** | Side energy over mid energy in dB. Around −10 dB is a conventional mix; above 0 dB there is more disagreement between the channels than agreement |
| **Balance** | Right over left in dB, shown as "centred" or "2.1 dB left" |
| **Mono sum** | How much level the programme loses when summed to mono, and the actionable one |

**Reading mono sum.** Two identical channels give 0 dB. Uncorrelated channels of
equal power give −3 dB, which is arithmetic rather than damage. Anything much
below −3 dB is cancellation, and a reading near the floor means the mix
disappears.

**When to distrust it.** The section is hidden entirely for anything that is not
a stereo pair, rather than shown with zeroes. Zeroes in a meter invite a
comparison against them.

### 5.6 Key

**What it is.** The spectrum is folded onto the twelve pitch classes — throwing
away which octave a note was in and keeping only which note it was — averaged
over the passage, and compared against what each of the twenty-four keys would
be expected to produce.

The profiles are written from music theory (the tonic triad weighted above the
rest of the scale, the scale above the notes outside it) and are stated in full
in the source. They are **not** the published probe-tone profiles from the music
psychology literature, which would very likely do better on real music;
transcribing a table of constants from memory is how a tool ends up quietly
wrong.

**The panel refuses to name a key rather than naming its best guess.** This is
the single most important thing about it. A detector like this always has a
best-fitting twenty-fourth, and a best-fitting twenty-fourth of nothing is still
a key name. Two refusals guard against that:

- If the twelve notes are used near-evenly — a **contrast** below 0.15 — the
  panel says **no key**, because there may be none to find. Atonal music, a
  field recording, a drum loop and a spoken-word file all land here.
- If no profile fits well enough — a **strength** below 0.20 — the panel says
  **no key** and gives the strength.

Where a key *is* named, two more things are shown:

- **The runner-up**, always. Relative major and minor share all seven notes, so
  telling them apart rests on which is emphasised rather than on which notes
  occur — and that is the confusion to expect. A small margin against the
  relative minor means something quite different from a small margin against an
  unrelated key.
- **The tuning offset** from A = 440, in cents, shown when it is more than 5
  cents. Past 25 cents the answer carries a caveat, because the fold assumes
  twelve-tone equal temperament and a recording a quarter-tone flat smears every
  pitch class into its neighbour before the profiles ever see it.

**Other limits.** One key is reported for the whole passage. Music that
modulates has more than one, and you will get whichever dominates. In the window
the key is taken from the first 60 seconds of the selection.

### 5.7 Tempo, and why the beat grid exists

**What it is.** An onset strength envelope — how much the spectrum grew, frame
by frame — is autocorrelated: a record whose onsets repeat every half second
correlates with itself at half a second. The period is found first from the
whole passage, then a phase is fitted at that period, and then both are refined
together by least squares against the onsets the coarse grid landed on.

**The octave problem, which is the whole reason for the grid.** 60, 120 and 240
beats per minute predict onsets at exactly the same instants. Nothing in the
signal separates them. **119.8 and 239.6 BPM are both correct answers about the
same record** — one has a beat where the other has every other beat — and no
panel can tell you which one you meant.

Something has to break the tie, so each candidate period's correlation is
multiplied by a Gaussian in log-tempo centred on the geometric middle of the
requested range, which is 110 BPM for the default 60–200. Of 60, 120 and 240,
this prefers 120. **That is a preference, not a measurement**, chosen because
tapping along to most music lands between 80 and 160.

This is why the beat grid is drawn over the waveform and why it is on by
default. A tempo is the one number in this product you can check by eye: lay the
grid over the transients and you will see in a second whether it is on the beat,
on the half-beat, or drifting. Without it, the figure has to be taken on trust.

**When to distrust it.**

- **A dashed grid means the tracker is not confident.** Below a confidence of
  0.35 the panel names the tempo but says the onsets repeat only weakly at that
  period, and tells you to judge the grid against the transients. The grid is
  drawn dashed so that a proposal does not look like a measurement.
- **"No tempo" is an answer, not an error.** A held chord, a field recording or
  a spoken-word file has no tempo, and the tracker reports none rather than
  finding one. It specifically tests that the envelope looks like *events* —
  rhythm is a few loud moments among many quiet ones — because an unresolved low
  tone pulses at its own frequency and correlates with itself perfectly.
- **One tempo and one phase for the whole passage.** A piece that changes tempo,
  or a performance with rubato in it, gets an average and a poor confidence
  rather than a curve. There is no tempo curve.
- **No metre and no downbeat.** Bar lines are not found, only beats. Every beat
  in the grid is drawn alike.
- If you know roughly where the answer should be, narrowing the range helps —
  from the command line, with `--min` and `--max`.
- Nothing here has been run against the MIREX beat-tracking sets. What is
  claimed is that on material whose beat times are known by construction, it
  recovers them to within one analysis hop.
- In the window, the tempo is taken from at most the first 120 seconds of the
  selection.

### 5.8 Pitch contour

**What it is.** YIN: the period of the signal is found by asking how badly it
matches a delayed copy of itself and taking the shortest delay that matches
well. The panel shows the **median of the voiced frames** in hertz, and how many
frames were voiced.

**When to distrust it.**

- **It is monophonic. Given two notes at once it reports one of them, and which
  one is not defined.** The panel says this permanently under the PITCH section
  rather than as a caveat about one result, because it is true of every contour
  it draws. On a chord, a mix, or anything with more than one voice, the reading
  is a reading of *something* and you cannot tell what.
- **It finds a period, not a note.** The figure is shown in hertz and never as a
  note name, because there is no mapping from hertz to a name here, no decision
  about where one note ends and the next begins, and no vibrato or portamento
  model. Printing "A4" would claim something nothing measured.
- **Frames with nothing periodic in them report nothing, not zero,** so the
  contour breaks across them instead of ruling a line through noise. If fewer
  than a quarter of the frames were voiced, the panel says so: the median above
  is then taken over very little.
- **The range is 50 Hz to 1000 Hz.** A frame pitched outside it is reported as
  having no pitch, which is the honest answer — the alternative is reporting the
  first multiple of the period that *does* fall inside, which is an octave error
  with a range check wrapped round it.
- **A long selection is tracked more coarsely.** The hop grows with the length,
  up to eight times the default, so a two-minute passage has a coarser contour
  than a phrase. The command-line driver reports the hop; the window uses it to
  scale the drawing.

### 5.9 Third-octave bands

**What it is.** Energy in thirty-one bands (or ten, at octave spacing), which is
the oldest way of describing a spectrum and still the one people talk in. An FFT
answers a finer question and is harder to read: a bin is the same width in hertz
everywhere, which is a hundredth of an octave at the top and most of one at the
bottom, so a flat noise spectrum slopes on a log axis and a listener's "bass"
spans four hundred bins.

Levels are corrected for the analysis window's noise bandwidth so that a
full-scale sine inside one band reads 0 dBFS however many bins it covers.

**When to distrust it.**

- **This is not IEC 61260 and does not claim to be.** A certified sound level
  meter contains a filter bank with tolerance masks; this integrates bins from a
  transform. That is exact for steady material, cheap, and free of the settling
  time a bank of steep filters has — the right trade for looking at a recording,
  and the wrong one for certifying a measurement. The masks are not in this
  project and the response has never been checked against them.
- The command-line `bands --filters` option runs a real Butterworth filter bank
  instead, which is closer to what a sound level meter does. **The two do not
  agree and neither is wrong**: on white noise the filters read about a decibel
  higher, because a real filter's skirts reach into its neighbours and a
  rectangular band does not. They also label the bands differently — the
  integration prints the preferred numbers people say aloud (125, 250, 16000)
  and the filter bank prints the exact centres those names stand for (125.89,
  251.19, 15848.9). They are the same bands, but joining two such reports on the
  centre column will not line up.

### 5.10 Room acoustics

**Analyse ▸ Measure as an impulse response.** The menu entry is named for what
it assumes rather than for what it reports, because that is what you need to
know before pressing it.

**What it is.** Everything here is derived from the Schroeder integral: the
energy still to come at each moment, rather than the energy at it. An impulse
response is noise with a shape, so reading a decay slope off it directly gives a
different answer every time; integrating backwards from the end turns it into a
smooth monotonic curve whose slope is the decay rate.

| Figure | What it says |
| --- | --- |
| **EDT** | Early decay time: the 0 to −10 dB slope extrapolated to 60 dB. What a listener hears as reverberance |
| **T20** | Reverberation time from the −5 to −25 dB slope, extrapolated ×3 |
| **T30** | The same from −5 to −35 dB, extrapolated ×2. Better when the measurement has the range for it |
| **C50** | Clarity: early over late energy split at 50 ms, in dB. Speech |
| **C80** | The same split at 80 ms. Music |
| **D50** | Definition: the fraction of energy in the first 50 ms |
| **Centre time** | The centre of gravity of the energy, in seconds. "How smeared", with no arbitrary split point |
| **Usable decay** | How far the decay actually fell before hitting the noise floor |

T20 rather than a literal T60 because a genuine 60 dB of clean decay needs an
impulse 60 dB above the room's noise floor, which almost no real measurement
has. Starting at −5 dB skips the direct sound and the first reflections, which
are not part of the exponential tail. A room where EDT and T30 disagree is a
room with strong early reflections, and that disagreement is useful signal
rather than error.

**When to distrust it.**

- **These figures are meaningful only for an impulse response, and are nonsense
  for music.** That is not a hedge; it is what the measurement is. If you point
  it at a song you are measuring the decay of the arrangement, not of a room.
  The panel refuses outright for material with no discernible decay — *"not an
  impulse response, or too short to measure"* — but it cannot refuse everything
  that is not one, so the judgement is yours.
- **A figure the recording has no range for is shown as `--`, never as zero.**
  Measuring T30 needs the decay to fall 35 dB clear of the noise, and most
  impulse responses do not manage it. A zero here would read as a very dead
  room. The **Usable decay** row is what says which figures the record can
  support, and the panel states it in words: *"the decay fell 27 dB before its
  own noise floor, which supports T20 but not T30"*.
- **Not claimed to be ISO 3382 conformant.** The definitions are implemented
  from their arithmetic and no certified reference material has been run against
  them. What is checked is that on a synthetic decay whose rate is known
  exactly, they recover it.
- **A single reverberation time is an average over something that is not flat.**
  A concert hall is lively at 125 Hz and dead at 8 kHz; a room with soft
  furnishings is the other way round; the two problems are fixed with different
  materials. A single T30 says "reverberant" without saying what to do about it.
  Per-band figures are available from the command line
  (`auscultate-cli room impulse.wav --bands`), and are how a room is actually
  described.
- The per-band measurement filters the impulse response with two biquad sections
  per band, which is a gentler skirt than the standard asks for. A decay leaking
  in from a neighbouring band biases that band's reverberation time towards its
  neighbour's. On material where adjacent bands decay at similar rates — most
  rooms — the bias is small.

To make an impulse response in the first place, see `sweep` and `deconvolve` in
§6.

### 5.11 How much is actually analysed

The analysis panel holds audio whole, so it is bounded: **one run reads at most
120 seconds** from the start of the selection, and **the key is taken from the
first 60 seconds** of that. The tempo is the greediest of the answers and two
minutes is well past the point where another bar changes it; the key settles
inside one minute.

When less was read than was asked about, the panel says so — *"read the first
2:00 of 5:30"* — and a vertical line is drawn across the waveform where the
analysis stopped. A beat grid that ends two minutes into a five-minute file is a
bound, not a failure, and the picture has to say which.

The spectrum panel is bounded differently: it samples the selection in stretches
spread across it, up to about 24 stretches of 2.7 seconds each, with a segment
boundary between them so the joins are not analysed as audio. Reading two hours
to compute an average spectrum would make selecting anything feel broken and
would say the same thing.

Both the spectrum and the band measurement use **the first channel only**. A
spectrum of a stereo sum shows a comb wherever the two channels disagree in
phase, which is a picture of the summing rather than of the material.

---

## 6. The command-line driver

`auscultate-cli.exe` sits in the same folder as the application. It uses no Qt
and opens no window: it is for scripts, batch jobs, and the measurements that
are easier to read as text than as a panel.

Run it with no arguments, or with `--help`, for the built-in usage text. That
text is the authority; this section describes the same commands in more detail.

**Every command returns 0 on success and 1 on failure, and writes nothing to
standard output on failure**, so it composes in a script.

### 6.1 Argument conventions

- Options are `--name value` or `--name=value`. Bare flags such as `--json` take
  no value.
- **Put the file names before the flags.** The parser takes the word after
  `--name` as that option's value unless it begins with `--`, so
  `auscultate-cli analyse --json track.wav` swallows the filename as the value
  of `--json` and then complains that no file was given.
  `auscultate-cli analyse track.wav --json` is correct.
- `--format 16`, `--format 24` or `--format float` chooses the output sample
  format for any command that writes a file. The default is to keep the input's
  format, except where noted.
- File names containing spaces need quoting, as do target names such as
  `"EBU R128"`.

### 6.2 Measurement and reporting

#### `analyse <file>... [--json | --csv]`

Loudness, peaks and statistics. Takes any number of files.

The plain output is a readable block per file. `--json` gives one JSON object
per file. `--csv` writes a header row and then one row per file, which is what
to point at a folder of deliverables and open in a spreadsheet. The columns are:

```
file, sampleRate, channels, seconds, format, integratedLufs, loudnessRangeLu,
maxShortTermLufs, maxMomentaryLufs, truePeakDbtp, truePeakExact,
samplePeakDbfs, rmsDbfs, crestFactorDb, dcOffset, stereoCorrelation,
stereoWidthDb, stereoBalanceDb, monoLossDb
```

A measurement that does not exist is an **empty cell** in CSV and **null** in
JSON, never a sentinel number. A mono file leaves the four stereo columns empty;
a file where nothing cleared the loudness gate leaves the integrated loudness
empty. A consumer that averages this output must not be handed −200 as though it
were a loudness.

`truePeakExact` says whether the true peak is the exact reconstruction or the
streaming meter's estimate — see §5.2. A compliance report that does not say
which is one nobody can check.

`--json` and `--csv` together is an error: they are two different reports.

#### `bands <file> [--octave] [--filters [--order <n>]] [--json | --csv]`

Energy in third-octave bands, or octaves with `--octave`. The plain output is a
bar chart in text, scaled to the loudest band over a 60 dB range.

`--filters` runs the audio through a real Butterworth band-pass per band instead
of integrating the transform, and `--order` sets the filter order in poles
(default 6; it must be even). §5.9 explains why the two methods disagree, and
why both labellings of the band centres are right.

Bounded, because a band average is a property of the programme: it reads at
most 5,760,000 frames, which is two minutes at 48 kHz and proportionally less
at a higher rate.

#### `contour <file> [--interval <s>] [--csv | --json]`

Loudness over time. An integrated figure says a master sits at −14 LUFS; the
contour says whether it sits there throughout or whether one loud chorus is
carrying the average.

Each point carries momentary (400 ms) and short-term (3 s) loudness, true peak,
PSR and crest factor. `--interval` sets the spacing, 0.1 s by default. `--csv`
gives every point.

**A cell is empty rather than zero where a window has not filled.** See §5.1.
PSR and PLR are industry practice rather than standards; see §5.3.

#### `key <file> [--channel <n>] [--json]`

What key the music is in. Reads the first minute, from the start — choosing a
"representative" stretch would be a guess presented as an analysis.

Prints the key, the strength, the runner-up and the tuning offset from A = 440.
It adds a note when the twelve notes are used near-evenly, and another when the
recording is more than 25 cents from concert pitch.

**It refuses on the same terms as the window.** Material whose contrast or
strength falls below the thresholds in §5.6 prints *no key*, with the strength
and contrast beside it and a note saying which of the two failed. In `--json`
the `key` field is `null` and a `worthNaming` flag says so outright, so a script
need not re-derive the thresholds to know whether the answer is one.

This was not always so: the command line used to name its best-fitting key
whatever the figures said, so the same recording could read *no key* in the
window and a confident-looking name on the command line. The thresholds now
belong to the measurement rather than to whichever interface is showing it.

#### `tempo <file> [--min <bpm>] [--max <bpm>] [--channel <n>] [--json]`

The tempo and where the beats fall. Reads the first two minutes.

`--json` gives every beat time, which is what a grid is for. Material with
nothing rhythmic in it prints *"no tempo found"* and exits 0, because that is an
answer rather than an error.

`--min` and `--max` are not only a filter: half and double a tempo predict
onsets at the same instants, and the range is half of what breaks the tie.
Narrow them if you know roughly where the answer should be. See §5.7.

#### `pitch-of <file> [--min <hz>] [--max <hz>] [--threshold <t>] [--channel <n>] [--csv | --json]`

Tracks the fundamental over time. Prints the median of the voiced frames and how
much of the file was voiced at all; `--csv` gives the whole contour, one row per
frame, **with an empty cell where nothing periodic was found rather than a
zero**.

Monophonic — see §5.8. Widening `--min` costs time on every frame, because the
lowest pitch sets how many samples each one has to read.

#### `null <reference> <other> [--no-align] [--no-gain-match] [--max-delay <n>] [--channel <n>] [--json]`

Subtract two recordings that should be the same and report what is left. This is
the forensic answer to "did that processing chain actually change anything". A
residual at the float floor means it did not.

The two files are aligned to the nearest sample, gain-matched, and subtracted.
It prints a verdict — *bit-identical*, *identical within the float floor*, or
*different* — the delay and gain it found, whether polarity was inverted, how
far the residual sits below the reference, where in time it is worst, and a
per-octave breakdown.

The band table prints the **reference level beside the residual**, because a
band where the reference is silent shows a large positive ratio that means the
opposite of what it looks like.

**When to distrust it.** The alignment is a whole number of samples and the gain
is one figure for the whole comparison, so anything that varies over time — a
drifting clock, a fader move, a compressor — will not null however right those
two numbers are. Neither will a fractional-sample offset or a difference in
group delay: an all-pass filter changes no magnitude at any frequency and can
still leave a residual louder than the material. A loud residual is evidence of
*a* difference, not evidence that anything was damaged. Nothing here is
weighted, gated or masked, so a residual 60 dB down may be inaudible or may be
one obvious click, and the two read the same.

The two files must be at the same sample rate; convert one first.

#### `provenance <file>... [--json]`

What the audio says about where it came from, as opposed to what its header
claims. A 24-bit WAV that was an MP3 an hour ago is still a 24-bit WAV, and the
only place the MP3 is still visible is in the samples.

It reports the frequency above which there is essentially nothing and how
sharply the spectrum stops there, and how many bits the file actually uses out
of the depth it declares — the latter exactly rather than as a guess. A 24-bit
file made by padding a 16-bit master reports 16.

**It does not say "this is an MP3", and that restraint is the point.** A brick
wall at 16 kHz is also what a deliberate low-pass looks like, and what a 32 kHz
source upsampled to 48 kHz looks like. What can be said from the samples is that
something with a very steep filter removed the top of the band. The conclusion
is left to you.

Material that has been through any processing at all fills every bit, so a
result equal to the declared depth means only that nothing is *provably* unused.
Float files are not given a bit depth, because the question does not apply.

Bounded like `bands`: at most 5,760,000 frames, two minutes at 48 kHz. What it
looks for is a property of the encode, which is the same everywhere in the file.

#### `room <impulse.wav> [--bands | --thirds] [--json]`

Reverberation and clarity from an impulse response: EDT, T20, T30, C50, C80, D50
and centre time. `--bands` gives a reverberation time per octave band and
`--thirds` per third-octave, which is how a room is actually described.

Everything in §5.10 applies, including that a figure the recording has no range
for prints `--` rather than being extrapolated from noise, and that this is not
claimed to be ISO 3382 conformant.

The per-band figures filter the impulse response per band, which is **not** the
same operation as the energy integration behind the `bands` command.

#### `sweep <out> [--rate <hz>] [--start <hz>] [--end <hz>] [--seconds <s>] [--level <dBFS>] [--fade <s>] [--format ...]`

Writes an exponential sine sweep to play into a room or through a loudspeaker.
Defaults: 20 Hz to 20 kHz over five seconds at −6 dBFS, written as float so the
excitation carries no quantisation noise of its own.

Exponential rather than linear because a loudspeaker's harmonic distortion then
deconvolves to *negative* times, arriving before the linear response instead of
smeared through it, where it can simply be cut off.

Firing a starting pistol in a room is the obvious way to get an impulse response
and a poor one: all the energy arrives at once, so the signal-to-noise ratio is
whatever a single instant can manage, and any loudspeaker asked to reproduce a
true impulse distorts badly. A sweep spreads the same measurement over seconds.

#### `deconvolve <recording> <out> [same sweep flags] [--keep <s>] [--channel <n>] [--format ...]`

Turns a recording of that sweep back into an impulse response.

**The sweep flags must match what `sweep` was given.** The deconvolution is only
valid against the sweep that was actually played.

`--keep` bounds the length kept, and is worth setting a little above the
reverberation time. The command prints how far the peak stands above the end of
the window, which is the number that says whether the measurement was loud
enough to trust. Then measure the result with `room`.

Playing the sweep and recording the room needs audio hardware; see §1.5. What
can be checked without it is that a sweep convolved with a known response and
then deconvolved gives that response back, and that is what the tests do.

### 6.3 Conversion and delivery

#### `convert <in> <out> [--rate <hz>] [--format 16|24|float] [--dither none|tpdf|shaped]`

Converts sample rate and format. Resampling uses the Kaiser-windowed-sinc
converter at its best quality: 141 dB stopband.

Streamed a block at a time, so a file's length costs disk rather than memory. A
same-rate, same-format conversion comes back bit-identical.

`--dither` applies before the bits are dropped, and only where they are: asking
for it on a float output writes the float file untouched. **It defaults to none
here, unlike the window**, because a file passing through this tool is usually
on its way somewhere else and dither belongs at the end of a chain rather than
at every step of one. Unlike the window, an explicit `--dither` at 24 bits *is*
honoured, because here it is typed out per invocation rather than standing.

#### `normalise <in> <out> --target <name> [--format ...]`

Measures, applies the gain that meets the target without breaching its true-peak
ceiling, and writes. Targets are the eight in §3.3, matched without regard to
case: `Spotify`, `Apple Music`, `YouTube`, `Amazon Music`, `Tidal`, `Podcast`,
`EBU R128`, `ATSC A/85`.

It prints what it measured, what it applied, and the target it was aiming at. A
file where nothing cleared the absolute gate is refused rather than normalised.

#### `render <session.sa> <out.wav> [--format ...]`

Renders a saved arrangement to audio. Missing sources are reported on standard
error and their clips are silent, as in the window.

### 6.4 Repair and processing

All of these write a new file and leave the input alone.

#### `denoise <in> <out> --noise <from>-<to> [--amount <dB>] [--format ...]`

Learns a noise profile from `<from>-<to>` in seconds — a passage of noise alone —
and then cleans the whole file. `--amount` defaults to 12 dB. Everything in §4.2
applies, including that a profile learned from a passage containing speech will
remove speech.

#### `dehum <in> <out> [--frequency <hz>] [--amount <0..1>] [--format ...]`

Finds mains hum and subtracts it. `--frequency` overrides the search; leaving it
out lets the command find 50 or 60 Hz itself, to a hundredth of a hertz.
`--amount` below 1 leaves some of the hum, which is occasionally what you want
when the removal takes something with it.

Prints the frequency it found and how many partials it removed. **Finding none
is success, not failure.**

#### `declip <in> <out> [--keep-level] [--format ...]`

Restores clipped peaks. The result is brought down so the restored peaks fit
unless `--keep-level` says otherwise, and the gain applied is printed.

#### `declick <in> <out> [--sensitivity <n>] [--format ...]`

Finds and repairs clicks. Prints how many it found and **how many stretches were
too long to be clicks and were left alone** — those are dropouts and want a
different tool. Default sensitivity 5; see §4.3.

#### `deess <in> <out> [--frequency <hz>] [--threshold <dB>] [--ratio <n>] [--max <dB>] [--format ...]`

Compresses the sibilance band and leaves the rest of the voice alone. Defaults
5000 Hz, −30 dB, 6 : 1 and at most 12 dB off. Prints how much it took and **on
what fraction of the file it acted**, which is the number that says whether the
threshold is anywhere near right.

#### `dereverb <in> <out> [--amount <dB>] [--decay <s>] [--floor <dB>] [--onset <s>] [--format ...]`

Takes some of the room back out of a take made in too live a one. It estimates
the late reverberant energy in each frequency bin from that bin's own recent
history and subtracts it. Defaults are 10 dB of removal assuming a 0.4 s decay.

Set `--decay` to roughly the room's reverberation time. **Over-stating it is not
a free way to remove more**: a sustained note is indistinguishable from its own
tail, so too long a setting starts eating the material and sounding like a gate.

**What it does not do: shorten the decay.** It scales the tail down and leaves
the slope alone, so T30 barely moves — judge it on EDT, C50 and D50. A discrete
echo is barely touched; this is for a diffuse tail, not a slapback.

`--floor` bounds how far a bin may be pushed down (default −20 dB). Dropping it
to −60 dB was measured to remove *less* reverberation, more raggedly: letting a
bin fall to nothing does not take out more of the room. `--onset` sets where the
late tail is taken to begin; the default of about 85 ms is past the 80 ms that
C80 splits at, and the early reflections before it are left alone deliberately
because removing them is what makes a de-reverb sound like a telephone.

#### `compress <in> <out> [--threshold <dB>] [--ratio <n>] [--attack <ms>] [--release <ms>] [--knee <dB>] [--makeup <dB>] [--no-link] [--format ...]`

Downward compression over the whole file. Defaults −20 dB, 4 : 1, 10 ms and
100 ms. A stereo pair shares one sidechain unless `--no-link`, so the image
cannot move.

#### `gate <in> <out> [--threshold <dB>] [--depth <dB>] [--attack <ms>] [--hold <ms>] [--release <ms>] [--hysteresis <dB>] [--no-link] [--format ...]`

Noise gate with hysteresis and hold. Defaults −40 dB open, 3 dB of hysteresis
and 80 dB of depth. Hysteresis is what stops a signal sitting on the threshold
switching the gate on and off continuously.

#### `stretch <in> <out> --length <percent> [--format ...]`

Changes how long it lasts without changing its pitch. 200 is twice as long, 50
is half. Accepted range is 10 to 1000 percent.

#### `pitch <in> <out> --semitones <n> [--format ...]`

Changes its pitch without changing how long it lasts. Fractions are allowed;
0.01 of a semitone is a cent. Accepted range is three octaves either way.

#### `channels <in> <out> --op reverse|invert|swap|mono [--format ...]`

The four edits that are pure arithmetic: play it backwards, flip its polarity,
exchange left and right, or put the average of the two channels on both. `swap`
and `mono` need a stereo file.

---

## 7. What this product does not claim

This section exists because the gap between what a tool claims and what it has
been shown to do is the whole difference between a measurement and a number.

**No measurement in this product has been verified against any certification
body's conformance material.**

- **No EBU R128 or ITU-R BS.1770 conformance vectors have been run.** The EBU
  Tech 3341 and Tech 3342 material and ITU-R BS.2217 are not available to this
  project. The loudness meter is implemented from the published definitions,
  checked against invariants that hold for any correct implementation, and
  anchored at 1 kHz; that is not conformance and is not described as
  conformance.
- **The true-peak meter does not use the filter BS.1770-4 Annex 2 Table 3
  specifies.** That table is not reproduced here. The figure shown for a file is
  measured by exact reconstruction instead, which is better rather than worse;
  the streaming meter used where an answer must arrive within a block
  under-reads by up to 0.44 dB on bright transients.
- **The band measurements are not IEC 61260.** The tolerance masks are not in
  this project and the response has never been checked against them.
- **The room measures are not claimed to be ISO 3382 conformant.** The
  definitions are implemented from their arithmetic; no certified reference
  material has been run against them.
- **PSR and PLR are not standardised at all.** They are industry practice, and
  implementations differ over the windows. The windows used here are stated so
  that a reading is at least reproducible.

**Therefore: do not use this as the sole basis for a delivery that carries a
contractual compliance requirement.** Check it against a tool that is certified.
This is the position the code takes, the position the licence agreement takes
(EULA §8), and it is repeated here so that nobody has to go looking for it.

Three further things that are true of this build:

- **Playback has never been run on real audio hardware.** See §1.5.
- **The software is unsigned**, which is why Windows warns about it. See §1.2.
- **It modifies files.** Keep backups of anything you care about. The
  application never writes over the file you opened, but an export can overwrite
  an existing file like any other program's can.

What *is* claimed is narrower and is stated where each measurement is
implemented: that the arithmetic matches the published definitions, that on
signals whose answer is known by construction the answer comes back, and that
where an answer cannot be established the program says so rather than printing
one.

---

## Appendix A — Keyboard shortcuts

Shortcuts shown as "standard" use the system's own key for that action, which
the menu displays. On Windows these are the familiar ones — Ctrl+O, Ctrl+S,
Ctrl+Z, Ctrl+Y, Ctrl+X/C/V, Delete, Ctrl+A.

### File

| | |
| --- | --- |
| Open audio | standard (Ctrl+O) |
| Open session | Ctrl+Shift+O |
| Save session | standard (Ctrl+S) |
| Export | standard (Ctrl+Shift+S) |
| Export selection | — |
| Export format ▸ 16-bit / 24-bit / 32-bit float | — |
| Dither ▸ None / Triangular / Triangular, noise-shaped | — |
| Quit | standard |

### Edit

| | |
| --- | --- |
| Undo | standard (Ctrl+Z) |
| Redo | standard (Ctrl+Y) |
| Cut / Copy / Paste | standard |
| Delete | standard (Del) |
| Silence selection | Ctrl+L |
| Trim to selection | Ctrl+T |
| Select all | standard (Ctrl+A) |
| Deselect | Escape |

### Process

| | |
| --- | --- |
| Gain | Ctrl+G |
| Filter | Ctrl+F |
| Limiter | Ctrl+Shift+L |
| Reverse / Invert polarity / Swap left and right / Sum to mono | — |
| Show EQ curve | Ctrl+Shift+E |
| Apply EQ curve | Ctrl+Shift+Q |
| Reset EQ bands | — |
| Compressor / Gate / Time stretch / Pitch shift | — |
| Normalise to target | Ctrl+N |
| Fade in / Fade out | — |
| Fade shape ▸ Linear / Equal power / Logarithmic / Exponential / S-curve | — |
| Flatten | — |

### Transport

| | |
| --- | --- |
| Play (or stop) | Space |
| Stop | Shift+Escape |

### Markers

| | |
| --- | --- |
| Add marker | Ctrl+M |
| Rename nearest marker | — |
| Delete nearest marker | — |
| Next marker | Alt+Right |
| Previous marker | Alt+Left |
| Clear all markers | — |

### Repair

| | |
| --- | --- |
| Attenuate selection | Ctrl+R |
| Heal selection | Ctrl+H |
| Learn noise profile from selection | Ctrl+Shift+N |
| Reduce noise | Ctrl+D |
| De-ess | — |
| Remove clicks | Ctrl+Shift+C |
| Restore clipped peaks | — |
| Remove mains hum | — |
| Select all frequencies | — |

### Analyse

| | |
| --- | --- |
| Beat grid over the waveform | Ctrl+Shift+B |
| Pitch contour over the waveform | Ctrl+Shift+P |
| Third-octave bands on the spectrum | Ctrl+Shift+T |
| Measure as an impulse response | — |

### View

| | |
| --- | --- |
| Zoom to fit | F |
| Zoom to selection | Ctrl+E |
| Frequency scale ▸ Logarithmic / Linear | — |
| Dynamic range ▸ −60 / −80 / −96 / −120 dB | — |
| Colour map ▸ Magma / Viridis / Greyscale | — |
| Set spectrum reference | Ctrl+Shift+R |
| Clear spectrum reference | — |

### Mouse

| | |
| --- | --- |
| Wheel | Zoom |
| Shift + wheel | Scroll |
| Middle-drag, Alt + left-drag | Pan |
| Left-drag | Select (time in the waveform, time and frequency in the spectrogram) |
| Shift + left-click | Extend the selection |
| Double-click | Select everything |

---

## Appendix B — Driving the window from a script

`auscultate.exe` accepts a small set of batch options, so the window itself can
be driven without a person. They are not a substitute for `auscultate-cli`,
which drives the engine with no Qt at all; they exercise the window's own wiring,
which is a different claim.

```
auscultate.exe <file> [options]
```

| Option | Effect |
| --- | --- |
| `--select <from>-<to>` | Select a span in seconds before `--apply` runs |
| `--apply <ops>` | Comma-separated operations, applied in order |
| `--export <wav>` | Write the edited document |
| `--save-session <file>` | Save the arrangement |
| `--print-analysis` | Print the measured loudness, peaks and markers on standard output |
| `--print-musical` | Print the key, tempo, contour and room figures, **with the text the panel decided to show for each** |
| `--play` | Play the selection to its end and report where the transport got to. Runs in real time |
| `--screenshot <png>` | Render the whole window and exit |
| `--screenshot-spectrogram <png>` | Render the spectrogram plot alone |
| `--screenshot-spectrum <png>` | Render the spectrum panel alone |
| `--screenshot-waveform <png>` | Render the waveform plot alone |
| `--help` | The built-in list |

A `.sa` argument is opened as a session rather than as audio.

**Use `select:` inside `--apply` rather than repeating `--select`.** Qt keeps
only the last value of a repeated option, so alternating `--select` and
`--apply` silently drops all but the final pair.

The operations `--apply` accepts:

| Group | Verbs |
| --- | --- |
| Selection | `select:<from>-<to>`, `selectall`, `deselect`, `band:<low>-<high>` |
| Editing | `cut`, `copy`, `paste`, `delete`, `silence`, `trim`, `undo`, `redo`, `flatten` |
| Level | `gain:<dB>`, `normalise`, `limit:<dBTP>` |
| Fades | `fadein`, `fadeout`, `fadein:<shape>`, `fadeout:<shape>` where `<shape>` is `linear`, `equalpower`, `logarithmic`, `exponential` or `scurve` |
| Filters | `highpass:<hz>`, `lowpass:<hz>` |
| Dynamics | `compress:<threshold>/<ratio>[/<attack_ms>/<release_ms>]`, `gate:<threshold>[/<depth_dB>]`, `deess:<hz>[/<threshold>/<ratio>]` |
| Time and pitch | `stretch:<percent>`, `pitch:<semitones>` |
| Channels | `reverse`, `invert`, `swapchannels`, `mono` |
| Repair | `attenuate:<dB>`, `heal`, `learnnoise`, `denoise:<dB>`, `declick`, `declick:<sensitivity>`, `declip`, `dehum` |
| Markers | `mark`, `mark:<label>`, `nextmarker`, `prevmarker`, `deletemarker`, `clearmarkers` |
| Export settings | `format:16`, `format:24`, `format:float`, `dither:none`, `dither:tpdf`, `dither:shaped` |
| Analysis overlays | `beatgrid`, `nobeatgrid`, `pitchcontour`, `nopitchcontour`, `bands`, `nobands`, `room`, `noroom`, `analysisprint` |
| Equaliser | `eq`, `eqhide`, `eqreset`, `eqapply`, `eqprint`, `eqadd:<hz>/<dB>`, `eqdrag:<index>/<hz>/<dB>`, `eqq:<index>/<notches>`, `eqshiftdrag:<index>/<dy>`, `eqremove:<index>` |
| Spectrum reference | `reference`, `clearreference` |

Slashes separate a verb's arguments rather than commas, because commas separate
the verbs themselves.

An unknown operation is an error and stops the run, with the offending verb
named on standard error.
