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
import random
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


def write_wav32(path: Path, samples: list[float]) -> None:
    """A 32-bit source, for tests about what happens when bits are dropped.

    write_wav above makes 16-bit files, which is fine for almost everything and
    useless here: a 16-bit source is already quantised, so converting it to
    16 bits has nothing left to dither and the test would measure the source's
    own distortion twice.
    """
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(4)
        handle.setframerate(SAMPLE_RATE)
        handle.writeframes(
            b"".join(
                struct.pack("<i", int(max(-1.0, min(1.0, s)) * 2147483647)) for s in samples
            )
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


def write_stereo(path: Path, left: list[float], right: list[float]) -> None:
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(2)
        handle.setsampwidth(2)
        handle.setframerate(SAMPLE_RATE)
        handle.writeframes(
            b"".join(
                struct.pack("<hh", int(max(-1.0, min(1.0, a)) * 32767),
                            int(max(-1.0, min(1.0, b)) * 32767))
                for a, b in zip(left, right)
            )
        )


def read_channels(path: Path) -> list[list[float]]:
    """Every channel of a WAV, one list per channel."""
    with wave.open(str(path), "rb") as handle:
        frames, channels, width = (
            handle.getnframes(),
            handle.getnchannels(),
            handle.getsampwidth(),
        )
        raw = handle.readframes(frames)
    scale = float(1 << (width * 8 - 1))
    step = channels * width
    return [
        [
            int.from_bytes(raw[i * step + c * width : i * step + (c + 1) * width],
                           "little", signed=True)
            / scale
            for i in range(frames)
        ]
        for c in range(channels)
    ]


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

        # De-essing. Speech-shaped material: a low tone for the voice and
        # bursts of high-frequency noise for the sibilants, so the two bands
        # can be checked separately.
        print("deess:")
        sibilant = workspace / "sibilant.wav"
        rng_ess = random.Random(3)
        voice: list[float] = []
        state = previous = 0.0
        for i in range(3 * SAMPLE_RATE):
            t = i / SAMPLE_RATE
            if (t % 0.4) < 0.1:
                raw = rng_ess.gauss(0.0, 1.0)
                state = 0.85 * (state + raw - previous)
                previous = raw
                noise = 0.1 * state
            else:
                state = previous = 0.0
                noise = 0.0
            voice.append(0.3 * math.sin(2.0 * math.pi * 220.0 * t) + noise)
        write_wav(sibilant, voice)

        def band_energy(values: list[float], above: bool, hz: float = 5000.0) -> float:
            angle = 2.0 * math.pi * hz / SAMPLE_RATE
            alpha = math.sin(angle) / (1.0 + math.cos(angle))
            low = 0.0
            total = 0.0
            for value in values:
                low += alpha * (value - low)
                part = (value - low) if above else low
                total += part * part
            return total

        deessed = workspace / "deessed.wav"
        result = run("deess", str(sibilant), str(deessed))
        check("deess exits cleanly", result.returncode == 0, result.stderr)
        check("and says what it took", "dB off" in result.stdout, result.stdout)
        if result.returncode == 0 and deessed.exists():
            before, _ = read_wav(sibilant)
            after, _ = read_wav(deessed)
            high_change = 10.0 * math.log10(
                max(band_energy(after, True), 1e-30) / max(band_energy(before, True), 1e-30))
            low_change = 10.0 * math.log10(
                max(band_energy(after, False), 1e-30) / max(band_energy(before, False), 1e-30))
            check("the sibilance band comes down", high_change < -3.0, f"{high_change:.1f} dB")
            # The property that makes it a de-esser rather than a low-pass.
            check("and the voice underneath does not", abs(low_change) < 0.3,
                  f"{low_change:.2f} dB")

        # A tone with nothing above the split is left alone, and the report
        # says so rather than claiming work it did not do.
        quiet_result = run("deess", str(source), str(workspace / "deessed-clean.wav"))
        check("clean material is left alone", "0.0 dB off on 0%" in quiet_result.stdout,
              quiet_result.stdout)

        check("an impossible split frequency is refused",
              run("deess", str(sibilant), str(workspace / "x.wav"),
                  "--frequency", "40000").returncode != 0)

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

        # Channel operations. Written at 24 bits so the comparison can demand
        # bit-exactness: at 16 the average of two 16-bit samples is a half-step
        # that has to be rounded, and the test would be measuring the writer
        # rather than the operation.
        # Dynamics, on a file that is loud then quiet, so the distance
        # between the two halves is the one number that says what happened.
        # The stereo field. Four cases with arithmetically known answers, so
        # the check is against the number rather than against "it reported
        # something".
        # Dither through convert, where the CLI can drop bits.
        # Provenance: what the audio says about where it came from, as
        # opposed to what its header claims.
        # The CSV report, which is what someone actually does with a folder.
        print("csv report:")
        result = run("analyse", str(source), str(noise_source), "--csv")
        check("csv exits cleanly", result.returncode == 0, result.stderr)
        if result.returncode == 0:
            rows = [line for line in result.stdout.splitlines() if line.strip()]
            check("csv has a header and one row per file", len(rows) == 3, f"{len(rows)} lines")
            if len(rows) == 3:
                columns = rows[0].split(",")
                check("csv names its columns", "integratedLufs" in columns and
                      "monoLossDb" in columns, rows[0])
                # A mono file has no stereo field, so those cells are empty
                # rather than zero -- a column mixing real numbers with
                # sentinels is a column nobody can average.
                cells = rows[1].split(",")
                check("csv has the same width on every row",
                      all(len(row.split(",")) == len(columns) for row in rows),
                      f"{[len(r.split(',')) for r in rows]}")
                stereo_at = columns.index("stereoCorrelation")
                check("csv leaves a missing measurement empty",
                      cells[stereo_at] == "", f"'{cells[stereo_at]}'")
                check("csv quotes the filename", cells[0].startswith('"'), cells[0])

        check("csv and json together are refused",
              run("analyse", str(source), "--csv", "--json").returncode != 0)

        # Third-octave and octave bands.
        print("bands:")
        result = run("bands", str(noise_source), "--csv")
        check("bands exits cleanly", result.returncode == 0, result.stderr)
        if result.returncode == 0:
            rows = [line for line in result.stdout.splitlines() if line.strip()]
            check("thirty-one third-octave bands at 48 kHz", len(rows) == 32, f"{len(rows)} lines")
            if len(rows) == 32:
                values = [row.split(",") for row in rows[1:]]
                centres = [float(v[0]) for v in values]
                check("the band centres are the standard series",
                      centres[0] == 20.0 and centres[-1] == 20000.0,
                      f"{centres[0]} .. {centres[-1]}")
                # Each band is a third of an octave above the last.
                ratios = [b / a for a, b in zip(centres, centres[1:])]
                check("and a third of an octave apart",
                      all(abs(r - 2 ** (1 / 3)) < 0.03 for r in ratios),
                      f"{min(ratios):.4f} .. {max(ratios):.4f}")
                # White noise is flat per hertz, so on a band display it rises
                # by 10*log10(2^(1/3)) = 1.0 dB a band. That is the difference
                # between a band picture and an FFT picture, and the check that
                # says the normalisation is right.
                levels = [float(v[3]) for v in values]
                low = levels[centres.index(500.0)]
                high = levels[centres.index(5000.0)]
                per_band = (high - low) / 10.0
                check("white noise rises about 1 dB a band",
                      abs(per_band - 10.0 * math.log10(2 ** (1 / 3))) < 0.25,
                      f"{per_band:.2f} dB a band")

        result = run("bands", str(noise_source), "--octave", "--csv")
        if result.returncode == 0:
            rows = [line for line in result.stdout.splitlines() if line.strip()]
            check("ten octave bands", len(rows) == 11, f"{len(rows)} lines")

        check("bands with two report formats is refused",
              run("bands", str(noise_source), "--json", "--csv").returncode != 0)
        check("bands with no file fails", run("bands").returncode != 0)

        print("provenance:")
        rng = random.Random(11)
        length = 1 << 16
        noise = [rng.gauss(0.0, 0.15) for _ in range(length)]

        def write_depth(path: Path, samples: list[float], width: int) -> None:
            scale = 1 << (width * 8 - 1)
            with wave.open(str(path), "wb") as handle:
                handle.setnchannels(1)
                handle.setsampwidth(width)
                handle.setframerate(SAMPLE_RATE)
                data = bytearray()
                for value in samples:
                    q = int(round(max(-1.0, min(1.0, value)) * scale))
                    data += max(-scale, min(scale - 1, q)).to_bytes(width, "little", signed=True)
                handle.writeframes(bytes(data))

        full = workspace / "prov-full.wav"
        padded = workspace / "prov-padded.wav"
        write_depth(full, noise, 3)
        # The same audio quantised to 16 bits and written as 24: the case this
        # exists for, and one no header can reveal.
        write_depth(padded, [round(v * 32768) / 32768 for v in noise], 3)

        result = run("provenance", str(full), "--json")
        check("provenance exits cleanly", result.returncode == 0, result.stderr)
        if result.returncode == 0:
            report = json.loads(result.stdout)
            check("a full-depth file uses all its bits",
                  report["effectiveBits"] == 24 and report["padded"] is False,
                  f"{report['effectiveBits']} bits, padded={report['padded']}")
            check("and has no encoder edge", report["steepCutoff"] is False,
                  f"cutoff {report['cutoffHz']}")

        result = run("provenance", str(padded), "--json")
        if result.returncode == 0:
            report = json.loads(result.stdout)
            check("a padded file is reported as the depth it really uses",
                  report["effectiveBits"] == 16 and report["padded"] is True,
                  f"{report['effectiveBits']} bits, padded={report['padded']}")

        # A band-limited file, built from partials that stop at 12 kHz, which
        # is a brick wall by construction -- the same shape a lossy encoder
        # leaves behind.
        limited = workspace / "prov-limited.wav"
        band = [0.0] * length
        for k in range(20, int(12000 * length / SAMPLE_RATE), 29):
            hz = k * SAMPLE_RATE / length
            phase = rng.uniform(0.0, 2.0 * math.pi)
            for i in range(length):
                band[i] += math.sin(2.0 * math.pi * hz * i / SAMPLE_RATE + phase)
        loudest = max(abs(v) for v in band) or 1.0
        write_depth(limited, [0.5 * v / loudest for v in band], 3)

        result = run("provenance", str(limited), "--json")
        if result.returncode == 0:
            report = json.loads(result.stdout)
            check("a band-limited file is spotted", report["steepCutoff"] is True,
                  f"cutoff {report['cutoffHz']}")
            if report["cutoffHz"] is not None:
                check("and the cutoff is reported near where it is",
                      10000.0 < report["cutoffHz"] < 16000.0, f"{report['cutoffHz']} Hz")

        check("provenance on a missing file fails",
              run("provenance", str(workspace / "nope.wav")).returncode != 0)
        check("provenance with no file fails", run("provenance").returncode != 0)

        print("dither:")
        quiet_path = workspace / "quiet-cli.wav"
        write_wav32(
            quiet_path,
            [0.0008 * math.sin(2.0 * math.pi * 997.0 * i / SAMPLE_RATE)
             for i in range(2 * SAMPLE_RATE)],
        )

        def goertzel(values: list[float], hz: float) -> float:
            angle = 2.0 * math.pi * hz / SAMPLE_RATE
            coefficient = 2.0 * math.cos(angle)
            s1 = s2 = 0.0
            for value in values:
                s0 = value + coefficient * s1 - s2
                s2, s1 = s1, s0
            return 2.0 * math.hypot(s1 - s2 * math.cos(angle), s2 * math.sin(angle)) / len(values)

        result = run("convert", str(quiet_path), str(workspace / "d-none.wav"), "--format", "16")
        check("convert without dither exits cleanly", result.returncode == 0, result.stderr)
        result = run("convert", str(quiet_path), str(workspace / "d-tpdf.wav"),
                     "--format", "16", "--dither", "tpdf")
        check("convert with dither exits cleanly", result.returncode == 0, result.stderr)

        if (workspace / "d-none.wav").exists() and (workspace / "d-tpdf.wav").exists():
            def worst_ratio(path: Path) -> float:
                values, _ = read_wav(path)
                fundamental = goertzel(values, 997.0)
                worst = max(goertzel(values, 997.0 * k) for k in (3, 5, 7, 9))
                return 20.0 * math.log10(max(worst, 1e-15) / max(fundamental, 1e-15))

            plain = worst_ratio(workspace / "d-none.wav")
            dithered = worst_ratio(workspace / "d-tpdf.wav")
            check("dither takes the quantisation distortion down",
                  plain - dithered > 6.0, f"{plain:.1f} -> {dithered:.1f} dB")

        check("an unknown dither is refused",
              run("convert", str(quiet_path), str(workspace / "x.wav"),
                  "--format", "16", "--dither", "gaussian").returncode != 0)
        check("dither on a float output is accepted and ignored",
              run("convert", str(quiet_path), str(workspace / "x.wav"),
                  "--format", "float", "--dither", "tpdf").returncode == 0)

        print("stereo field:")
        cases = {
            # Both channels identical: mono in all but name.
            "identical": (lambda v: v, 1.0, 0.0, 0.0),
            # Inverted: cancels completely when summed.
            "opposed": (lambda v: -v, -1.0, 0.0, None),
            # Right at half amplitude: still perfectly in phase, but 6 dB left.
            "tilted": (lambda v: 0.5 * v, 1.0, -6.0206, -0.4624),
        }
        for name, (right_of, correlation, balance, mono_loss) in cases.items():
            probe = workspace / f"stereo-{name}.wav"
            left = [0.5 * math.sin(2.0 * math.pi * 440.0 * i / SAMPLE_RATE)
                    for i in range(SAMPLE_RATE)]
            write_stereo(probe, left, [right_of(v) for v in left])

            result = run("analyse", str(probe), "--json")
            check(f"{name} analyses", result.returncode == 0, result.stderr)
            if result.returncode != 0:
                continue
            try:
                measured = json.loads(result.stdout)
            except json.JSONDecodeError as problem:
                failures.append(f"{name}: the JSON did not parse: {problem}")
                continue

            check(f"{name} correlation is {correlation:+.2f}",
                  abs(measured["stereoCorrelation"] - correlation) < 0.01,
                  f"{measured['stereoCorrelation']}")
            check(f"{name} balance is {balance:.1f} dB",
                  abs(measured["stereoBalanceDb"] - balance) < 0.05,
                  f"{measured['stereoBalanceDb']}")
            if mono_loss is None:
                # Total cancellation reads at the floor, not as a small number.
                check(f"{name} cancels in mono", measured["monoLossDb"] < -100.0,
                      f"{measured['monoLossDb']}")
            else:
                check(f"{name} loses {mono_loss:.2f} dB in mono",
                      abs(measured["monoLossDb"] - mono_loss) < 0.05,
                      f"{measured['monoLossDb']}")

        # A mono file has no stereo field, and says so rather than reporting
        # zeroes that would read as measurements.
        result = run("analyse", str(source), "--json")
        if result.returncode == 0:
            measured = json.loads(result.stdout)
            check("a mono file reports no stereo field",
                  measured["stereoCorrelation"] is None and measured["monoLossDb"] is None,
                  f"{measured.get('stereoCorrelation')}, {measured.get('monoLossDb')}")

        print("dynamics:")
        steps = workspace / "loud-then-quiet.wav"
        write_wav(
            steps,
            [
                (0.5 if i < 4 * SAMPLE_RATE else 0.02)
                * math.sin(2.0 * math.pi * 440.0 * i / SAMPLE_RATE)
                for i in range(8 * SAMPLE_RATE)
            ],
        )

        def halves(path: Path) -> tuple[float, float]:
            values, _ = read_wav(path)

            def peak_db(start: int, stop: int) -> float:
                loudest = max((abs(v) for v in values[start:stop]), default=0.0)
                return 20.0 * math.log10(max(loudest, 1e-12))

            return peak_db(2 * SAMPLE_RATE, 3 * SAMPLE_RATE), peak_db(
                6 * SAMPLE_RATE, 7 * SAMPLE_RATE
            )

        loud0, quiet0 = halves(steps)
        compressed = workspace / "compressed.wav"
        result = run("compress", str(steps), str(compressed),
                     "--threshold", "-30", "--ratio", "8")
        check("compress exits cleanly", result.returncode == 0, result.stderr)
        if result.returncode == 0 and compressed.exists():
            loud, quiet = halves(compressed)
            check("compress closes the gap", (loud - quiet) < (loud0 - quiet0) - 10.0,
                  f"{loud0 - quiet0:.1f} -> {loud - quiet:.1f} dB")
            check("and leaves what is under the threshold alone", abs(quiet - quiet0) < 1.0,
                  f"{quiet0:.1f} -> {quiet:.1f} dB")

        gated = workspace / "gated.wav"
        result = run("gate", str(steps), str(gated), "--threshold", "-30", "--depth", "60")
        check("gate exits cleanly", result.returncode == 0, result.stderr)
        if result.returncode == 0 and gated.exists():
            loud, quiet = halves(gated)
            check("gate widens the gap", (loud - quiet) > (loud0 - quiet0) + 30.0,
                  f"{loud0 - quiet0:.1f} -> {loud - quiet:.1f} dB")
            check("and leaves what is over the threshold alone", abs(loud - loud0) < 1.0,
                  f"{loud0:.1f} -> {loud:.1f} dB")

        print("channels:")
        stereo = workspace / "stereo.wav"
        write_stereo(
            stereo,
            [0.5 * math.sin(2.0 * math.pi * 300.0 * i / SAMPLE_RATE) for i in range(SAMPLE_RATE)],
            [0.3 * math.sin(2.0 * math.pi * 1100.0 * i / SAMPLE_RATE + 0.7)
             for i in range(SAMPLE_RATE)],
        )
        pair = read_channels(stereo)
        averaged = [0.5 * (a + b) for a, b in zip(pair[0], pair[1])]
        for operation, wanted in (
            ("reverse", [list(reversed(c)) for c in pair]),
            ("invert", [[-v for v in c] for c in pair]),
            ("swap", [pair[1], pair[0]]),
            ("mono", [averaged, averaged]),
        ):
            output = workspace / f"channels-{operation}.wav"
            result = run("channels", str(stereo), str(output), "--op", operation, "--format", "24")
            check(f"{operation} exits cleanly", result.returncode == 0, result.stderr)
            if result.returncode != 0 or not output.exists():
                continue
            got = read_channels(output)
            if len(got) != len(wanted):
                check(f"{operation} keeps both channels", False, f"{len(got)} channels")
                continue
            worst = max(max(abs(x - y) for x, y in zip(a, b)) for a, b in zip(got, wanted))
            check(f"{operation} is exact to the sample", worst == 0.0, f"worst {worst:.2e}")

        check("swap refuses a mono file",
              run("channels", str(source), str(workspace / "x.wav"), "--op", "swap").returncode
              != 0)
        check("mono refuses a mono file",
              run("channels", str(source), str(workspace / "x.wav"), "--op", "mono").returncode
              != 0)
        check("but reverse accepts one",
              run("channels", str(source), str(workspace / "x.wav"), "--op",
                  "reverse").returncode == 0)

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
        check("compress with no output fails",
              run("compress", str(source)).returncode != 0)
        check("gate on a missing file fails",
              run("gate", str(workspace / "nope.wav"), str(workspace / "x.wav")).returncode != 0)
        check("channels with no --op fails",
              run("channels", str(source), str(workspace / "x.wav")).returncode != 0)
        check("an unknown channel operation fails",
              run("channels", str(source), str(workspace / "x.wav"),
                  "--op", "sideways").returncode != 0)
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
