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
import random
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


def write_tone_mix(path: Path, parts: list[tuple[float, float]], seconds: int) -> None:
    """A sum of pure tones, so what a filter did to each is measurable."""
    samples = bytearray()
    for i in range(SAMPLE_RATE * seconds):
        t = i / SAMPLE_RATE
        value = sum(a * math.sin(2.0 * math.pi * f * t) for f, a in parts)
        samples += struct.pack("<h", max(-32768, min(32767, int(value * 32767))))

    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(SAMPLE_RATE)
        handle.writeframes(bytes(samples))


def amplitude_at(x: list[float], frequency: float, start: int, count: int) -> float:
    """Amplitude of one frequency over a window, by direct correlation.

    A whole DFT would answer a question nobody asked. The windows here are whole
    numbers of cycles of every tone in the probe, so there is nothing to leak.
    """
    real = imaginary = 0.0
    for i in range(count):
        angle = 2.0 * math.pi * frequency * (start + i) / SAMPLE_RATE
        real += x[start + i] * math.cos(angle)
        imaginary += x[start + i] * math.sin(angle)
    return 2.0 * math.hypot(real, imaginary) / count


def butterworth_db(frequency: float, corner: float, high_pass: bool) -> float:
    """What a second-order Butterworth section does at a frequency, in decibels.

    The prediction the filter is held to. Asserting "it got quieter" would pass
    on a filter with the wrong corner, the wrong order or the wrong shape; this
    passes only on the one that was asked for.
    """
    ratio = (frequency / corner) ** 2
    return 20.0 * math.log10((ratio if high_pass else 1.0) / math.sqrt(1.0 + ratio**2))


def error_db(actual: list[float], wanted: list[float]) -> float:
    """Error energy of one signal against another, relative to the signal."""
    error = sum((a - b) ** 2 for a, b in zip(actual, wanted))
    signal = sum(b * b for b in wanted)
    return 10.0 * math.log10(error / signal) if error > 0.0 and signal > 0.0 else -200.0


def shape_error_db(actual: list[float], wanted: list[float]) -> float:
    """Error against a reference with the level difference taken out.

    A declipped file is deliberately quieter than the original -- the restored
    peaks have to fit somewhere. Comparing raw samples would measure that gain
    and call it damage, so both are scaled to the same peak first and what is
    left is the shape.
    """
    actual_peak = max((abs(v) for v in actual), default=0.0)
    wanted_peak = max((abs(v) for v in wanted), default=0.0)
    if actual_peak <= 0.0 or wanted_peak <= 0.0:
        return 0.0
    scale = wanted_peak / actual_peak
    error = sum((a * scale - b) ** 2 for a, b in zip(actual, wanted))
    signal = sum(b * b for b in wanted)
    return 10.0 * math.log10(error / signal) if error > 0.0 and signal > 0.0 else -200.0


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

        # Filtering. The probe is two pure tones an octave and a bit apart, so
        # the textbook 12 dB per octave of a second-order Butterworth is
        # measurable to a tenth of a decibel -- a far stronger statement than
        # "the low end got quieter".
        print("\nfiltering:")
        mix = workspace / "rumble.wav"
        write_tone_mix(mix, [(40.0, 0.4), (1000.0, 0.4)], 6)
        unfiltered = load(mix)
        # Whole cycles of both tones, well clear of either end of the file.
        window_start, window_length = SAMPLE_RATE, SAMPLE_RATE * 4

        def filtered(operations: str, name: str) -> list[float] | None:
            output = workspace / f"{name}.wav"
            completed = subprocess.run(
                [str(arguments.binary), str(mix), "--apply", operations, "--export", str(output)],
                capture_output=True,
                text=True,
                timeout=300,
            )
            if completed.returncode != 0 or not output.exists():
                failures.append(f"{name}: exited {completed.returncode} -- {completed.stderr}")
                return None
            return load(output)

        for operation, corner, high_pass, moved, held in (
            ("highpass:80", 80.0, True, 40.0, 1000.0),
            ("lowpass:300", 300.0, False, 1000.0, 40.0),
        ):
            result = filtered(operation, operation.replace(":", "-"))
            if result is None:
                continue
            if len(result) != len(unfiltered):
                failures.append(f"{operation}: {len(result)} frames, wanted {len(unfiltered)}")
                continue

            for frequency, wanted in (
                (moved, butterworth_db(moved, corner, high_pass)),
                (held, 0.0),
            ):
                was = amplitude_at(unfiltered, frequency, window_start, window_length)
                now = amplitude_at(result, frequency, window_start, window_length)
                measured = 20.0 * math.log10(now / was) if now > 0.0 and was > 0.0 else -200.0
                # The passband has to be left alone far more tightly than the
                # stopband has to hit its figure: a filter that colours what it
                # was not pointed at is the worse fault.
                tolerance = 0.15 if frequency == moved else 0.05
                if abs(measured - wanted) > tolerance:
                    failures.append(
                        f"{operation}: {frequency:.0f} Hz moved {measured:+.2f} dB, "
                        f"wanted {wanted:+.2f} dB"
                    )
                else:
                    print(f"  ok  {operation}: {frequency:.0f} Hz {measured:+.2f} dB "
                          f"(predicted {wanted:+.2f})")

        # Filtering a selection has two ways to go wrong that filtering the
        # whole file does not. The biquad can start cold at the edge, which
        # leaves a settling transient instead of the filter's steady state; and
        # the join can step, because the band is gone on one side of it and
        # present on the other. The run-up answers the first and the edge blend
        # the second, and both are checked here against the whole-file result,
        # which is the only thing either is trying to be.
        whole = filtered("highpass:80", "hp-whole")
        part = filtered("select:2-4,highpass:80", "hp-part")
        if whole is not None and part is not None and len(whole) == len(part) == len(unfiltered):
            start, end = 2 * SAMPLE_RATE, 4 * SAMPLE_RATE
            blend = int(2.0 * SAMPLE_RATE / 80.0) + 1  # Two periods of the corner.

            interior = max(abs(part[i] - whole[i]) for i in range(start + blend, end - blend))
            if interior > 2e-4:
                failures.append(
                    f"filter a selection: inside it, {interior:.5f} away from the same "
                    "region filtered whole -- the run-up is not settling the filter"
                )
            else:
                print(f"  ok  a filtered selection matches whole-file filtering "
                      f"inside it ({interior:.1e})")

            untouched = max(
                abs(part[i] - unfiltered[i])
                for i in list(range(0, start)) + list(range(end, len(unfiltered)))
            )
            if untouched > 2e-4:
                failures.append(
                    f"filter a selection: audio outside it moved by {untouched:.5f}"
                )
            else:
                print(f"  ok  audio outside the selection is untouched ({untouched:.1e})")

            # No click at the join: the step across it is no bigger than the
            # steps the signal takes anyway. Before the edge blend existed this
            # measured 0.166 against a 0.053 neighbourhood.
            join = abs(part[start] - part[start - 1])
            neighbourhood = max(
                abs(part[i] - part[i - 1]) for i in range(start + 200, start + 4000)
            )
            if join > neighbourhood * 1.2:
                failures.append(
                    f"filter a selection: the join steps {join:.5f} where the signal "
                    f"steps {neighbourhood:.5f} -- that is a click"
                )
            else:
                print(f"  ok  no click at the join: {join:.5f} against {neighbourhood:.5f}")

        # Time stretch and pitch shift. The two halves of the same machine, and
        # each one's whole claim is that it changes one thing and leaves the
        # other alone -- so each is checked on both.
        print("\nstretch and shift:")
        note = workspace / "note.wav"
        write_tone_mix(note, [(440.0, 0.4)], 4)
        plain = load(note)

        def middle(x: list[float]) -> tuple[int, int]:
            """The middle half of a signal: clear of both ends whatever its length."""
            return len(x) // 4, len(x) // 2

        def processed(operations: str, name: str) -> list[float] | None:
            output = workspace / f"{name}.wav"
            completed = subprocess.run(
                [str(arguments.binary), str(note), "--apply", operations, "--export", str(output)],
                capture_output=True,
                text=True,
                timeout=600,
            )
            if completed.returncode != 0 or not output.exists():
                failures.append(f"{name}: exited {completed.returncode} -- {completed.stderr}")
                return None
            return load(output)

        for operation, factor in (("stretch:200", 2.0), ("stretch:50", 0.5), ("stretch:137", 1.37)):
            result = processed(operation, operation.replace(":", "-"))
            if result is None:
                continue
            wanted = round(len(plain) * factor)
            if len(result) != wanted:
                failures.append(f"{operation}: {len(result)} frames, wanted {wanted}")
                continue
            # The note has to survive at its own pitch and its own level. A
            # stretch done by resampling would pass the length and fail this.
            held = amplitude_at(result, 440.0, *middle(result))
            if abs(held - 0.4) > 0.01:
                failures.append(f"{operation}: 440 Hz came out at {held:.4f}, wanted 0.4")
            else:
                print(f"  ok  {operation}: {len(result)} frames, 440 Hz still at {held:.4f}")

        for operation, semitones in (("pitch:12", 12.0), ("pitch:-7", -7.0), ("pitch:0.5", 0.5)):
            result = processed(operation, operation.replace(":", "").replace(".", "-"))
            if result is None:
                continue
            if len(result) != len(plain):
                failures.append(
                    f"{operation}: {len(result)} frames, wanted {len(plain)} -- a shift must "
                    "not change the length"
                )
                continue
            wanted = 440.0 * 2.0 ** (semitones / 12.0)
            moved = amplitude_at(result, wanted, *middle(result))
            stayed = amplitude_at(result, 440.0, *middle(result))
            if abs(moved - 0.4) > 0.015:
                failures.append(
                    f"{operation}: {wanted:.2f} Hz came out at {moved:.4f}, wanted 0.4"
                )
            elif stayed > 0.02:
                failures.append(
                    f"{operation}: 440 Hz is still there at {stayed:.4f} -- it was joined, "
                    "not moved"
                )
            else:
                print(f"  ok  {operation}: {wanted:.2f} Hz at {moved:.4f}, nothing left at 440")

        # Stretching a selection moves everything after it and nothing before
        # it, and both edges have to be exact or the document has drifted.
        result = processed("select:1-2,stretch:200", "stretch-part")
        if result is not None:
            wanted = len(plain) + SAMPLE_RATE
            if len(result) != wanted:
                failures.append(
                    f"stretch a selection: {len(result)} frames, wanted {wanted}"
                )
            else:
                head = max(abs(result[i] - plain[i]) for i in range(SAMPLE_RATE))
                tail = max(
                    abs(result[len(result) - 1 - i] - plain[len(plain) - 1 - i])
                    for i in range(2 * SAMPLE_RATE - 1)
                )
                if max(head, tail) > 2e-4:
                    failures.append(
                        f"stretch a selection: audio outside it moved -- head {head:.5f}, "
                        f"tail {tail:.5f}"
                    )
                else:
                    print(
                        f"  ok  stretching a selection: {len(result)} frames, and what was "
                        "outside it is untouched"
                    )

        # Declicking. Two claims, and the second matters more: it takes the
        # damage out, and it leaves undamaged audio exactly alone. A
        # restoration tool that quietly rewrites clean material is worse than
        # none, because nobody hears what it did until the master has shipped.
        print("\ndeclick:")
        rng = random.Random(4)
        clean_samples: list[float] = []
        for i in range(SAMPLE_RATE * 4):
            t_seconds = i / SAMPLE_RATE
            clean_samples.append(
                0.35 * math.sin(2.0 * math.pi * 220.0 * t_seconds)
                + 0.20 * math.sin(2.0 * math.pi * 553.0 * t_seconds)
                + 0.10 * math.sin(2.0 * math.pi * 1310.0 * t_seconds)
                + rng.gauss(0.0, 0.01)
            )
        damaged_samples = list(clean_samples)
        clicks = list(range(5000, len(clean_samples) - 5000, 4300))
        for place in clicks:
            height = rng.uniform(0.4, 0.9) * rng.choice([1.0, -1.0])
            for k in range(rng.randint(1, 4)):
                damaged_samples[place + k] += height

        def write_samples(path: Path, values: list[float]) -> None:
            raw = bytearray()
            for value in values:
                scaled = max(-8388608, min(8388607, int(value * 8388607)))
                raw += (scaled & 0xFFFFFF).to_bytes(3, "little")
            with wave.open(str(path), "wb") as handle:
                handle.setnchannels(1)
                handle.setsampwidth(3)
                handle.setframerate(SAMPLE_RATE)
                handle.writeframes(bytes(raw))

        clean_file = workspace / "clean.wav"
        damaged_file = workspace / "damaged.wav"
        write_samples(clean_file, clean_samples)
        write_samples(damaged_file, damaged_samples)
        reference = load(clean_file)

        def declicked(source_file: Path, operations: str, name: str) -> list[float] | None:
            output = workspace / f"{name}.wav"
            completed = subprocess.run(
                [
                    str(arguments.binary),
                    str(source_file),
                    "--apply",
                    operations,
                    "--export",
                    str(output),
                ],
                capture_output=True,
                text=True,
                timeout=600,
            )
            if completed.returncode != 0 or not output.exists():
                failures.append(f"{name}: exited {completed.returncode} -- {completed.stderr}")
                return None
            return load(output)

        before = error_db(load(damaged_file), reference)
        repaired = declicked(damaged_file, "declick", "declicked")
        if repaired is not None:
            after = error_db(repaired, reference)
            if after > before - 25.0:
                failures.append(
                    f"declick: {len(clicks)} clicks, error {before:.1f} dB -> {after:.1f} dB, "
                    "wanted at least 25 dB better"
                )
            else:
                print(
                    f"  ok  declick: {len(clicks)} clicks, error {before:.1f} dB -> "
                    f"{after:.1f} dB"
                )

        untouched = declicked(clean_file, "declick", "declicked-clean")
        if untouched is not None:
            moved = max((abs(a - b) for a, b in zip(untouched, reference)), default=0.0)
            if moved > 0.0:
                failures.append(
                    f"declick: clean material moved by {moved:.6f} -- it found damage that "
                    "was not there"
                )
            else:
                print("  ok  declick: clean material comes back bit-identical")

        # Declipping. The peaks have to come back, the waveform has to be
        # closer to the undamaged one than it was, and the result has to still
        # fit in a file -- a restoration that clips on export has undone
        # itself.
        print("\ndeclip:")
        peak = max(abs(v) for v in clean_samples)
        clip_level = 0.7 * peak
        clipped_samples = [max(-clip_level, min(clip_level, v)) for v in clean_samples]
        clipped_file = workspace / "clipped.wav"
        write_samples(clipped_file, clipped_samples)

        restored = declicked(clipped_file, "declip", "declipped")
        if restored is not None:
            before = shape_error_db(load(clipped_file), reference)
            after = shape_error_db(restored, reference)
            restored_peak = max(abs(v) for v in restored)
            clipped_peak = max(abs(v) for v in load(clipped_file))
            if after > before - 15.0:
                failures.append(
                    f"declip: shape error {before:.1f} dB -> {after:.1f} dB, wanted at "
                    "least 15 dB better"
                )
            elif restored_peak <= clipped_peak:
                failures.append(
                    f"declip: peak went {clipped_peak:.4f} -> {restored_peak:.4f}; the peaks "
                    "did not come back"
                )
            elif restored_peak > 1.0:
                failures.append(
                    f"declip: peak came out at {restored_peak:.4f}, which clips on export"
                )
            else:
                print(
                    f"  ok  declip: shape {before:.1f} dB -> {after:.1f} dB, peak "
                    f"{clipped_peak:.4f} -> {restored_peak:.4f}, still under full scale"
                )

        unclipped = declicked(clean_file, "declip", "declipped-clean")
        if unclipped is not None:
            moved = max((abs(a - b) for a, b in zip(unclipped, reference)), default=0.0)
            if moved > 2e-4:
                failures.append(
                    f"declip: unclipped material moved by {moved:.6f} -- it found clipping "
                    "that was not there"
                )
            else:
                print("  ok  declip: unclipped material is left alone")

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
