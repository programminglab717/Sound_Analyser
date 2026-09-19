#!/usr/bin/env python3
"""End-to-end check of the headless driver.

sa-cli is the path a batch user takes and the one a script depends on, so its
output and its exit codes are an interface, not a convenience. This drives every
command against generated audio and checks the numbers that come back.

No third-party imports: `wave` reads everything we write.
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

SAMPLE_RATE = 48000


def write_wav(path: Path, samples: list[float], rate: int = SAMPLE_RATE) -> None:
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(rate)
        handle.writeframes(
            b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s)) * 32767)) for s in samples)
        )


def read_wav(path: Path) -> tuple[list[float], int]:
    with wave.open(str(path), "rb") as handle:
        frames, channels, width, rate = (
            handle.getnframes(),
            handle.getnchannels(),
            handle.getsampwidth(),
            handle.getframerate(),
        )
        raw = handle.readframes(frames)
    scale = float(1 << (width * 8 - 1))
    step = channels * width
    return (
        [
            int.from_bytes(raw[i * step : i * step + width], "little", signed=True) / scale
            for i in range(frames)
        ],
        rate,
    )


def rms(values: list[float]) -> float:
    return math.sqrt(sum(v * v for v in values) / max(1, len(values)))


def tone_amplitude(values: list[float], hz: float, rate: int) -> float:
    real = imaginary = 0.0
    for i, v in enumerate(values):
        phase = 2.0 * math.pi * hz * i / rate
        real += v * math.cos(phase)
        imaginary += v * math.sin(phase)
    return 2.0 * math.hypot(real, imaginary) / max(1, len(values))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    arguments = parser.parse_args()
    failures: list[str] = []

    def run(*args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(arguments.binary), *args], capture_output=True, text=True, timeout=300
        )

    def check(name: str, condition: bool, detail: str = "") -> None:
        if condition:
            print(f"  ok  {name}")
        else:
            failures.append(f"{name}{': ' + detail if detail else ''}")

    with tempfile.TemporaryDirectory() as directory:
        workspace = Path(directory)

        # A 1 kHz tone at a known amplitude, plus hiss for the denoise test.
        seconds = 6
        frames = SAMPLE_RATE * seconds
        state = 12345
        tone, noisy = [], []
        for i in range(frames):
            state = (1103515245 * state + 12345) & 0x7FFFFFFF
            hiss = (state / 0x7FFFFFFF - 0.5) * 0.06
            t = i / SAMPLE_RATE
            value = 0.5 * math.sin(2.0 * math.pi * 1000.0 * t)
            tone.append(value)
            noisy.append(hiss + (0.3 * math.sin(2.0 * math.pi * 1000.0 * t) if t >= 2.0 else 0.0))

        source = workspace / "tone.wav"
        noise_source = workspace / "noisy.wav"
        write_wav(source, tone)
        write_wav(noise_source, noisy)

        print("analyse:")
        result = run("analyse", str(source), "--json")
        check("exits cleanly", result.returncode == 0, result.stderr)
        try:
            measured = json.loads(result.stdout)
        except json.JSONDecodeError as error:
            failures.append(f"--json did not produce JSON: {error}")
            measured = {}

        check("reports the rate", measured.get("sampleRate") == SAMPLE_RATE)
        check("reports the length", measured.get("frames") == frames)
        # A full-scale sine is -3.01 dBFS; this one is half scale, so -9.03.
        check(
            "sample peak is right for a half-scale sine",
            abs(measured.get("samplePeakDbfs", 0) - (-6.02)) < 0.05,
            str(measured.get("samplePeakDbfs")),
        )
        check(
            "rms is right for a half-scale sine",
            abs(measured.get("rmsDbfs", 0) - (-9.03)) < 0.05,
            str(measured.get("rmsDbfs")),
        )

        print("convert:")
        converted = workspace / "at441.wav"
        result = run("convert", str(source), str(converted), "--rate", "44100", "--format", "24")
        check("exits cleanly", result.returncode == 0, result.stderr)
        if converted.exists():
            samples, rate = read_wav(converted)
            check("the rate changed", rate == 44100, str(rate))
            check(
                "the duration is preserved",
                abs(len(samples) / 44100 - seconds) < 0.01,
                f"{len(samples) / 44100:.4f} s",
            )
            # The tone must come out at the same frequency and amplitude. A
            # resampler that got the ratio wrong shifts the pitch; one that got
            # the gain wrong shows here rather than in a spectrum.
            middle = samples[44100 : 44100 * 5]
            check(
                "the tone survives at 1 kHz",
                abs(tone_amplitude(middle, 1000.0, 44100) - 0.5) < 0.01,
                f"{tone_amplitude(middle, 1000.0, 44100):.4f}",
            )

        print("normalise:")
        normalised = workspace / "normalised.wav"
        result = run("normalise", str(source), str(normalised), "--target", "EBU R128")
        check("exits cleanly", result.returncode == 0, result.stderr)
        after = json.loads(run("analyse", str(normalised), "--json").stdout or "{}")
        check(
            "lands on the target",
            abs(after.get("integratedLufs", 0) - (-23.0)) < 0.1,
            str(after.get("integratedLufs")),
        )

        print("denoise:")
        cleaned = workspace / "cleaned.wav"
        result = run("denoise", str(noise_source), str(cleaned), "--noise", "0-1.8",
                     "--amount", "18")
        check("exits cleanly", result.returncode == 0, result.stderr)
        if cleaned.exists():
            before_samples, _ = read_wav(noise_source)
            after_samples, _ = read_wav(cleaned)
            quiet = slice(0, int(1.5 * SAMPLE_RATE))
            loud = slice(3 * SAMPLE_RATE, 5 * SAMPLE_RATE)
            floor_change = 20.0 * math.log10(
                rms(after_samples[quiet]) / rms(before_samples[quiet])
            )
            tone_change = 20.0 * math.log10(
                tone_amplitude(after_samples[loud], 1000.0, SAMPLE_RATE)
                / tone_amplitude(before_samples[loud], 1000.0, SAMPLE_RATE)
            )
            check("the noise floor drops", floor_change < -8.0, f"{floor_change:.1f} dB")
            check("the tone survives", tone_change > -1.0, f"{tone_change:.2f} dB")

        print("declick:")
        clicked = workspace / "clicked.wav"
        clean_samples, _ = read_wav(source)
        damaged_samples = list(clean_samples)
        places = list(range(4000, len(clean_samples) - 4000, 4300))
        for index, place in enumerate(places):
            height = 0.5 if index % 2 == 0 else -0.6
            damaged_samples[place] += height
            damaged_samples[place + 1] += height
        write_wav(clicked, damaged_samples)

        repaired_file = workspace / "declicked.wav"
        result = run("declick", str(clicked), str(repaired_file))
        check("exits cleanly", result.returncode == 0, result.stderr)
        check("says what it found", "click" in result.stdout, result.stdout)
        if repaired_file.exists():
            repaired_samples, _ = read_wav(repaired_file)

            def error_db(actual: list[float], wanted: list[float]) -> float:
                error = sum((a - b) ** 2 for a, b in zip(actual, wanted))
                signal = sum(b * b for b in wanted)
                return 10.0 * math.log10(error / signal) if error > 0 and signal > 0 else -200.0

            before = error_db(damaged_samples, clean_samples)
            after = error_db(repaired_samples, clean_samples)
            check("the clicks are gone", after < before - 20.0, f"{before:.1f} -> {after:.1f} dB")

        print("dehum:")
        # Not the 1 kHz tone the other checks use, for two reasons that are
        # really one: it is perfectly steady, and 1000 Hz is exactly the
        # twentieth harmonic of 50. A held sine at a hum harmonic *is* hum by
        # every available test -- sa-dsp's own tests pin that -- so using it
        # here would be checking the pathological case rather than the ordinary
        # one. 1037 Hz is not a multiple of 50 and is left alone.
        carrier = [
            0.4 * math.sin(2.0 * math.pi * 1037.0 * i / SAMPLE_RATE)
            for i in range(len(clean_samples))
        ]
        hum_file = workspace / "hummy.wav"
        hummed = [
            v + sum((0.02 / k) * math.sin(2.0 * math.pi * 50.0 * k * i / SAMPLE_RATE + 0.3 * k)
                    for k in range(1, 41))
            for i, v in enumerate(carrier)
        ]
        write_wav(hum_file, hummed)
        dehummed_file = workspace / "dehummed.wav"
        result = run("dehum", str(hum_file), str(dehummed_file))
        check("exits cleanly", result.returncode == 0, result.stderr)
        check("names the frequency it found", "50." in result.stdout, result.stdout)
        if dehummed_file.exists():
            dehummed_samples, rate = read_wav(dehummed_file)
            before = tone_amplitude(hummed, 50.0, rate)
            after = tone_amplitude(dehummed_samples, 50.0, rate)
            check("the hum is gone", after < before * 0.1, f"{before:.5f} -> {after:.5f}")
            survived = tone_amplitude(dehummed_samples, 1037.0, rate)
            check(
                "the tone it was sitting under survives",
                abs(survived - 0.4) < 0.01,
                f"1037 Hz came out at {survived:.4f}, wanted 0.4",
            )

        print("declip:")
        # Deliberately not the 1 kHz tone the other checks use. A single
        # sustained tone, clipped every cycle, is the one case a model-based
        # declipper cannot help with -- the flat top is the shape, repeated,
        # with no unclipped example to learn the real peak from. sa-dsp's own
        # tests pin that limit; here the point is the ordinary case, so the
        # material is three partials that do not share a period.
        rich_samples = []
        for i in range(len(clean_samples)):
            t_seconds = i / SAMPLE_RATE
            rich_samples.append(
                0.55 * math.sin(2.0 * math.pi * 180.0 * t_seconds)
                + 0.28 * math.sin(2.0 * math.pi * 431.0 * t_seconds)
                + 0.14 * math.sin(2.0 * math.pi * 1103.0 * t_seconds)
            )
        peak = max(abs(v) for v in rich_samples)
        level = 0.7 * peak
        clipped_samples = [max(-level, min(level, v)) for v in rich_samples]
        clipped_file = workspace / "clipped.wav"
        write_wav(clipped_file, clipped_samples)

        declipped_file = workspace / "declipped.wav"
        result = run("declip", str(clipped_file), str(declipped_file))
        check("exits cleanly", result.returncode == 0, result.stderr)
        check("says what it restored", "clipped peak" in result.stdout, result.stdout)
        if declipped_file.exists():
            declipped_samples, _ = read_wav(declipped_file)
            restored_peak = max(abs(v) for v in declipped_samples)
            check("the peaks came back", restored_peak > level + 0.01,
                  f"{level:.4f} -> {restored_peak:.4f}")
            check("and it still fits in a file", restored_peak <= 1.0, f"{restored_peak:.4f}")

        print("stretch:")
        longer = workspace / "longer.wav"
        result = run("stretch", str(source), str(longer), "--length", "175")
        check("exits cleanly", result.returncode == 0, result.stderr)
        if longer.exists():
            before_samples, _ = read_wav(source)
            after_samples, rate = read_wav(longer)
            wanted = round(len(before_samples) * 1.75)
            check("the length is what was asked for", len(after_samples) == wanted,
                  f"{len(after_samples)} frames, wanted {wanted}")
            middle = after_samples[len(after_samples) // 4 : 3 * len(after_samples) // 4]
            held = tone_amplitude(middle, 1000.0, rate)
            was = tone_amplitude(before_samples, 1000.0, rate)
            check("the pitch did not move", abs(held - was) < 0.02 * max(was, 1e-9),
                  f"{held:.4f} against {was:.4f}")

        print("pitch:")
        higher = workspace / "higher.wav"
        result = run("pitch", str(source), str(higher), "--semitones", "5")
        check("exits cleanly", result.returncode == 0, result.stderr)
        if higher.exists():
            before_samples, _ = read_wav(source)
            after_samples, rate = read_wav(higher)
            check("the length did not move", len(after_samples) == len(before_samples),
                  f"{len(after_samples)} frames, wanted {len(before_samples)}")
            middle = after_samples[len(after_samples) // 4 : 3 * len(after_samples) // 4]
            wanted_hz = 1000.0 * 2.0 ** (5.0 / 12.0)
            moved = tone_amplitude(middle, wanted_hz, rate)
            left = tone_amplitude(middle, 1000.0, rate)
            was = tone_amplitude(before_samples, 1000.0, rate)
            check("the tone moved up a fourth", abs(moved - was) < 0.05 * max(was, 1e-9),
                  f"{wanted_hz:.1f} Hz at {moved:.4f}, wanted {was:.4f}")
            check("and nothing was left behind", left < 0.05 * max(was, 1e-9), f"{left:.4f}")

        print("failure handling:")
        check("a missing file fails", run("analyse", str(workspace / "nope.wav")).returncode != 0)
        check("an unknown command fails", run("frobnicate").returncode != 0)
        check("an unknown target fails",
              run("normalise", str(source), str(workspace / "x.wav"),
                  "--target", "not-a-platform").returncode != 0)
        check("no arguments prints usage and fails", run().returncode != 0)
        check("a stretch with no length fails",
              run("stretch", str(source), str(workspace / "x.wav")).returncode != 0)
        check("an impossible stretch fails",
              run("stretch", str(source), str(workspace / "x.wav"),
                  "--length", "5000").returncode != 0)
        check("a shift past three octaves fails",
              run("pitch", str(source), str(workspace / "x.wav"),
                  "--semitones", "99").returncode != 0)

    if failures:
        print()
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1
    print("\nOK: the headless driver does what it says on every command")
    return 0


if __name__ == "__main__":
    sys.exit(main())
