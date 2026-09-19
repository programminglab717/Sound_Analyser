#!/usr/bin/env python3
"""End-to-end check that the window draws the analysis, and admits what it cannot.

Every probe below has its answer built into it: a click every half second is
120 BPM whatever the tracker says, a 440 Hz tone is 440 Hz, white noise has no
key and no tempo, and an exponential decay of a stated rate has that
reverberation time. So the question is never "did the library get it right" --
sa-analysis has its own tests for that -- but "did the window draw what the
library found, in the right place, and did it keep quiet about what the library
refused".

Four claims, none of which the other drivers can make:

  1. The beat grid over the waveform falls on the transients. A tempo is the
     one number in this product a reader can check by eye, and it is checked
     twice here: the drawn columns are compared against the beat times the
     window reports, and those beat times are compared against the clicks that
     are actually in the file. Either alone would pass on a grid that was
     consistently wrong.
  2. The pitch contour is drawn at the height its frequency maps to on the
     panel's own axis, and it *breaks* across the silence rather than ruling a
     line through it.
  3. Every third-octave band is drawn at the level it was measured at, band by
     band, against a level axis recomputed in this file.
  4. A doubtful answer says so and an unsupported one is never printed as a
     number. This is checked as a rule rather than a set of examples: across
     every probe, a key is named exactly when the strength and contrast allow
     it, and a room figure is a number exactly when its flag is true.

No third-party imports: the PNG decoder below is forty lines and Pillow is not
worth adding to a CI image for it.
"""

from __future__ import annotations

import argparse
import math
import random
import struct
import subprocess
import sys
import tempfile
import wave
import zlib
from pathlib import Path

SAMPLE_RATE = 48000

# The click track. A beat every half second is 120 BPM by construction, and the
# clicks are what the drawn grid has to land on.
CLICK_SECONDS = 8.0
CLICK_PERIOD = 0.5
CLICK_BPM = 60.0 / CLICK_PERIOD

# The tone. 440 Hz with a second of silence cut out of the middle, so that the
# contour has something to break across.
TONE_HZ = 440.0
TONE_SECONDS = 6.0
GAP_FROM = 2.0
GAP_TO = 3.0

# A tone a long way from concert pitch: 45 cents sharp of A = 440, which is
# past the 25 cents at which the panel stops trusting a key.
DETUNED_CENTS = 45.0
DETUNED_HZ = TONE_HZ * 2.0 ** (DETUNED_CENTS / 1200.0)

# The impulse responses. Both decay at the same rate; the second sits on a
# noise floor high enough that the decay runs into it before it has fallen the
# 30 dB a T30 needs, which is the case the room section exists to be honest
# about.
IR_T60 = 0.6
IR_SECONDS = 1.6
IR_QUIET_FLOOR = 1e-5
IR_NOISY_FLOOR = 0.012

# The panel's own axes, from sa/ui/ViewGeometry.h and sa/analysis/PitchTrack.h.
# The test has to know them to say where a row is, and stating them here is
# what makes the assertions arithmetic rather than tolerance.
TOP_DB = 0.0
BOTTOM_DB = -108.0
LABEL_STRIP = 18  # The frequency labels sit below the spectrum plot.
AXIS_LOW_HZ = 20.0
NYQUIST_HZ = SAMPLE_RATE / 2.0
PITCH_LOW_HZ = 50.0
PITCH_HIGH_HZ = 1000.0

# AnalysisReadout.h's Certainty, in the order it is declared.
FIRM, DOUBTFUL, NONE = 0, 1, 2

# The thresholds the panel decides by. Restated here so that the rule can be
# checked rather than the examples: a driver that only knew the examples would
# pass a panel that had stopped applying the rule between them.
KEYLESS_CONTRAST = 0.15
KEY_REFUSED_BELOW = 0.20
KEY_FIRM_AT = 0.50
TUNING_FAR_CENTS = 25.0
TEMPO_DOUBTFUL_BELOW = 0.35


# ----------------------------------------------------------------------------
# Probes
# ----------------------------------------------------------------------------


def write_wav(path: Path, samples: list[float]) -> None:
    data = bytearray()
    for value in samples:
        data += struct.pack("<h", max(-32767, min(32767, int(value * 32767))))
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(SAMPLE_RATE)
        handle.writeframes(bytes(data))


def write_clicks(path: Path) -> None:
    """A short decaying tone burst every CLICK_PERIOD seconds.

    A burst rather than a single sample because a one-sample impulse is a click
    to an onset detector and nothing at all to a display: the point of the file
    is that a reader can see the transients the grid is meant to sit on.
    """
    frames = int(SAMPLE_RATE * CLICK_SECONDS)
    samples = []
    for i in range(frames):
        t = i / SAMPLE_RATE
        phase = t % CLICK_PERIOD
        if phase > 0.04:
            samples.append(0.0)
            continue
        samples.append(0.8 * math.exp(-phase * 90.0) * math.sin(2.0 * math.pi * 1000.0 * t))
    write_wav(path, samples)


def write_tone(path: Path, hz: float, gap: bool) -> None:
    frames = int(SAMPLE_RATE * TONE_SECONDS)
    samples = []
    for i in range(frames):
        t = i / SAMPLE_RATE
        if gap and GAP_FROM <= t < GAP_TO:
            samples.append(0.0)
            continue
        samples.append(0.5 * math.sin(2.0 * math.pi * hz * t))
    write_wav(path, samples)


def write_noise(path: Path) -> None:
    """White noise: no key in it, no rhythm in it, and a flat power spectrum.

    All three matter. The first two are what the panel has to refuse, and the
    third makes the band chart a staircase -- a third-octave band is a fixed
    ratio wide, so each one holds twice the bandwidth of the one four bands
    below it and reads 1 dB per band higher. Adjacent bars therefore sit on
    different rows, and a chart that drew them all at one level would fail.
    """
    rng = random.Random(4242)
    frames = int(SAMPLE_RATE * 6.0)
    write_wav(path, [rng.uniform(-0.3, 0.3) for _ in range(frames)])


def write_impulse(path: Path, floor_amplitude: float) -> None:
    """Noise under an exponential envelope: a room with a known decay rate.

    The envelope falls 60 dB in IR_T60 seconds by construction, so EDT, T20 and
    T30 all have a known right answer. `floor_amplitude` is a noise floor laid
    under it, which is what decides how far the decay gets before it runs out
    of room -- and so which of the three figures the record can support.
    """
    rng = random.Random(12345)
    frames = int(SAMPLE_RATE * IR_SECONDS)
    lead = int(SAMPLE_RATE * 0.02)
    samples = []
    for i in range(frames):
        if i < lead:
            samples.append(rng.uniform(-1.0, 1.0) * floor_amplitude)
            continue
        t = (i - lead) / SAMPLE_RATE
        envelope = math.exp(-t * math.log(1000.0) / IR_T60)
        samples.append(rng.uniform(-1.0, 1.0) * envelope
                       + rng.uniform(-1.0, 1.0) * floor_amplitude)
    write_wav(path, samples)


# ----------------------------------------------------------------------------
# Reading the picture
# ----------------------------------------------------------------------------


def read_png(path: Path) -> tuple[int, int, list[list[tuple[int, int, int]]]]:
    """Decode a non-interlaced 8-bit PNG into rows of RGB triples."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path} is not a PNG")

    offset = 8
    width = height = channels = 0
    compressed = bytearray()
    while offset < len(data):
        (length,) = struct.unpack(">I", data[offset : offset + 4])
        kind = data[offset + 4 : offset + 8]
        payload = data[offset + 8 : offset + 8 + length]
        offset += 12 + length

        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", payload)
            if depth != 8 or interlace != 0:
                raise ValueError(f"unsupported PNG: depth {depth}, interlace {interlace}")
            channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(colour, 0)
            if channels == 0:
                raise ValueError(f"unsupported PNG colour type {colour}")
        elif kind == b"IDAT":
            compressed += payload
        elif kind == b"IEND":
            break

    raw = zlib.decompress(bytes(compressed))
    stride = width * channels
    rows: list[list[tuple[int, int, int]]] = []
    previous = bytearray(stride)
    position = 0

    for _ in range(height):
        filter_type = raw[position]
        position += 1
        line = bytearray(raw[position : position + stride])
        position += stride

        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            upleft = previous[i - channels] if i >= channels else 0
            if filter_type == 1:
                line[i] = (line[i] + left) & 0xFF
            elif filter_type == 2:
                line[i] = (line[i] + up) & 0xFF
            elif filter_type == 3:
                line[i] = (line[i] + (left + up) // 2) & 0xFF
            elif filter_type == 4:
                p = left + up - upleft
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - upleft)
                predictor = left if (pa <= pb and pa <= pc) else (up if pb <= pc else upleft)
                line[i] = (line[i] + predictor) & 0xFF
            elif filter_type != 0:
                raise ValueError(f"unknown PNG filter {filter_type}")

        rows.append(
            [
                (line[x * channels], line[x * channels + min(1, channels - 1)],
                 line[x * channels + min(2, channels - 1)])
                for x in range(width)
            ]
        )
        previous = line

    return width, height, rows


def is_beat_colour(pixel: tuple[int, int, int]) -> bool:
    """Whether a pixel belongs to the beat grid.

    The grid is the one magenta thing on the waveform: the audio is blue, the
    playhead amber, the selection a pale blue and the pitch contour green. The
    thresholds sit clear of all four -- the pale blue selection edge is the
    nearest and fails on green, which it has far too much of.
    """
    red, green, blue = pixel
    return red >= 170 and blue >= 220 and green <= red - 50


def is_contour_colour(pixel: tuple[int, int, int]) -> bool:
    """Whether a pixel belongs to the pitch contour, or to a band cap.

    Both are the same green, and they are never in the same picture: the
    contour is on the waveform and the bands are on the spectrum. The
    contour's own scale markings are a much darker green and fail on the first
    test, which keeps the axis out of the reading.
    """
    red, green, blue = pixel
    return green >= 150 and green - red >= 60 and green - blue >= 50


def marked_columns(rows, width: int, height: int, matches) -> dict[int, list[int]]:
    """Which rows of each column carry a mark, for the columns that have any."""
    found: dict[int, list[int]] = {}
    for y in range(height):
        line = rows[y]
        for x in range(width):
            if matches(line[x]):
                found.setdefault(x, []).append(y)
    return found


# ----------------------------------------------------------------------------
# The axes, recomputed here
# ----------------------------------------------------------------------------


def away_from_zero(value: float) -> int:
    """Round half away from zero, which is what std::lround does.

    Python's round() is half-to-even and would disagree with the drawing on
    exactly the values that land between two pixels.
    """
    return int(math.floor(value + 0.5)) if value >= 0 else -int(math.floor(-value + 0.5))


def time_column(seconds: float, total_frames: int, width: int) -> int:
    """The column an instant falls in on a view showing the whole document.

    Through the sample it belongs to first, which is what the window does: it
    rounds a time to the nearest sample and a sample to the nearest column.
    """
    sample = away_from_zero(seconds * SAMPLE_RATE)
    return away_from_zero(sample / total_frames * width)


def pitch_row(hz: float, plot_height: int) -> int:
    """The row a frequency is drawn at on the contour's logarithmic axis."""
    fraction = math.log(hz / PITCH_LOW_HZ) / math.log(PITCH_HIGH_HZ / PITCH_LOW_HZ)
    return away_from_zero((1.0 - fraction) * (plot_height - 1))


def level_row(decibels: float, plot_height: int) -> int:
    """The row a level is drawn at on the spectrum panel's level axis."""
    fraction = min(max((TOP_DB - decibels) / (TOP_DB - BOTTOM_DB), 0.0), 1.0)
    return away_from_zero(fraction * (plot_height - 1))


def frequency_column(hz: float, width: int) -> int:
    """The column a frequency falls in on the spectrum panel's log axis."""
    fraction = math.log(hz / AXIS_LOW_HZ) / math.log(NYQUIST_HZ / AXIS_LOW_HZ)
    return away_from_zero(fraction * width)


# ----------------------------------------------------------------------------
# Driving the window
# ----------------------------------------------------------------------------


def run(binary: Path, audio: Path, operations: str, **outputs: Path) -> subprocess.CompletedProcess:
    command = [str(binary), str(audio)]
    if operations:
        command += ["--apply", operations]
    for option, path in outputs.items():
        command += ["--" + option.replace("_", "-")] + ([] if path is None else [str(path)])
    return subprocess.run(command, capture_output=True, text=True, timeout=900)


def reported(stdout: str) -> dict[str, str]:
    """The key=value lines, as text. Numbers are converted where they are used."""
    out: dict[str, str] = {}
    for line in stdout.splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            out[key.strip()] = value
    return out


def number(values: dict[str, str], key: str) -> float:
    return float(values[key])


class Failure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


# ----------------------------------------------------------------------------
# The rules, applied to every run
# ----------------------------------------------------------------------------


def check_key_rule(values: dict[str, str], what: str) -> None:
    """A key is named exactly when the strength and the contrast allow it.

    Checked on every probe rather than on the two that were chosen to show it,
    because a panel that had stopped applying the rule in between would pass a
    pair of examples.
    """
    strength = number(values, "key_strength")
    contrast = number(values, "key_contrast")
    cents = number(values, "key_tuning_cents")
    shown = values["shown_key"]
    certainty = int(values["key_certainty"])

    nameable = contrast >= KEYLESS_CONTRAST and strength >= KEY_REFUSED_BELOW
    if not nameable:
        require(
            shown == "no key" and certainty == NONE,
            f"{what}: strength {strength:.3f} and contrast {contrast:.3f} are below the "
            f"thresholds, but the panel shows '{shown}' with certainty {certainty}",
        )
        require(
            values["shown_key_caveat"] != "",
            f"{what}: the panel refused a key without saying why",
        )
        # And the two figures that only describe a named key go away with it.
        require(
            values["shown_runner_up"] == "--" and values["shown_tuning"] == "--",
            f"{what}: no key was named, but the runner-up and tuning rows still carry "
            f"'{values['shown_runner_up']}' and '{values['shown_tuning']}'",
        )
        return

    require(shown != "no key", f"{what}: a nameable key was not named")
    doubtful = strength < KEY_FIRM_AT or abs(cents) > TUNING_FAR_CENTS
    want = DOUBTFUL if doubtful else FIRM
    require(
        certainty == want,
        f"{what}: strength {strength:.3f} and {cents:+.0f} cents should read as "
        f"certainty {want}, not {certainty}",
    )
    require(
        (values["shown_key_caveat"] != "") == doubtful,
        f"{what}: the caveat is {'missing' if doubtful else 'present'} for "
        f"'{shown}' at strength {strength:.3f}, {cents:+.0f} cents",
    )


def check_tempo_rule(values: dict[str, str], what: str) -> None:
    valid = values["tempo_valid"] == "1"
    shown = values["shown_tempo"]
    certainty = int(values["tempo_certainty"])
    if not valid:
        require(
            shown == "no tempo" and certainty == NONE,
            f"{what}: no tempo was found, but the panel shows '{shown}'",
        )
        require(values["shown_tempo_caveat"] != "",
                f"{what}: the panel refused a tempo without saying why")
        require(
            values["shown_beat_grid"] == "--" and values["shown_confidence"] != "",
            f"{what}: no tempo was found, but the grid row carries "
            f"'{values['shown_beat_grid']}'",
        )
        return

    confidence = number(values, "tempo_confidence")
    want = DOUBTFUL if confidence < TEMPO_DOUBTFUL_BELOW else FIRM
    require(
        certainty == want,
        f"{what}: a tempo at confidence {confidence:.2f} should read as certainty "
        f"{want}, not {certainty}",
    )


def check_room_rule(values: dict[str, str], what: str) -> None:
    """A room figure is a number exactly when its own flag is true.

    The one the brief cares most about. Each of these fields is left at zero
    when the decay had no range for it, and zero seconds would read as a very
    dead room -- so the panel has to print a dash, and there is no third
    option.
    """
    for figure, flag in (("edt", "room_has_edt"), ("t20", "room_has_t20"), ("t30", "room_has_t30")):
        text = values["shown_" + figure]
        supported = values[flag] == "1" and values["room_valid"] == "1"
        if supported:
            require(
                text.endswith(" s") and text != "-- s",
                f"{what}: {figure.upper()} is supported but the panel shows '{text}'",
            )
        else:
            require(
                text == "--",
                f"{what}: {figure.upper()} is not supported by this record, but the panel "
                f"shows '{text}'",
            )


def main() -> int:  # noqa: C901 - one long script of checks, in the order they build on each other
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="path to the auscult executable")
    parser.add_argument("--keep", type=Path, help="write the screenshots into this directory")
    arguments = parser.parse_args()

    with tempfile.TemporaryDirectory() as directory:
        workspace = Path(directory)
        shots = arguments.keep if arguments.keep else workspace
        shots.mkdir(parents=True, exist_ok=True)

        clicks = workspace / "clicks.wav"
        tone = workspace / "tone.wav"
        detuned = workspace / "detuned.wav"
        noise = workspace / "noise.wav"
        quiet_room = workspace / "room-quiet.wav"
        noisy_room = workspace / "room-noisy.wav"
        write_clicks(clicks)
        write_tone(tone, TONE_HZ, gap=True)
        write_tone(detuned, DETUNED_HZ, gap=False)
        write_noise(noise)
        write_impulse(quiet_room, IR_QUIET_FLOOR)
        write_impulse(noisy_room, IR_NOISY_FLOOR)

        click_frames = int(SAMPLE_RATE * CLICK_SECONDS)
        tone_frames = int(SAMPLE_RATE * TONE_SECONDS)

        # ------------------------------------------------------------------
        # Nothing is drawn until it is asked for.
        #
        # Checked first because it is what makes every "it is drawn here"
        # assertion below mean anything: if a colour test matched something the
        # views paint anyway, all of them would pass on an overlay that was
        # never drawn at all.
        # ------------------------------------------------------------------
        bare_wave = shots / "bare-waveform.png"
        bare_spectrum = shots / "bare-spectrum.png"
        done = run(arguments.binary, clicks, "nobeatgrid",
                   screenshot_waveform=bare_wave, screenshot_spectrum=bare_spectrum)
        if done.returncode != 0 or not bare_wave.exists():
            print(f"FAIL: rendering the bare views exited {done.returncode} -- {done.stderr}")
            return 1

        width, height, rows = read_png(bare_wave)
        if width < 200 or height < 60:
            print(f"FAIL: the waveform plot is {width}x{height}, too small to read")
            return 1
        stray = marked_columns(rows, width, height, is_beat_colour)
        if stray:
            print(f"FAIL: {len(stray)} columns of beat grid with the grid switched off")
            return 1
        stray = marked_columns(rows, width, height, is_contour_colour)
        if stray:
            print(f"FAIL: {len(stray)} columns of pitch contour with no contour asked for")
            return 1

        spectrum_width, spectrum_height, spectrum_rows = read_png(bare_spectrum)
        spectrum_plot = spectrum_height - LABEL_STRIP
        stray = marked_columns(spectrum_rows, spectrum_width, spectrum_plot, is_contour_colour)
        if stray:
            print(f"FAIL: {len(stray)} columns of band chart with no bands asked for")
            return 1

        # ------------------------------------------------------------------
        # The beat grid falls on the transients.
        # ------------------------------------------------------------------
        beat_shot = shots / "beat-grid.png"
        done = run(arguments.binary, clicks, "beatgrid",
                   screenshot_waveform=beat_shot, print_musical=None)
        if done.returncode != 0 or not beat_shot.exists():
            print(f"FAIL: rendering the beat grid exited {done.returncode} -- {done.stderr}")
            return 1
        beats = reported(done.stdout)
        check_key_rule(beats, "the click track")
        check_tempo_rule(beats, "the click track")

        if beats["tempo_valid"] != "1":
            print(f"FAIL: a click every {CLICK_PERIOD} s was not recognised as a tempo")
            return 1
        bpm = number(beats, "tempo_bpm")
        if abs(bpm - CLICK_BPM) > 0.5:
            print(f"FAIL: a click every {CLICK_PERIOD} s read as {bpm:.2f} BPM, not {CLICK_BPM:.0f}")
            return 1

        # Every reported beat sits on a click. This is the half of the check
        # that does not go through the drawing at all: a grid that drew its own
        # numbers perfectly but had them in the wrong place would pass the
        # pixel comparison below and fail here.
        first_beat = number(beats, "tempo_first_beat")
        period = 60.0 / bpm
        beat_count = int(beats["tempo_beats"])
        worst_offset = 0.0
        for n in range(beat_count):
            at = first_beat + n * period
            offset = abs(at - round(at / CLICK_PERIOD) * CLICK_PERIOD)
            worst_offset = max(worst_offset, offset)
        # One analysis hop is 256 samples, 5.3 ms, and TempoTrack.h claims a
        # beat within half a hop of where it really was. Ten milliseconds is
        # that bound with room for the least-squares fit across eight seconds.
        if worst_offset > 0.010:
            print(
                f"FAIL: the beat grid is up to {worst_offset * 1000:.1f} ms from the clicks it "
                f"was fitted to, over {beat_count} beats"
            )
            return 1

        width, height, rows = read_png(beat_shot)
        drawn = marked_columns(rows, width, height, is_beat_colour)
        # A beat line runs the height of the plot; anything shorter is not one.
        full = sorted(x for x, ys in drawn.items() if len(ys) > height * 0.8)
        expected = [
            time_column(first_beat + n * period, click_frames, width) for n in range(beat_count)
        ]
        expected = [x for x in expected if 0 <= x < width]
        if full != expected:
            print(
                f"FAIL: the beat grid was drawn in columns {full[:8]}... where the times the "
                f"window reports put it in {expected[:8]}..."
            )
            return 1
        if len(full) < 12:
            print(f"FAIL: only {len(full)} beats were drawn over {CLICK_SECONDS:.0f} s at "
                  f"{bpm:.1f} BPM")
            return 1

        # ------------------------------------------------------------------
        # A refused tempo leaves the waveform bare.
        #
        # The same picture has to mean "there is no tempo here", not "the grid
        # failed to draw" -- which is why the panel's words are checked in the
        # same breath as the pixels.
        # ------------------------------------------------------------------
        noise_shot = shots / "noise-waveform.png"
        noise_spectrum = shots / "noise-spectrum.png"
        done = run(arguments.binary, noise, "beatgrid,bands",
                   screenshot_waveform=noise_shot, screenshot_spectrum=noise_spectrum,
                   print_musical=None)
        if done.returncode != 0 or not noise_shot.exists():
            print(f"FAIL: analysing white noise exited {done.returncode} -- {done.stderr}")
            return 1
        noise_values = reported(done.stdout)
        check_key_rule(noise_values, "white noise")
        check_tempo_rule(noise_values, "white noise")

        if noise_values["tempo_valid"] != "1":
            pass  # The expected answer; asserted below so the message is specific.
        if noise_values["shown_tempo"] != "no tempo":
            print(f"FAIL: white noise was given a tempo of {noise_values['shown_tempo']}")
            return 1
        if noise_values["shown_key"] != "no key":
            print(f"FAIL: white noise was given a key of {noise_values['shown_key']}")
            return 1
        # Refused on strength rather than on contrast, and the difference is
        # worth stating. Noise this short still leans a little -- its chroma
        # contrast comes out around 0.18, above the 0.15 at which a chroma is
        # called flat -- but it fits no key profile at all, so the strength,
        # which is the fit scaled by the contrast, collapses. That is the path
        # this probe exercises; the flat-chroma path is covered in
        # AnalysisReadoutTests, where a contrast can be stated rather than
        # hoped for.
        if number(noise_values, "key_strength") >= KEY_REFUSED_BELOW:
            print(
                f"FAIL: white noise scored a key strength of "
                f"{number(noise_values, 'key_strength'):.3f}, so this probe no longer tests the "
                f"refusal it was written for"
            )
            return 1
        if "strength" not in noise_values["shown_key_caveat"]:
            print(
                f"FAIL: the key was refused without the figure it was refused on: "
                f"'{noise_values['shown_key_caveat']}'"
            )
            return 1

        width, height, rows = read_png(noise_shot)
        drawn = marked_columns(rows, width, height, is_beat_colour)
        if drawn:
            print(f"FAIL: a grid was drawn over material the window says has no tempo")
            return 1

        # ------------------------------------------------------------------
        # Every band is drawn at the level it was measured at.
        #
        # Band by band, against a level axis recomputed in this file from the
        # constants in ViewGeometry.h. A chart that agreed with a second model
        # inside the application would agree with itself however wrong both
        # were.
        # ------------------------------------------------------------------
        spectrum_width, spectrum_height, spectrum_rows = read_png(noise_spectrum)
        spectrum_plot = spectrum_height - LABEL_STRIP
        caps = marked_columns(spectrum_rows, spectrum_width, spectrum_plot, is_contour_colour)
        if not caps:
            print("FAIL: the bands were asked for and nothing was drawn")
            return 1

        band_count = int(noise_values["bands"])
        if band_count != 31:
            print(f"FAIL: {band_count} third-octave bands, not 31")
            return 1

        centres: dict[int, list[int]] = {}
        for i in range(band_count):
            centre_hz, _, level = noise_values[f"band_{i}"].partition(",")
            column = frequency_column(float(centre_hz), spectrum_width)
            centres.setdefault(column, []).append(i)

        checked = 0
        levels: list[float] = []
        for i in range(band_count):
            centre_hz, _, level_db = noise_values[f"band_{i}"].partition(",")
            levels.append(float(level_db))
            column = frequency_column(float(centre_hz), spectrum_width)
            # The bottom bands are narrower than a column at the left of a log
            # axis, so two of them can share their centre column and there is
            # no way to say which bar a row belongs to. Skipped rather than
            # guessed at.
            if len(centres[column]) != 1 or not 0 <= column < spectrum_width:
                continue
            want = level_row(float(level_db), spectrum_plot)
            got = caps.get(column, [])
            if want not in got:
                print(
                    f"FAIL: the {centre_hz} Hz band measured {float(level_db):.1f} dB, which is "
                    f"row {want} of {spectrum_plot}, but column {column} carries rows {got}"
                )
                return 1
            checked += 1
        if checked < 20:
            print(f"FAIL: only {checked} of {band_count} bands could be checked")
            return 1
        # And the staircase is really a staircase: white noise puts twice the
        # bandwidth in a band four above, so the chart must climb. Without this
        # a drawing that put every bar on one row would pass everything above
        # as long as the levels happened to agree.
        if levels[-4] - levels[4] < 12.0:
            print(
                f"FAIL: over white noise the bands only climb "
                f"{levels[-4] - levels[4]:.1f} dB from {levels[4]:.1f} to {levels[-4]:.1f}"
            )
            return 1

        # ------------------------------------------------------------------
        # The pitch contour is drawn at the pitch it found, and breaks.
        # ------------------------------------------------------------------
        contour_shot = shots / "pitch-contour.png"
        done = run(arguments.binary, tone, "pitchcontour",
                   screenshot_waveform=contour_shot, print_musical=None)
        if done.returncode != 0 or not contour_shot.exists():
            print(f"FAIL: tracking the pitch exited {done.returncode} -- {done.stderr}")
            return 1
        contour = reported(done.stdout)
        check_key_rule(contour, "the 440 Hz tone")
        check_tempo_rule(contour, "the 440 Hz tone")

        if contour["visible_pitch"] != "1":
            print("FAIL: the pitch section is hidden although a contour was asked for")
            return 1
        shown_hz = float(contour["shown_pitch"].removesuffix(" Hz"))
        if abs(shown_hz - TONE_HZ) > 1.0:
            print(f"FAIL: a {TONE_HZ:.0f} Hz tone was reported as {shown_hz:.1f} Hz")
            return 1

        width, height, rows = read_png(contour_shot)
        drawn = marked_columns(rows, width, height, is_contour_colour)
        if len(drawn) < width // 2:
            print(f"FAIL: the contour covers only {len(drawn)} of {width} columns")
            return 1

        want_row = pitch_row(TONE_HZ, height)
        worst = max(abs(y - want_row) for ys in drawn.values() for y in ys)
        # One row, because the contour is drawn without antialiasing and a
        # steady tone has to land on exactly the row its frequency maps to.
        # The row either side is the tracker's own wobble, not the drawing's.
        if worst > 1:
            rows_seen = sorted({y for ys in drawn.values() for y in ys})
            print(
                f"FAIL: a {TONE_HZ:.0f} Hz contour was drawn on rows {rows_seen} where the "
                f"{PITCH_LOW_HZ:.0f}-{PITCH_HIGH_HZ:.0f} Hz axis over {height} rows puts it on "
                f"row {want_row}"
            )
            return 1

        # The break. One gap, covering the silence, and nothing ruled across
        # it: PitchTrack.h reports zero hertz on an unvoiced frame exactly so
        # that a drawing can stop, and a contour that joined the two notes
        # would be drawing a glissando nobody played.
        present = sorted(drawn)
        gaps = [(present[i - 1], present[i]) for i in range(1, len(present))
                if present[i] - present[i - 1] > 1]
        if len(gaps) != 1:
            print(f"FAIL: the contour has {len(gaps)} breaks in it, not the one silence: {gaps}")
            return 1
        gap_from, gap_to = gaps[0]
        want_from = time_column(GAP_FROM, tone_frames, width)
        want_to = time_column(GAP_TO, tone_frames, width)
        # The analysis window is 2048 samples and reaches further still for its
        # longest lag, so the contour stops a little before the silence and
        # resumes a little after it. A twentieth of the file either way covers
        # that and does not cover a break in the wrong place.
        slack = width // 20
        if abs(gap_from - want_from) > slack or abs(gap_to - want_to) > slack:
            print(
                f"FAIL: the contour breaks between columns {gap_from} and {gap_to}, where the "
                f"silence from {GAP_FROM} s to {GAP_TO} s is columns {want_from} to {want_to}"
            )
            return 1

        # ------------------------------------------------------------------
        # A key from a detuned recording is named and not trusted.
        # ------------------------------------------------------------------
        done = run(arguments.binary, detuned, "", print_musical=None)
        if done.returncode != 0:
            print(f"FAIL: analysing the detuned tone exited {done.returncode} -- {done.stderr}")
            return 1
        off = reported(done.stdout)
        check_key_rule(off, "the detuned tone")
        cents = number(off, "key_tuning_cents")
        if abs(abs(cents) - DETUNED_CENTS) > 12.0:
            print(
                f"FAIL: a tone {DETUNED_CENTS:.0f} cents sharp was measured at {cents:+.0f} "
                f"cents, so this probe no longer tests the caveat it was written for"
            )
            return 1
        if off["key_certainty"] != str(DOUBTFUL):
            print(
                f"FAIL: a key from a recording {cents:+.0f} cents from A = 440 was shown as "
                f"certainty {off['key_certainty']}"
            )
            return 1
        if "cents" not in off["shown_key_caveat"]:
            print(f"FAIL: the detuning is not in the caveat: '{off['shown_key_caveat']}'")
            return 1

        # ------------------------------------------------------------------
        # The room figures, and the ones the record cannot support.
        # ------------------------------------------------------------------
        done = run(arguments.binary, quiet_room, "room", print_musical=None)
        if done.returncode != 0:
            print(f"FAIL: measuring the quiet room exited {done.returncode} -- {done.stderr}")
            return 1
        full_room = reported(done.stdout)
        check_room_rule(full_room, "the clean impulse response")
        if full_room["room_valid"] != "1":
            print("FAIL: a decaying noise burst was not recognised as an impulse response")
            return 1
        for figure in ("edt", "t20", "t30"):
            seconds = float(full_room["shown_" + figure].removesuffix(" s"))
            # The envelope falls 60 dB in IR_T60 seconds by construction, so
            # all three measures have the same right answer.
            if abs(seconds - IR_T60) > 0.05:
                print(
                    f"FAIL: {figure.upper()} on a {IR_T60:.1f} s decay read {seconds:.3f} s"
                )
                return 1
        if full_room["visible_t30"] != "1":
            print("FAIL: the room section is hidden although it was asked for")
            return 1

        done = run(arguments.binary, noisy_room, "room", print_musical=None)
        if done.returncode != 0:
            print(f"FAIL: measuring the noisy room exited {done.returncode} -- {done.stderr}")
            return 1
        short_room = reported(done.stdout)
        check_room_rule(short_room, "the truncated impulse response")
        if short_room["room_has_t30"] != "0":
            print(
                f"FAIL: a decay with only "
                f"{number(short_room, 'room_usable_range_db'):.0f} dB of range reported a T30, "
                f"so this probe no longer tests the refusal it was written for"
            )
            return 1
        if short_room["shown_t30"] != "--":
            print(f"FAIL: an unsupported T30 was shown as '{short_room['shown_t30']}'")
            return 1
        if short_room["room_has_t20"] != "1":
            print(
                f"FAIL: the truncated probe lost T20 as well as T30 "
                f"({number(short_room, 'room_usable_range_db'):.0f} dB of range), so it no "
                f"longer shows the difference between the two"
            )
            return 1
        if not short_room["shown_t20"].endswith(" s"):
            print(f"FAIL: a supported T20 was shown as '{short_room['shown_t20']}'")
            return 1
        if "T30" not in short_room["shown_room_caveat"]:
            print(f"FAIL: the panel dropped T30 without saying so: "
                  f"'{short_room['shown_room_caveat']}'")
            return 1

        # ------------------------------------------------------------------
        # And something that is not an impulse response at all.
        # ------------------------------------------------------------------
        done = run(arguments.binary, tone, "room", print_musical=None)
        if done.returncode != 0:
            print(f"FAIL: measuring a tone as a room exited {done.returncode} -- {done.stderr}")
            return 1
        not_a_room = reported(done.stdout)
        check_room_rule(not_a_room, "a steady tone measured as a room")
        if not_a_room["room_valid"] != "0":
            print("FAIL: a steady tone was measured as an impulse response")
            return 1
        if "not an impulse response" not in not_a_room["shown_room_caveat"]:
            print(
                f"FAIL: a tone measured as a room said '{not_a_room['shown_room_caveat']}'"
            )
            return 1

        # The sections nobody asked for are not on screen at all, rather than
        # eight rows of dashes.
        if contour["visible_t30"] != "0" or beats["visible_pitch"] != "0":
            print("FAIL: a section nobody asked for is on screen")
            return 1

        print(
            f"OK: waveform {width}x{height}, spectrum {spectrum_width}x{spectrum_plot}; "
            f"{len(full)} beats drawn at {bpm:.2f} BPM, within "
            f"{worst_offset * 1000:.1f} ms of the clicks; contour on row {want_row} "
            f"({TONE_HZ:.0f} Hz, drawn within {worst} row), breaking over columns "
            f"{gap_from}-{gap_to}; {checked} bands drawn on their measured rows, climbing "
            f"{levels[-4] - levels[4]:.1f} dB; T20 {full_room['shown_t20']} and T30 "
            f"{full_room['shown_t30']} on a {IR_T60:.1f} s decay, T30 '{short_room['shown_t30']}' "
            f"on {number(short_room, 'room_usable_range_db'):.0f} dB of range; "
            f"key refused on noise, doubted at {cents:+.0f} cents"
        )
        return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Failure as failure:
        print(f"FAIL: {failure}")
        sys.exit(1)
