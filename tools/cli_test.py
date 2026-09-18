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

        print("failure handling:")
        check("a missing file fails", run("analyse", str(workspace / "nope.wav")).returncode != 0)
        check("an unknown command fails", run("frobnicate").returncode != 0)
        check("an unknown target fails",
              run("normalise", str(source), str(workspace / "x.wav"),
                  "--target", "not-a-platform").returncode != 0)
        check("no arguments prints usage and fails", run().returncode != 0)

    if failures:
        print()
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1
    print("\nOK: the headless driver does what it says on every command")
    return 0


if __name__ == "__main__":
    sys.exit(main())
