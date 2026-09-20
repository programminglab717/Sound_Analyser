# Auscultate — Quick start

One page. The full manual is [MANUAL.md](MANUAL.md).

## 1. Download and unzip

On the repository's **Actions** tab, open the most recent green run, scroll to
**Artifacts**, and download `auscultate-windows`. Unzip it anywhere — there is
no installer. Inside are `auscultate.exe`, the headless driver
`auscultate-cli.exe`, and the Qt DLLs they run on. Keep them together.

## 2. Run it, past the warning

Windows will say **"Windows protected your PC"** and offer only *Don't run*.
Click **More info**, then **Run anyway**.

That happens because the executable is **not code-signed** — a certificate is a
commercial product this build does not carry — and Windows shows the same
warning for every unsigned program it has not seen many times before. It is a
statement about the absence of a certificate, not a detection of anything.
Windows remembers your decision for that copy of the file.

## 3. Open something

**File ▸ Open audio** (Ctrl+O). WAV, AIFF, FLAC and MP3 all work. Open a real
recording rather than a test tone — the point of this program is what it tells
you about material that has something wrong with it.

## 4. Press Space

Playback runs from the caret, or over the selection if you have made one.
Space again stops it.

The status bar names the device it opened — *"Playing through …"* — or tells
you it could not open one and is playing silently. The playhead moves either
way, so it says which of the two happened rather than leaving you to guess.

Sound has been heard from this program on Windows. What has not been tried is
everything around that: other sample rates, exclusive-mode devices, a device
unplugged mid-playback.

## 5. The four things worth trying first

**Drag a selection.** Drag in the waveform for a span of time; drag in the
spectrogram for a span of time *and* a band of frequency. Everything on the
right — the meters, the analysis, the spectrum — follows the selection. With
nothing selected they describe the whole file. The wheel zooms, Shift+wheel
scrolls, middle-drag pans, and **F** fits the whole file to the window.

**Read the loudness meters.** The big number is integrated loudness in LUFS:
how loud the material actually sounds, as opposed to how big its samples are.
Pick a delivery target in the TARGET box at the bottom of the panel and the
meters will tell you how far off you are and what gain would fix it.
**Process ▸ Normalise to target** (Ctrl+N) applies that gain — and it applies
the gain that gets closest *without* pushing the true peak through the target's
ceiling, saying so when those two differ.

**Repair something you can see.** Find a click, a squeak or a band of hum on the
spectrogram, draw a box round it, and press **Ctrl+R** (attenuate it by however
many dB you ask for) or **Ctrl+H** (heal it — interpolate across it from the
audio either side). This is the thing the product is built around: you edit
where you see the problem.

**Check the beat grid.** The vertical lines over the waveform are where the
tempo tracker thinks the beats fall, and they are on by default. Look at whether
they sit on the transients. 119.8 and 239.6 BPM are both correct answers about
the same record, and nothing in the signal separates them — the grid is there so
you can settle it by eye in a second rather than trusting the number.

## 6. When a number is missing, that is the answer

`--` never means zero. It means *there is no measurement here*. The analysis
panel will say **no key** rather than naming its best guess when nothing fits,
**no tempo** when nothing in the passage is rhythmic, and `--` for a room
measure the recording had no decay range for. That restraint is deliberate and
it is the most useful thing the panel does. The manual explains each refusal.

## 7. Get it back out

**File ▸ Export** writes a WAV of the whole document; **File ▸ Export selection**
writes just the selection. **File ▸ Export format** picks 16-bit, 24-bit or
float, and **File ▸ Dither** decides what to do about the bits a 16-bit export
drops. Triangular is the default and is almost always right.

**File ▸ Save session** (Ctrl+S) saves the arrangement as a `.sa` file so you
can come back to it. Nothing you do in the window ever changes the file you
opened.

## 8. And from a script

`auscultate-cli.exe` is in the same folder and needs no window. Some starting
points:

```
auscultate-cli analyse *.wav --csv                 one row per file, for a spreadsheet
auscultate-cli key      track.wav                  what key it is in, and how sure
auscultate-cli tempo    track.wav --json           the tempo, and every beat time
auscultate-cli null     before.wav after.wav       what a processing chain actually changed
auscultate-cli normalise in.wav out.wav --target "EBU R128"
```

Put the file names before the flags. Run it with no arguments for the full list
of twenty-five commands.

---

**One thing to know before you rely on it.** No measurement in this program has
been checked against any certification body's conformance material — no EBU R128
or ITU-R BS.1770 vectors have been run. The arithmetic follows the published
definitions and is anchored where it can be, but that is not conformance. Do not
make it the sole basis for a delivery that carries a contractual compliance
requirement without checking it against a certified tool.
