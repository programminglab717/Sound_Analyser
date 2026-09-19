#!/usr/bin/env python3
"""End-to-end check that the interface really draws the audio it was given.

Generates a signal whose spectrogram has a known shape, renders the window
headlessly, and inspects the pixels. A build that loads the file, runs the STFT
and then paints nothing would pass a "did it exit zero" test; this one fails it.

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


def write_probe_wav(path: Path, sample_rate: int = 48000, seconds: float = 6.0) -> None:
    """A 40 Hz to 18 kHz exponential sweep.

    Chosen because it is the strongest correctness signal available in one
    picture: on a logarithmic frequency axis an exponential sweep is a straight
    diagonal line, so an axis bug bends it and a rendering bug erases it.
    """
    frames = int(sample_rate * seconds)
    f0, f1 = 40.0, 18000.0
    k = math.log(f1 / f0)

    phase = 0.0
    samples = bytearray()
    for i in range(frames):
        t = i / sample_rate
        frequency = f0 * math.exp(k * t / seconds)
        phase += 2.0 * math.pi * frequency / sample_rate
        fade = min(1.0, t / 0.05, (seconds - t) / 0.05)
        value = 0.6 * math.sin(phase) * max(0.0, fade)
        samples += struct.pack("<h", int(value * 32767))

    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(sample_rate)
        handle.writeframes(bytes(samples))


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


def spectrum_curve(path: Path) -> tuple[int, int, list[int]]:
    """The height of the spectrum panel's average curve in each column.

    The curve is the one bright blue line in the panel, so the topmost blue
    pixel in a column is how high it reaches there. Smaller means higher on
    screen, which means louder.
    """
    width, height, rows = read_png(path)
    plot_height = height - 18  # The frequency labels sit below the plot.
    highest = [plot_height] * width
    for y in range(plot_height):
        row = rows[y]
        for x in range(width):
            red, _, blue = row[x]
            if blue > 150 and blue > red + 40 and y < highest[x]:
                highest[x] = y
    return width, plot_height, highest


def spectrum_column(width: int, hz: float) -> int:
    """Which column a frequency lands in, on the same log axis the panel uses."""
    fraction = math.log(hz / 20.0) / math.log(24000.0 / 20.0)
    return max(0, min(width - 1, int(fraction * width)))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="path to the auscultate executable")
    parser.add_argument("--keep", type=Path, help="write the screenshot here instead of a temp dir")
    arguments = parser.parse_args()

    with tempfile.TemporaryDirectory() as directory:
        workspace = Path(directory)
        audio = workspace / "probe.wav"
        shot = arguments.keep if arguments.keep else workspace / "shot.png"
        shot.parent.mkdir(parents=True, exist_ok=True)

        write_probe_wav(audio)

        plot = workspace / "plot.png"
        curve = workspace / "curve.png"
        completed = subprocess.run(
            [
                str(arguments.binary),
                str(audio),
                "--screenshot",
                str(shot),
                "--screenshot-spectrogram",
                str(plot),
                "--screenshot-spectrum",
                str(curve),
                "--print-analysis",
            ],
            capture_output=True,
            text=True,
            timeout=180,
        )
        if completed.returncode != 0:
            print(completed.stdout)
            print(completed.stderr, file=sys.stderr)
            print(f"FAIL: the application exited {completed.returncode}")
            return 1
        if not shot.exists():
            print("FAIL: no screenshot was written")
            return 1

        width, height, rows = read_png(shot)
        if width < 400 or height < 300:
            print(f"FAIL: screenshot is {width}x{height}, far smaller than the window")
            return 1

        if not plot.exists():
            print("FAIL: no spectrogram image was written")
            return 1

        plot_width, plot_height, plot_rows = read_png(plot)

        # The spectrum panel, checked at three points on its frequency axis.
        #
        # The probe sweeps 40 Hz to 18 kHz, so the curve should stand well clear
        # of the floor at 100 Hz, 1 kHz and 10 kHz and fall away above 18 kHz
        # where the sweep stops. Three points inside and one outside pin the
        # mapping: an axis that is reversed, linear instead of logarithmic, or
        # off by an octave fails at least one of them, where a check that the
        # curve merely exists would pass all four.
        if not curve.exists():
            print("FAIL: no spectrum image was written")
            return 1
        curve_width, curve_height, heights = spectrum_curve(curve)
        if curve_width < 120 or curve_height < 80:
            print(f"FAIL: the spectrum panel is {curve_width}x{curve_height}")
            return 1

        floor = curve_height - 1
        inside = {hz: heights[spectrum_column(curve_width, hz)] for hz in (100.0, 1000.0, 10000.0)}
        above = min(heights[spectrum_column(curve_width, hz)] for hz in (21000.0, 23000.0))
        if any(height > curve_height * 0.75 for height in inside.values()):
            print(
                "FAIL: the spectrum is at the floor inside the swept band -- "
                + ", ".join(f"{hz:.0f} Hz at row {row}" for hz, row in inside.items())
                + f" of {floor}"
            )
            return 1
        if above < max(inside.values()) + 8:
            print(
                f"FAIL: the spectrum is as loud above the sweep (row {above}) as inside it "
                f"(row {max(inside.values())}) -- the frequency axis is wrong"
            )
            return 1

        # And the panel follows the selection, which is the whole point of it.
        #
        # The probe sweeps 40 Hz to 18 kHz over six seconds, so the first
        # second holds nothing above about 114 Hz and the last second nothing
        # below about 6 kHz. Rendering each and checking that the curve is at
        # the floor where the sweep was not says three things at once: the
        # selection reaches the analysis, the analysis reaches the picture, and
        # the frequency axis is right at both ends of its range.
        for span, present, absent in (("0-1", 60.0, 1000.0), ("5-6", 12000.0, 1000.0)):
            selected = workspace / f"curve{span}.png"
            done = subprocess.run(
                [
                    str(arguments.binary),
                    str(audio),
                    "--apply",
                    f"select:{span}",
                    "--screenshot-spectrum",
                    str(selected),
                ],
                capture_output=True,
                text=True,
                timeout=180,
            )
            if done.returncode != 0 or not selected.exists():
                print(f"FAIL: selecting {span} s and rendering the spectrum exited "
                      f"{done.returncode} -- {done.stderr}")
                return 1
            span_width, span_height, span_heights = spectrum_curve(selected)
            here = span_heights[spectrum_column(span_width, present)]
            there = span_heights[spectrum_column(span_width, absent)]
            if here > span_height * 0.6:
                print(
                    f"FAIL: with {span} s selected, {present:.0f} Hz is at row {here} of "
                    f"{span_height} -- the sweep is there and the spectrum does not show it"
                )
                return 1
            if there < span_height * 0.9:
                print(
                    f"FAIL: with {span} s selected, {absent:.0f} Hz is at row {there} of "
                    f"{span_height} -- the sweep is not there and the spectrum shows it anyway"
                )
                return 1

        # Markers are drawn, and only when there are markers.
        #
        # The ruler paints them in one colour that appears nowhere else in the
        # window, so counting pixels of it says whether they reached the
        # picture. Checking the empty case as well is what makes it a test of
        # the drawing rather than of the colour constant.
        marker_colour = (0x8F, 0xD6, 0x94)
        counts = []
        for operations in ("deselect", "select:1-2,mark:intro,select:3-4,mark:chorus,deselect"):
            marked = workspace / f"marked{len(counts)}.png"
            done = subprocess.run(
                [str(arguments.binary), str(audio), "--apply", operations,
                 "--screenshot", str(marked)],
                capture_output=True,
                text=True,
                timeout=180,
            )
            if done.returncode != 0 or not marked.exists():
                print(f"FAIL: rendering markers exited {done.returncode} -- {done.stderr}")
                return 1
            _, _, marked_rows = read_png(marked)
            counts.append(sum(1 for row in marked_rows for pixel in row if pixel == marker_colour))
        if counts[0] != 0:
            print(f"FAIL: {counts[0]} marker-coloured pixels with no markers placed")
            return 1
        if counts[1] < 20:
            print(f"FAIL: only {counts[1]} marker-coloured pixels with two markers placed")
            return 1

        # The spectrum reference. Checked by comparing renders rather than by
        # hunting for a colour: the line is dashed, antialiased and drawn with
        # alpha, so no pixel in it holds the constant the code names. What can
        # be said exactly is that setting a reference changes the picture and
        # clearing it puts the picture back, and those are the two things that
        # matter.
        references = {}
        for name, verbs in (
            ("plain", "select:4-8"),
            ("set", "select:0-4,reference,select:4-8"),
            ("cleared", "select:0-4,reference,clearreference,select:4-8"),
        ):
            shot = workspace / f"reference-{name}.png"
            done = subprocess.run(
                [str(arguments.binary), str(audio), "--apply", verbs,
                 "--screenshot-spectrum", str(shot)],
                capture_output=True,
                text=True,
                timeout=300,
            )
            if done.returncode != 0 or not shot.exists():
                print(f"FAIL: spectrum reference '{name}' exited {done.returncode} -- {done.stderr}")
                return 1
            _, _, references[name] = read_png(shot)

        def differing(a, b) -> int:
            return sum(1 for ra, rb in zip(a, b) for pa, pb in zip(ra, rb) if pa != pb)

        changed = differing(references["plain"], references["set"])
        if changed < 200:
            print(f"FAIL: setting a spectrum reference changed only {changed} pixels")
            return 1
        restored = differing(references["plain"], references["cleared"])
        if restored != 0:
            print(f"FAIL: clearing the reference left {restored} pixels changed")
            return 1

        # An empty render is one flat colour. A real one is not.
        region = [pixel for row in plot_rows[::3] for pixel in row[::4]]
        distinct = len(set(region))
        if distinct < 64:
            print(f"FAIL: the spectrogram has only {distinct} distinct colours")
            return 1

        # The sweep rises left to right, so the brightest row near the left edge
        # must sit below the brightest row near the right edge. This is what
        # catches an inverted axis, a mirrored image, or a frozen first tile.
        def brightest_row(x: int) -> int:
            best_row, best = 0, -1
            for y in range(plot_height):
                pixel = plot_rows[y][x]
                luma = pixel[0] * 2 + pixel[1] * 5 + pixel[2]
                if luma > best:
                    best, best_row = luma, y
            return best_row

        left = brightest_row(plot_width // 6)
        right = brightest_row(plot_width * 5 // 6)
        if not left > right:
            print(f"FAIL: the sweep does not rise -- brightest row {left} left, {right} right")
            return 1

        # On a logarithmic axis an exponential sweep is a straight line, so the
        # midpoint has to sit near the middle of the two ends. A linear axis
        # would bow it far off; this is the axis check, not just a rise check.
        middle = brightest_row(plot_width // 2)
        expected = (left + right) / 2
        if abs(middle - expected) > plot_height * 0.08:
            print(
                f"FAIL: the sweep is not straight on a log axis -- rows {left}, "
                f"{middle}, {right}; expected the middle near {expected:.0f}"
            )
            return 1

        # The meters run on a worker thread and are joined before this prints,
        # so an empty dump means the panel never got its result -- which the
        # screenshot checks above would not notice.
        measured: dict[str, float] = {}
        for line in completed.stdout.splitlines():
            if "=" in line:
                key, _, value = line.partition("=")
                try:
                    measured[key] = float(value)
                except ValueError:
                    pass

        for key in ("integrated_lufs", "true_peak_dbtp", "sample_peak_dbfs", "rms_dbfs"):
            if key not in measured:
                print(f"FAIL: the meters did not report {key}")
                return 1

        if measured.get("gated_blocks", 0) < 1:
            print("FAIL: no 400 ms block cleared the absolute gate")
            return 1

        # Relationships that must hold by definition. They catch a meter wired
        # to the wrong field far more reliably than any absolute value, which
        # would only pin the one signal this test happens to use.
        checks = [
            (
                "crest factor is sample peak over RMS",
                abs(
                    measured["crest_factor_db"]
                    - (measured["sample_peak_dbfs"] - measured["rms_dbfs"])
                )
                < 0.05,
            ),
            (
                "true peak is at or above sample peak",
                measured["true_peak_dbtp"] >= measured["sample_peak_dbfs"] - 0.01,
            ),
            (
                "peak to loudness is true peak over integrated loudness",
                abs(
                    measured["peak_to_loudness_lu"]
                    - (measured["true_peak_dbtp"] - measured["integrated_lufs"])
                )
                < 0.05,
            ),
            (
                "a -6 dBFS sweep measures somewhere sane",
                -30.0 < measured["integrated_lufs"] < 0.0,
            ),
        ]
        for name, holds in checks:
            if not holds:
                print(f"FAIL: {name} -- {measured}")
                return 1

        print(
            f"OK: window {width}x{height}, plot {plot_width}x{plot_height}, "
            f"{distinct} distinct colours, sweep straight from row {left} "
            f"through {middle} to {right}; "
            f"{measured['integrated_lufs']:.1f} LUFS, "
            f"{measured['true_peak_dbtp']:.1f} dBTP"
        )
        return 0


if __name__ == "__main__":
    sys.exit(main())
