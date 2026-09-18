#!/usr/bin/env python3
"""End-to-end check that editing through the window really edits the audio.

The rendering test proves the analysis path draws the right picture. This one
proves the editing path moves the right samples: it drives the window headlessly
through select, cut, paste, trim, silence and undo, exports each result, and
compares the exported samples against what the operation was supposed to do.

An editor that opens files and draws them beautifully but cuts the wrong four
seconds would pass every other check in this repository.

No third-party imports: `wave` from the standard library reads everything we
write.
"""

from __future__ import annotations

import argparse
import math
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

SAMPLE_RATE = 48000
SECONDS = 12
BLOCK = 4  # The probe is three four-second blocks of distinct material.


def write_probe_wav(path: Path) -> None:
    """Three four-second blocks at 200, 700 and 3000 Hz.

    Distinct blocks are what make the assertions meaningful: after moving one,
    the test can tell which block landed where rather than just counting frames.
    """
    frames = SAMPLE_RATE * SECONDS
    samples = bytearray()
    for i in range(frames):
        t = i / SAMPLE_RATE
        frequency = (200.0, 700.0, 3000.0)[min(2, int(t) // BLOCK)]
        value = 0.5 * math.sin(2.0 * math.pi * frequency * t)
        samples += struct.pack("<h", int(value * 32767))

    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(SAMPLE_RATE)
        handle.writeframes(bytes(samples))


def load(path: Path) -> list[float]:
    """Read a mono or stereo WAV's first channel as floats in [-1, 1]."""
    with wave.open(str(path), "rb") as handle:
        frames = handle.getnframes()
        channels = handle.getnchannels()
        width = handle.getsampwidth()
        raw = handle.readframes(frames)

    scale = float(1 << (width * 8 - 1))
    step = channels * width
    return [
        int.from_bytes(raw[i * step : i * step + width], "little", signed=True) / scale
        for i in range(frames)
    ]


def worst_difference(a: list[float], b: list[float]) -> float:
    if len(a) != len(b):
        return float("inf")
    return max((abs(x - y) for x, y in zip(a, b)), default=0.0)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="path to the sound-analyser executable")
    arguments = parser.parse_args()

    failures: list[str] = []

    with tempfile.TemporaryDirectory() as directory:
        workspace = Path(directory)
        source = workspace / "probe.wav"
        write_probe_wav(source)
        original = load(source)
        block = SAMPLE_RATE * BLOCK

        def run(operations: str, name: str) -> list[float] | None:
            output = workspace / f"{name}.wav"
            completed = subprocess.run(
                [
                    str(arguments.binary),
                    str(source),
                    "--apply",
                    operations,
                    "--export",
                    str(output),
                ],
                capture_output=True,
                text=True,
                timeout=180,
            )
            if completed.returncode != 0 or not output.exists():
                failures.append(f"{name}: exited {completed.returncode} -- {completed.stderr}")
                return None
            return load(output)

        def expect(name: str, actual: list[float] | None, wanted: list[float]) -> None:
            if actual is None:
                return
            difference = worst_difference(actual, wanted)
            # Round-tripping 16-bit through float and out as 24-bit is exact, so
            # the only tolerance needed is for the float comparison itself.
            if difference > 2e-4:
                failures.append(
                    f"{name}: {len(actual)} frames, worst sample difference {difference:.6f}"
                    + (f" (expected {len(wanted)} frames)" if len(actual) != len(wanted) else "")
                )
            else:
                print(f"  ok  {name}: {len(actual)} frames, worst difference {difference:.2e}")

        print("editing checks:")
        expect(
            "cut the middle block",
            run("select:4-8,cut", "cut"),
            original[:block] + original[2 * block :],
        )
        expect(
            "cut then undo restores the original",
            run("select:4-8,cut,undo", "undo"),
            original,
        )
        expect(
            "cut then undo then redo cuts again",
            run("select:4-8,cut,undo,redo", "redo"),
            original[:block] + original[2 * block :],
        )
        expect(
            "cut and paste moves a block to the front",
            run("select:4-8,cut,select:0-0,paste", "moved"),
            original[block : 2 * block] + original[:block] + original[2 * block :],
        )
        expect(
            "silence keeps the timing and zeroes the range",
            run("select:4-8,silence", "silenced"),
            original[:block] + [0.0] * block + original[2 * block :],
        )
        expect(
            "trim to the last block",
            run("select:8-12,trim", "trim-tail"),
            original[2 * block :],
        )
        expect(
            "trim to the first block",
            run("select:0-4,trim", "trim-head"),
            original[:block],
        )
        expect(
            "trim to the middle block",
            run("select:4-8,trim", "trim-middle"),
            original[block : 2 * block],
        )
        expect(
            "trim to everything is a no-op, not an error",
            run("selectall,trim", "trim-all"),
            original,
        )
        expect(
            "deleting nothing leaves the document alone",
            run("deselect,delete", "delete-none"),
            original,
        )

        # The mastering loop: measure, apply the correction the meter advises,
        # write the file, then measure the file that came out. Every stage has
        # to agree or this does not land on the target, which makes it the
        # strongest single check in the project -- it covers the meters, the
        # gain verb, the render and the writer at once.
        print("\nmastering loop:")
        loudness = workspace / "normalised.wav"
        completed = subprocess.run(
            [
                str(arguments.binary),
                str(source),
                "--apply",
                "normalise",
                "--export",
                str(loudness),
            ],
            capture_output=True,
            text=True,
            timeout=300,
        )
        if completed.returncode != 0 or not loudness.exists():
            failures.append(f"normalise: exited {completed.returncode} -- {completed.stderr}")
        else:
            remeasured = subprocess.run(
                [str(arguments.binary), str(loudness), "--print-analysis"],
                capture_output=True,
                text=True,
                timeout=300,
            )
            values = {}
            for line in remeasured.stdout.splitlines():
                key, _, value = line.partition("=")
                try:
                    values[key] = float(value)
                except ValueError:
                    pass

            integrated = values.get("integrated_lufs")
            if integrated is None:
                failures.append("normalise: the exported file did not measure")
            elif abs(integrated - (-23.0)) > 0.1:
                failures.append(
                    f"normalise: exported file reads {integrated:.3f} LUFS, wanted -23.0"
                )
            else:
                print(f"  ok  normalise to EBU R128: exported file reads {integrated:.3f} LUFS")

        # A gain is exact arithmetic, so a -6 dB correction must show up as
        # exactly -6 dB on both the loudness and the peak.
        before = {}
        after = {}
        for target, arguments_list in (
            (before, ["--print-analysis"]),
            (after, ["--apply", "gain:-6", "--print-analysis"]),
        ):
            result = subprocess.run(
                [str(arguments.binary), str(source), *arguments_list],
                capture_output=True,
                text=True,
                timeout=300,
            )
            for line in result.stdout.splitlines():
                key, _, value = line.partition("=")
                try:
                    target[key] = float(value)
                except ValueError:
                    pass

        for key in ("integrated_lufs", "sample_peak_dbfs"):
            if key not in before or key not in after:
                failures.append(f"gain: no {key} measured")
            elif abs((before[key] - after[key]) - 6.0) > 0.01:
                failures.append(
                    f"gain: -6 dB moved {key} by {before[key] - after[key]:.3f} dB"
                )
        if not failures:
            print("  ok  gain: -6 dB moved loudness and peak by exactly 6 dB")

        # Playback, against the null device. That device runs a real thread on a
        # real clock, so this exercises the ring, the render worker, the
        # callback and the position counter -- everything except the final
        # hand-off to hardware, which no machine without a sound card can check.
        print("\ntransport:")
        played = subprocess.run(
            [str(arguments.binary), str(source), "--apply", "select:4-6", "--play"],
            capture_output=True,
            text=True,
            timeout=300,
        )
        transport = {}
        for line in played.stdout.splitlines():
            key, _, value = line.partition("=")
            try:
                transport[key] = int(value)
            except ValueError:
                pass

        if played.returncode != 0:
            failures.append(f"play: exited {played.returncode} -- {played.stderr}")
        elif transport.get("played_from") != 4 * SAMPLE_RATE:
            failures.append(f"play: started at {transport.get('played_from')}, wanted 192000")
        elif not (2 * SAMPLE_RATE <= transport.get("played_to", 0) - 4 * SAMPLE_RATE < 2.2 * SAMPLE_RATE):
            failures.append(
                f"play: reached {transport.get('played_to')}, wanted about 288000"
            )
        elif transport.get("underruns", -1) != 0:
            failures.append(f"play: {transport.get('underruns')} underruns")
        else:
            print(
                f"  ok  played {transport['played_from']} to {transport['played_to']}, "
                "no underruns"
            )

    if failures:
        print()
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1

    print("\nOK: every operation moved exactly the samples it should have")
    return 0


if __name__ == "__main__":
    sys.exit(main())
