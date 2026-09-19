#!/usr/bin/env python3
"""End-to-end check that the EQ curve is the filter, and the filter is the audio.

Three claims, none of which the other driver scripts can make:

  1. Dragging a band moves the *drawn* curve where it was dragged. The window
     is driven through the same press/move/release a pointer would deliver, so
     the hit-testing and the pixel-to-hertz mapping are exercised rather than
     bypassed, and the picture is read back to see where the band went.
  2. The drawn curve is the response of the filter that will be applied, at
     every column, not a decorative bell drawn near it. The expected response
     is recomputed here from the cookbook transfer function; a drawing that
     agreed with a second model inside the application would agree with itself
     however wrong both were.
  3. Applying it changes the audio by the amount the curve promised, and undo
     puts the audio back.

A boost at 1 kHz that draws beautifully and filters at 700 Hz would pass every
other check in this repository.

No third-party imports: the PNG decoder below is forty lines and Pillow is not
worth adding to a CI image for it.
"""

from __future__ import annotations

import argparse
import math
import struct
import subprocess
import sys
import tempfile
import wave
import zlib
from pathlib import Path

SAMPLE_RATE = 48000
SECONDS = 4.0

# The three tones the probe is made of. Each divides the sample rate exactly, so
# a measurement window of a whole number of cycles has no leakage and the
# amplitude that comes back is the amplitude that is there.
TONES = (200.0, 1000.0, 5000.0)
TONE_AMPLITUDE = 0.08

# What the EQ is asked for: a bell placed at 300 Hz, then dragged to 1 kHz.
BAND_GAIN_DB = 12.0
BAND_START_HZ = 300.0
BAND_END_HZ = 1000.0

# The panel's level axis, which the curve is drawn on. These are the constants
# in sa/ui/ViewGeometry.h; the test has to know them to say where a row is,
# and stating them here is what makes the row assertions arithmetic.
TOP_DB = 0.0
BOTTOM_DB = -108.0
EQ_ZERO_LEVEL_DB = -54.0
LABEL_STRIP = 18  # The frequency labels sit below the plot.
AXIS_LOW_HZ = 20.0


def write_probe_wav(path: Path) -> None:
    """Three steady tones, two octaves and a bit apart.

    Steady rather than a sweep because the claim being checked is about level
    at a frequency: a sweep puts every frequency somewhere in the file and
    nowhere in particular, which is the wrong shape for "is 1 kHz louder now".
    """
    frames = int(SAMPLE_RATE * SECONDS)
    samples = bytearray()
    for i in range(frames):
        t = i / SAMPLE_RATE
        value = sum(TONE_AMPLITUDE * math.sin(2.0 * math.pi * hz * t) for hz in TONES)
        samples += struct.pack("<h", int(value * 32767))

    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(SAMPLE_RATE)
        handle.writeframes(bytes(samples))


def load(path: Path) -> list[float]:
    """Read a WAV's first channel as floats in [-1, 1]."""
    with wave.open(str(path), "rb") as handle:
        frames = handle.getnframes()
        channels = handle.getnchannels()
        width = handle.getsampwidth()
        raw = handle.readframes(frames)

    out: list[float] = []
    stride = width * channels
    if width == 2:
        for i in range(0, len(raw) - stride + 1, stride):
            out.append(struct.unpack("<h", raw[i : i + 2])[0] / 32768.0)
    elif width == 3:
        for i in range(0, len(raw) - stride + 1, stride):
            out.append(int.from_bytes(raw[i : i + 3], "little", signed=True) / 8388608.0)
    elif width == 4:
        for i in range(0, len(raw) - stride + 1, stride):
            out.append(struct.unpack("<f", raw[i : i + 4])[0])
    else:
        raise ValueError(f"unsupported sample width {width}")
    return out


def tone_db(samples: list[float], hz: float) -> float:
    """Level of one tone, in dBFS.

    A single-frequency DFT over a whole number of cycles, starting a second in
    so that the filter's own settling at the head of the file is behind us.
    """
    start = SAMPLE_RATE
    length = SAMPLE_RATE // 2  # 0.5 s: an exact number of cycles of every tone.
    window = samples[start : start + length]
    if len(window) < length:
        raise ValueError("the file is too short to measure")
    real = imaginary = 0.0
    for i, value in enumerate(window):
        angle = 2.0 * math.pi * hz * i / SAMPLE_RATE
        real += value * math.cos(angle)
        imaginary -= value * math.sin(angle)
    amplitude = 2.0 * math.hypot(real, imaginary) / length
    return 20.0 * math.log10(max(amplitude, 1e-12))


def peaking_db(f0: float, q: float, gain_db: float, hz: float) -> float:
    """Magnitude of an RBJ peaking section, in decibels.

    Written out here rather than taken from the application. With
    A = 10^(gain/40), w0 = 2 pi f0 / fs and alpha = sin(w0) / 2Q, the section is

        b = {1 + alpha A, -2 cos w0, 1 - alpha A}
        a = {1 + alpha/A, -2 cos w0, 1 - alpha/A}

    and putting z = e^(jw), multiplying through by z and using z + 1/z = 2cos w,
    z - 1/z = 2j sin w, the numerator is 2(cos w - cos w0) + 2j alpha A sin w and
    the denominator the same with A inverted. So with C = (cos w - cos w0)^2 and
    S = (alpha sin w)^2,

        |H|^2 = (C + A^2 S) / (C + S / A^2).
    """
    a = 10.0 ** (gain_db / 40.0)
    w0 = 2.0 * math.pi * f0 / SAMPLE_RATE
    w = 2.0 * math.pi * hz / SAMPLE_RATE
    alpha = math.sin(w0) / (2.0 * q)
    c = (math.cos(w) - math.cos(w0)) ** 2
    s = (alpha * math.sin(w)) ** 2
    return 10.0 * math.log10((c + a * a * s) / (c + s / (a * a)))


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


def is_eq_colour(pixel: tuple[int, int, int]) -> bool:
    """Whether a pixel belongs to the summed EQ curve.

    The curve is the one violet thing in the panel: everything else is blue
    (the average), amber (the peak), or neutral (grid, labels, reference). The
    thresholds allow for the antialiasing on a sloped stroke while staying well
    clear of every other colour the panel draws -- the handles are pale enough
    that their green fails the test, which keeps them out of the curve.
    """
    red, green, blue = pixel
    return red >= 170 and blue >= 170 and green <= red - 55 and green <= blue - 55


def curve_rows(
    rows: list[list[tuple[int, int, int]]], width: int, plot_height: int
) -> list[float | None]:
    """Where the EQ curve sits in each column, as a row, or None if it is absent.

    The centre of the stroke rather than its top edge: a two-pixel pen centred
    on row y covers rows y-1 and y, so the mean of the coloured rows tracks the
    path itself and does not drift by half a pixel wherever the curve slopes.
    """
    found: list[float | None] = []
    for x in range(width):
        hits = [y for y in range(plot_height) if is_eq_colour(rows[y][x])]
        found.append(sum(hits) / len(hits) + 0.5 if hits else None)
    return found


def column_frequency(width: int, column: int) -> float:
    """The frequency at a column, on the panel's log axis."""
    return AXIS_LOW_HZ * (24000.0 / AXIS_LOW_HZ) ** (column / width)


def frequency_column(width: int, hz: float) -> int:
    fraction = math.log(hz / AXIS_LOW_HZ) / math.log(24000.0 / AXIS_LOW_HZ)
    return max(0, min(width - 1, int(fraction * width + 0.5)))


def gain_row(plot_height: int, gain_db: float) -> float:
    """The row an EQ gain is drawn at, from the panel's level axis."""
    fraction = (TOP_DB - (EQ_ZERO_LEVEL_DB + gain_db)) / (TOP_DB - BOTTOM_DB)
    return math.floor(min(max(fraction, 0.0), 1.0) * (plot_height - 1) + 0.5)


def db_per_row(plot_height: int) -> float:
    return (TOP_DB - BOTTOM_DB) / (plot_height - 1)


def run(binary: Path, audio: Path, operations: str, **outputs: Path) -> subprocess.CompletedProcess:
    command = [str(binary), str(audio)]
    if operations:
        command += ["--apply", operations]
    for option, path in outputs.items():
        command += ["--" + option.replace("_", "-"), str(path)]
    return subprocess.run(command, capture_output=True, text=True, timeout=600)


def numbers_from(stdout: str) -> dict[str, float]:
    measured: dict[str, float] = {}
    for line in stdout.splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            try:
                measured[key.strip()] = float(value)
            except ValueError:
                pass
    return measured


def main() -> int:  # noqa: C901 - one long script of checks, in the order they build on each other
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="path to the auscult executable")
    parser.add_argument("--keep", type=Path, help="write the screenshots into this directory")
    arguments = parser.parse_args()

    with tempfile.TemporaryDirectory() as directory:
        workspace = Path(directory)
        shots = arguments.keep if arguments.keep else workspace
        shots.mkdir(parents=True, exist_ok=True)

        audio = workspace / "probe.wav"
        write_probe_wav(audio)

        # ------------------------------------------------------------------
        # The curve is not drawn until it is asked for.
        #
        # Checked first because it is what makes every "the curve is here"
        # assertion below mean anything: if the colour test matched something
        # the spectrum draws anyway, all of them would pass on an EQ that was
        # never painted.
        # ------------------------------------------------------------------
        hidden = shots / "eq-hidden.png"
        done = run(arguments.binary, audio, "", screenshot_spectrum=hidden)
        if done.returncode != 0 or not hidden.exists():
            print(f"FAIL: rendering the plain spectrum exited {done.returncode} -- {done.stderr}")
            return 1
        width, height, rows = read_png(hidden)
        plot_height = height - LABEL_STRIP
        if width < 120 or plot_height < 80:
            print(f"FAIL: the spectrum panel is {width}x{plot_height}, too small to read")
            return 1
        stray = sum(1 for y in range(plot_height) for x in range(width) if is_eq_colour(rows[y][x]))
        if stray != 0:
            print(f"FAIL: {stray} EQ-coloured pixels with the EQ hidden")
            return 1

        per_row = db_per_row(plot_height)

        # ------------------------------------------------------------------
        # Shown with no bands, the curve is a flat line on the 0 dB row.
        #
        # This pins the level axis: a curve drawn on its own private decibel
        # scale, or offset from the datum the gutter labels describe, lands
        # somewhere other than the row this arithmetic says.
        # ------------------------------------------------------------------
        flat = shots / "eq-flat.png"
        done = run(arguments.binary, audio, "eq", screenshot_spectrum=flat)
        if done.returncode != 0 or not flat.exists():
            print(f"FAIL: showing the EQ curve exited {done.returncode} -- {done.stderr}")
            return 1
        _, _, flat_rows = read_png(flat)
        drawn = curve_rows(flat_rows, width, plot_height)
        missing = [x for x, row in enumerate(drawn) if row is None]
        if len(missing) > width // 20:
            print(f"FAIL: the flat EQ curve is missing from {len(missing)} of {width} columns")
            return 1
        expected_zero = gain_row(plot_height, 0.0)
        worst = max(abs(row - expected_zero) for row in drawn if row is not None)
        if worst > 1.5:
            print(
                f"FAIL: the 0 dB EQ line is up to {worst:.1f} rows from row {expected_zero}, "
                f"where {-EQ_ZERO_LEVEL_DB:.0f} dB below the top of a "
                f"{plot_height}-row axis puts it"
            )
            return 1

        # ------------------------------------------------------------------
        # A band added by double-clicking lands where it was clicked, and a
        # drag moves it. Read off the picture, then off the numbers.
        # ------------------------------------------------------------------
        added = shots / "eq-added.png"
        done = run(
            arguments.binary,
            audio,
            f"eq,eqadd:{BAND_START_HZ:.0f}/{BAND_GAIN_DB:.0f},eqprint",
            screenshot_spectrum=added,
        )
        if done.returncode != 0 or not added.exists():
            print(f"FAIL: adding an EQ band exited {done.returncode} -- {done.stderr}")
            return 1
        placed = numbers_from(done.stdout)
        if placed.get("eq_bands") != 1:
            print(f"FAIL: double-clicking the curve left {placed.get('eq_bands')} bands")
            return 1
        _, _, added_rows = read_png(added)
        added_curve = curve_rows(added_rows, width, plot_height)

        def highest(curve: list[float | None]) -> tuple[int, float]:
            """The column of the curve's peak, and how far above 0 dB it is."""
            best_column, best_row = 0, float(plot_height)
            for x, row in enumerate(curve):
                if row is not None and row < best_row:
                    best_column, best_row = x, row

            return best_column, (expected_zero - best_row) * per_row

        peak_column, peak_db = highest(added_curve)
        start_column = frequency_column(width, BAND_START_HZ)
        # The handle is drawn over its own band's summit, so the highest pixel
        # of the curve that is still visible sits just outside it -- a handful
        # of columns to one side, and a little lower than the full gain. Near
        # is therefore the claim, not exact.
        if abs(peak_column - start_column) > 12:
            print(
                f"FAIL: the curve peaks at column {peak_column}, "
                f"{column_frequency(width, peak_column):.0f} Hz, where {BAND_START_HZ:.0f} Hz "
                f"is column {start_column}"
            )
            return 1
        if peak_db < BAND_GAIN_DB * 0.5:
            print(
                f"FAIL: a {BAND_GAIN_DB:.0f} dB band lifted the curve only {peak_db:.1f} dB "
                f"off the 0 dB line"
            )
            return 1

        dragged = shots / "eq-dragged.png"
        done = run(
            arguments.binary,
            audio,
            f"eq,eqadd:{BAND_START_HZ:.0f}/{BAND_GAIN_DB:.0f},"
            f"eqdrag:0/{BAND_END_HZ:.0f}/{BAND_GAIN_DB:.0f},eqprint",
            screenshot_spectrum=dragged,
        )
        if done.returncode != 0 or not dragged.exists():
            print(f"FAIL: dragging the EQ band exited {done.returncode} -- {done.stderr}")
            return 1
        band = numbers_from(done.stdout)
        for key in ("eq_band0_hz", "eq_band0_gain_db", "eq_band0_q"):
            if key not in band:
                print(f"FAIL: the window did not report {key} after a drag")
                return 1

        # The drag is expressed in hertz and decibels but carried out in
        # pixels, so it can only land as near as one pixel of each axis allows:
        # a column is a fixed ratio in frequency, a row a fixed number of
        # decibels. Both tolerances are that pixel, doubled.
        column_ratio = (24000.0 / AXIS_LOW_HZ) ** (1.0 / width)
        if not BAND_END_HZ / column_ratio**2 < band["eq_band0_hz"] < BAND_END_HZ * column_ratio**2:
            print(
                f"FAIL: the band was dragged to {band['eq_band0_hz']:.1f} Hz, "
                f"not {BAND_END_HZ:.0f} Hz (a column is a factor of {column_ratio:.4f})"
            )
            return 1
        if abs(band["eq_band0_gain_db"] - BAND_GAIN_DB) > 2.0 * per_row:
            print(
                f"FAIL: the band was dragged to {band['eq_band0_gain_db']:.2f} dB, "
                f"not {BAND_GAIN_DB:.0f} dB (a row is {per_row:.3f} dB)"
            )
            return 1

        _, _, dragged_rows = read_png(dragged)
        dragged_curve = curve_rows(dragged_rows, width, plot_height)
        moved_column, moved_db = highest(dragged_curve)
        end_column = frequency_column(width, BAND_END_HZ)
        if abs(moved_column - end_column) > 12:
            print(
                f"FAIL: after dragging to {BAND_END_HZ:.0f} Hz the curve peaks at column "
                f"{moved_column} ({column_frequency(width, moved_column):.0f} Hz), not near "
                f"column {end_column}"
            )
            return 1
        if moved_column <= peak_column:
            print(
                f"FAIL: dragging a band from {BAND_START_HZ:.0f} Hz up to {BAND_END_HZ:.0f} Hz "
                f"moved its peak from column {peak_column} to {moved_column} -- the wrong way"
            )
            return 1
        # Sideways only. A log axis gives every column the same ratio, so the
        # same band at a new centre draws to the same height; a drag that let
        # the gain slide with the frequency would show up here.
        if abs(moved_db - peak_db) > 2.0:
            print(
                f"FAIL: the drag took the peak of the curve from {peak_db:.1f} dB to "
                f"{moved_db:.1f} dB, when only its frequency was meant to change"
            )
            return 1
        # And the frequency it left is flat again.
        before = dragged_curve[start_column]
        if before is None or (expected_zero - before) * per_row > 0.35 * BAND_GAIN_DB:
            print(
                f"FAIL: after the drag, {BAND_START_HZ:.0f} Hz is still "
                f"{(expected_zero - (before or 0)) * per_row:.1f} dB up"
            )
            return 1

        # ------------------------------------------------------------------
        # The whole drawn curve is the filter's response, column by column.
        #
        # Columns near the handle are skipped because the handle is painted
        # over the curve there. Everywhere else the drawing is compared with
        # the transfer function recomputed in this file, so agreement is
        # between two independent derivations rather than between the
        # application and itself.
        # ------------------------------------------------------------------
        handle_column = frequency_column(width, band["eq_band0_hz"])
        errors = []
        for x, row in enumerate(dragged_curve):
            if row is None or abs(x - handle_column) <= 9:
                continue
            response = peaking_db(
                band["eq_band0_hz"], band["eq_band0_q"], band["eq_band0_gain_db"],
                column_frequency(width, x),
            )
            # Half a row for the two-pixel pen's centring, half for the
            # rounding that chose the row it was drawn on.
            errors.append(abs(row - (gain_row(plot_height, response) + 0.5)))
        if len(errors) < width // 2:
            print(f"FAIL: only {len(errors)} of {width} columns carried a curve to compare")
            return 1
        errors.sort()
        median = errors[len(errors) // 2]
        if median > 1.5 or errors[-1] > 3.5:
            print(
                f"FAIL: the drawn curve is not the filter -- median {median:.2f} rows out, "
                f"worst {errors[-1]:.2f}, over {len(errors)} columns "
                f"({per_row:.3f} dB to the row)"
            )
            return 1

        # ------------------------------------------------------------------
        # The wheel changes Q, and only Q; a right-click takes the band away.
        # ------------------------------------------------------------------
        done = run(
            arguments.binary, audio,
            "eq,eqadd:1000/6,eqprint,eqq:0/4,eqprint,eqremove:0,eqprint",
        )
        if done.returncode != 0:
            print(f"FAIL: the Q and remove gestures exited {done.returncode} -- {done.stderr}")
            return 1
        reports = done.stdout.split("eq_bands=")
        if len(reports) < 4:
            print(f"FAIL: expected three band reports, got:\n{done.stdout}")
            return 1
        first = numbers_from(reports[0])
        second = numbers_from(reports[1])
        # Four notches at the documented 1.15 per notch.
        wanted = first["eq_band0_q"] * 1.15**4
        if abs(second["eq_band0_q"] - wanted) > 0.01:
            print(
                f"FAIL: four wheel notches took Q to {second['eq_band0_q']:.4f}, "
                f"not {wanted:.4f}"
            )
            return 1
        if abs(second["eq_band0_hz"] - first["eq_band0_hz"]) > 0.01 or abs(
            second["eq_band0_gain_db"] - first["eq_band0_gain_db"]
        ) > 0.01:
            print("FAIL: the wheel moved the band as well as reshaping it")
            return 1
        if numbers_from("eq_bands=" + reports[3]).get("eq_bands") != 0:
            print(f"FAIL: right-clicking the handle did not remove the band:\n{done.stdout}")
            return 1

        # Shift and drag is the same control for pointers with no wheel, so it
        # has to reach the same place: sixty pixels up doubles Q and leaves the
        # band where it is.
        done = run(arguments.binary, audio, "eq,eqadd:1000/6,eqprint,eqshiftdrag:0/-60,eqprint")
        if done.returncode != 0:
            print(f"FAIL: the Shift-drag gesture exited {done.returncode} -- {done.stderr}")
            return 1
        held = done.stdout.split("eq_bands=")
        if len(held) < 3:
            print(f"FAIL: expected two band reports from the Shift-drag, got:\n{done.stdout}")
            return 1
        before_q = numbers_from(held[0])
        after_q = numbers_from(held[1])
        if abs(after_q["eq_band0_q"] - before_q["eq_band0_q"] * 2.0) > 0.01:
            print(
                f"FAIL: a 60-pixel Shift-drag took Q from {before_q['eq_band0_q']:.3f} to "
                f"{after_q['eq_band0_q']:.3f}, not to {before_q['eq_band0_q'] * 2.0:.3f}"
            )
            return 1
        if abs(after_q["eq_band0_gain_db"] - before_q["eq_band0_gain_db"]) > 0.01:
            print("FAIL: the Shift-drag changed the gain as well as the width")
            return 1
        # The frequency follows the pointer, which did not move sideways -- so
        # it may only shift by the column it was already rounded to.
        if not (
            before_q["eq_band0_hz"] / column_ratio
            < after_q["eq_band0_hz"]
            < before_q["eq_band0_hz"] * column_ratio
        ):
            print(
                f"FAIL: the Shift-drag moved the band from {before_q['eq_band0_hz']:.1f} Hz "
                f"to {after_q['eq_band0_hz']:.1f} Hz"
            )
            return 1

        # ------------------------------------------------------------------
        # And the audio. A boost at 1 kHz makes 1 kHz louder by what was asked
        # for, leaves the tones either side of it alone, and undoes.
        # ------------------------------------------------------------------
        applied = workspace / "applied.wav"
        chain = (
            f"eq,eqadd:{BAND_START_HZ:.0f}/{BAND_GAIN_DB:.0f},"
            f"eqdrag:0/{BAND_END_HZ:.0f}/{BAND_GAIN_DB:.0f},eqapply"
        )
        done = run(arguments.binary, audio, chain, export=applied)
        if done.returncode != 0 or not applied.exists():
            print(f"FAIL: applying the EQ exited {done.returncode} -- {done.stderr}")
            return 1

        undone = workspace / "undone.wav"
        done = run(arguments.binary, audio, chain + ",undo", export=undone)
        if done.returncode != 0 or not undone.exists():
            print(f"FAIL: undoing the EQ exited {done.returncode} -- {done.stderr}")
            return 1

        original = load(audio)
        after = load(applied)
        restored = load(undone)

        # What the band actually is, after the pixels have had their say, is
        # what it should be measured against -- not the round number asked for.
        expected = {
            hz: peaking_db(band["eq_band0_hz"], band["eq_band0_q"], band["eq_band0_gain_db"], hz)
            for hz in TONES
        }
        moved: dict[float, float] = {}
        for hz in TONES:
            moved[hz] = tone_db(after, hz) - tone_db(original, hz)
            # Half a decibel covers the 16-to-24-bit round trip and the
            # measurement window; it does not cover a band an octave out, a
            # gain read off the wrong axis, or a filter that was never run.
            if abs(moved[hz] - expected[hz]) > 0.5:
                print(
                    f"FAIL: {hz:.0f} Hz moved {moved[hz]:+.2f} dB where the curve promised "
                    f"{expected[hz]:+.2f} dB"
                )
                return 1

        if moved[1000.0] < BAND_GAIN_DB - 1.5:
            print(
                f"FAIL: a {BAND_GAIN_DB:.0f} dB boost at 1 kHz only moved 1 kHz "
                f"{moved[1000.0]:+.2f} dB"
            )
            return 1
        for hz in (200.0, 5000.0):
            if abs(moved[hz]) > 1.5:
                print(f"FAIL: a bell at 1 kHz moved {hz:.0f} Hz by {moved[hz]:+.2f} dB")
                return 1

        for hz in TONES:
            back = tone_db(restored, hz) - tone_db(original, hz)
            if abs(back) > 0.05:
                print(f"FAIL: undo left {hz:.0f} Hz {back:+.3f} dB from where it started")
                return 1

        print(
            f"OK: panel {width}x{plot_height} ({per_row:.3f} dB/row), 0 dB line on row "
            f"{expected_zero}; band dragged from column {peak_column} to {moved_column} "
            f"({band['eq_band0_hz']:.0f} Hz, {band['eq_band0_gain_db']:.2f} dB, "
            f"Q {band['eq_band0_q']:.2f}); curve matches the filter to a median "
            f"{median:.2f} rows over {len(errors)} columns; "
            + ", ".join(f"{hz:.0f} Hz {moved[hz]:+.2f} dB" for hz in TONES)
            + "; undo exact"
        )
        return 0


if __name__ == "__main__":
    sys.exit(main())
