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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="path to the sound-analyser executable")
    parser.add_argument("--keep", type=Path, help="write the screenshot here instead of a temp dir")
    arguments = parser.parse_args()

    with tempfile.TemporaryDirectory() as directory:
        workspace = Path(directory)
        audio = workspace / "probe.wav"
        shot = arguments.keep if arguments.keep else workspace / "shot.png"
        shot.parent.mkdir(parents=True, exist_ok=True)

        write_probe_wav(audio)

        plot = workspace / "plot.png"
        completed = subprocess.run(
            [
                str(arguments.binary),
                str(audio),
                "--screenshot",
                str(shot),
                "--screenshot-spectrogram",
                str(plot),
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

        print(
            f"OK: window {width}x{height}, plot {plot_width}x{plot_height}, "
            f"{distinct} distinct colours, sweep straight from row {left} "
            f"through {middle} to {right}"
        )
        return 0


if __name__ == "__main__":
    sys.exit(main())
