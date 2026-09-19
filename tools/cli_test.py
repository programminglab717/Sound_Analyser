#!/usr/bin/env python3
"""End-to-end check of the headless driver.

auscultate-cli is the path a batch user takes and the one a script depends on, so its
output and its exit codes are an interface, not a convenience. This drives every
command against generated audio and checks the numbers that come back.

No third-party imports: `wave` reads everything we write.
"""

from __future__ import annotations

import argparse
import hashlib
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


def write_wav_codes(path: Path, channels: list[list[int]], width: int,
                    rate: int = SAMPLE_RATE) -> None:
    """A WAV built from exact integer sample codes, one list per channel.

    Exactness is the whole point of it. A lossless round trip has to be checked
    as equality, and equality only means something if what went in sat on a code
    to begin with -- otherwise the test is measuring float rounding rather than
    the writer.
    """
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(len(channels))
        handle.setsampwidth(width)
        handle.setframerate(rate)
        data = bytearray()
        for i in range(len(channels[0])):
            for channel in channels:
                data += int(channel[i]).to_bytes(width, "little", signed=True)
        handle.writeframes(bytes(data))


def raw_frames(path: Path) -> tuple[bytes, int, int, int]:
    """(payload, channels, bytes per sample, rate) straight out of a WAV."""
    with wave.open(str(path), "rb") as handle:
        return (
            handle.readframes(handle.getnframes()),
            handle.getnchannels(),
            handle.getsampwidth(),
            handle.getframerate(),
        )


def flac_stream_info(path: Path) -> dict:
    """The 34 bytes of STREAMINFO, unpacked.

    Read here rather than taken from the tool that wrote it, because a writer
    agreeing with itself about what it wrote proves nothing.
    """
    raw = path.read_bytes()
    if raw[:4] != b"fLaC":
        raise ValueError("not a native FLAC stream")
    body = raw[8 : 8 + 34]
    packed = int.from_bytes(body[10:18], "big")
    return {
        "minBlock": int.from_bytes(body[0:2], "big"),
        "maxBlock": int.from_bytes(body[2:4], "big"),
        "minFrame": int.from_bytes(body[4:7], "big"),
        "maxFrame": int.from_bytes(body[7:10], "big"),
        "rate": packed >> 44,
        "channels": ((packed >> 41) & 0x7) + 1,
        "bits": ((packed >> 36) & 0x1F) + 1,
        "samples": packed & 0xFFFFFFFFF,
        "md5": body[18:34].hex(),
    }


def aiff_chunks(path: Path) -> tuple[str, int, dict[str, tuple[int, int]]]:
    """(form type, declared FORM size, {chunk id: (body offset, declared size)})."""
    raw = path.read_bytes()
    if raw[:4] != b"FORM":
        raise ValueError("not a FORM container")
    form_size = int.from_bytes(raw[4:8], "big")
    chunks: dict[str, tuple[int, int]] = {}
    offset = 12
    while offset + 8 <= len(raw):
        identifier = raw[offset : offset + 4].decode("ascii", "replace")
        size = int.from_bytes(raw[offset + 4 : offset + 8], "big")
        chunks[identifier] = (offset + 8, size)
        offset += 8 + size + (size & 1)
    return raw[8:12].decode("ascii", "replace"), form_size, chunks


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
        # Room acoustics from an impulse response whose decay rate is known,
        # so the check is against the right answer rather than against "it
        # printed something".
        print("room:")

        def write_ir(path: Path, t60: float) -> None:
            # An envelope exp(-t/tau) decays 8.6859 dB per tau in energy, so
            # sixty decibels takes 6.9078*tau and tau = t60/6.9078.
            tau = t60 * math.log10(math.e) * 2.0 * 10.0 / 60.0
            rng_ir = random.Random(17)
            write_wav32(
                path,
                [0.9 * math.exp(-(i / SAMPLE_RATE) / tau) * rng_ir.gauss(0.0, 1.0)
                 for i in range(int(SAMPLE_RATE * t60 * 2.0))],
            )

        for t60 in (0.4, 1.2):
            ir = workspace / f"ir-{t60}.wav"
            write_ir(ir, t60)
            result = run("room", str(ir), "--json")
            check(f"room exits cleanly on a {t60}s decay", result.returncode == 0, result.stderr)
            if result.returncode != 0:
                continue
            report = json.loads(result.stdout)
            measured = report["channels"][0]
            check(f"{t60}s decay is valid", measured["valid"] is True, str(measured))
            # Both reverberation times have to recover the decay they were made
            # from. Five per cent is generous for a synthetic decay and tight
            # enough to catch the missing-tail-compensation bug, which read
            # 0.547 for a 1.000 s decay.
            for key in ("t20Seconds", "t30Seconds"):
                value = measured[key]
                check(f"{key} recovers {t60}s",
                      value is not None and abs(value - t60) < 0.05 * t60,
                      f"{value}")

        # Clarity falls as a room gets livelier. Direction, independent of any
        # absolute calibration.
        clarities = []
        for t60 in (0.4, 1.2):
            result = run("room", str(workspace / f"ir-{t60}.wav"), "--json")
            if result.returncode == 0:
                clarities.append(json.loads(result.stdout)["channels"][0]["c80Db"])
        check("a livelier room reads as less clear",
              len(clarities) == 2 and clarities[1] < clarities[0], str(clarities))

        # Per-octave reverberation. A decay that is the same at every
        # frequency has to read the same in every band, which is the check that
        # the per-band filtering is not itself colouring the answer.
        result = run("room", str(workspace / "ir-1.2.wav"), "--bands", "--json")
        check("room --bands exits cleanly", result.returncode == 0, result.stderr)
        if result.returncode == 0:
            bands = json.loads(result.stdout)["bands"]
            check("bands cover the octave centres", len(bands) >= 8, f"{len(bands)} bands")
            measured = [b["t20Seconds"] for b in bands if b["t20Seconds"] is not None]
            check("most bands measure something", len(measured) >= 5, f"{len(measured)}")
            if measured:
                # Wide, because the lowest bands have few cycles in a short
                # record and are genuinely noisier; the point is that no band
                # is wildly off, not that they agree to a percent.
                check("and every band agrees with the decay it was made from",
                      all(abs(v - 1.2) < 0.35 for v in measured),
                      f"{min(measured):.3f} .. {max(measured):.3f}")

        check("room with no file fails", run("room").returncode != 0)
        check("room on a missing file fails",
              run("room", str(workspace / "nope.wav")).returncode != 0)

        # Measuring an impulse response with a sweep, end to end through files.
        # The room here is three discrete arrivals rather than a decay: a
        # sparse response can be convolved in pure Python in a moment, and it
        # says more about whether the deconvolution is placing things
        # correctly than a wash of noise would. The decay case is covered by
        # the C++ tests, which can afford a dense one.
        print("sweep and deconvolve:")
        sweep_file = workspace / "sweep.wav"
        sweep_args = ("--seconds", "0.5", "--start", "100", "--end", "12000")
        result = run("sweep", str(sweep_file), *sweep_args, "--format", "24")
        check("sweep exits cleanly", result.returncode == 0, result.stderr)
        check("sweep says what it wrote", "0.50 s" in result.stdout, result.stdout)

        if result.returncode == 0:
            played, sweep_rate = read_wav(sweep_file)
            check("the sweep is as long as it was asked for",
                  abs(len(played) - 24000) <= 1, f"{len(played)} frames")
            check("the sweep stays inside the file",
                  max(abs(v) for v in played) <= 1.0)

            # A room: direct sound, an early reflection with its polarity
            # flipped, and a later one. Positions in samples so there is
            # nothing to round.
            taps = [(0, 1.0), (511, -0.5), (1303, 0.3)]
            recorded = [0.0] * (len(played) + 1400)
            for offset, gain in taps:
                for i, value in enumerate(played):
                    recorded[i + offset] += value * gain
            loudest = max(abs(v) for v in recorded)
            recorded = [v * (0.5 / loudest) for v in recorded]
            recording = workspace / "recording.wav"
            write_wav32(recording, recorded)

            impulse = workspace / "impulse.wav"
            result = run("deconvolve", str(recording), str(impulse), *sweep_args,
                         "--keep", "0.05", "--format", "24")
            check("deconvolve exits cleanly", result.returncode == 0, result.stderr)
            check("deconvolve reports the headroom it found",
                  "above the last tenth" in result.stdout, result.stdout)

            if result.returncode == 0:
                answer, _ = read_wav(impulse)
                # A recording of length R is a sweep of length N through a
                # response of length L, so R = N + L - 1 and there are only
                # R - N + 1 samples of measurement to be had. --keep asked for
                # 0.05 s, which is 2400 samples and more than exists; the
                # shorter of the two has to win, because the rest would be the
                # deconvolution's own residual dressed up as a room.
                informative = len(recorded) - len(played) + 1
                check("the impulse response stops where the measurement does",
                      len(answer) == informative,
                      f"{len(answer)} frames, {informative} available")
                direct = answer[0]
                check("the direct sound is at sample zero",
                      max(range(len(answer)), key=lambda i: abs(answer[i])) == 0,
                      f"peak at {max(range(len(answer)), key=lambda i: abs(answer[i]))}")
                # Each reflection at the sample it was put at, and at the level
                # it was given relative to the direct sound. The polarity of
                # the first one is the point: a measurement that loses the sign
                # of a reflection is not measuring a room.
                for offset, gain in taps[1:]:
                    got = answer[offset] / direct
                    check(f"the arrival at {offset} comes back at {gain}",
                          abs(got - gain) < 0.06, f"{got:+.4f}")
                between = max(abs(answer[i]) for i in range(60, 451)) / abs(direct)
                check("and nothing is smeared between them",
                      between < 0.05, f"{between:.4f}")

            # And where --keep is the shorter of the two, it is the one that
            # binds: 0.01 s is 480 samples, well inside what is available.
            short = workspace / "impulse-short.wav"
            result = run("deconvolve", str(recording), str(short), *sweep_args,
                         "--keep", "0.01", "--format", "24")
            check("deconvolve honours a --keep shorter than the measurement",
                  result.returncode == 0 and len(read_wav(short)[0]) == 480,
                  result.stderr or f"{len(read_wav(short)[0])} frames")

        # The flags have to match what was played. Deconvolving against a sweep
        # that was not the one used is not a smaller error, it is a different
        # measurement, and it should not quietly return something.
        if sweep_file.exists():
            mismatched = run("deconvolve", str(workspace / "recording.wav"),
                             str(workspace / "wrong.wav"), "--seconds", "2.0",
                             "--format", "24")
            check("deconvolving a recording shorter than the sweep fails",
                  mismatched.returncode != 0, mismatched.stdout)

        check("sweep with no output file fails", run("sweep").returncode != 0)
        check("deconvolve with no output file fails",
              run("deconvolve", str(workspace / "recording.wav")).returncode != 0)
        check("deconvolve on a missing file fails",
              run("deconvolve", str(workspace / "nope.wav"),
                  str(workspace / "out.wav")).returncode != 0)

        print("tempo:")

        def beat_track(bpm: float, seconds: float, offset: float = 0.0) -> list[float]:
            """Decaying tone bursts on the beat, with a quieter one between.

            Bursts rather than clicks because a click is a single sample and an
            onset detector has an easy time of it; a burst with an attack and a
            decay is closer to a drum and harder.
            """
            total = int(SAMPLE_RATE * seconds)
            out = [0.0] * total
            beat = 60.0 / bpm

            def burst(at: float, hz: float, amplitude: float) -> None:
                start = int(at * SAMPLE_RATE)
                for i in range(int(0.12 * SAMPLE_RATE)):
                    if start + i >= total:
                        break
                    t = i / SAMPLE_RATE
                    out[start + i] += (amplitude * math.exp(-t / 0.03)
                                       * math.sin(2.0 * math.pi * hz * t))

            index = 0
            while offset + index * beat < seconds:
                burst(offset + index * beat, 160.0, 0.5)
                if offset + (index + 0.5) * beat < seconds:
                    burst(offset + (index + 0.5) * beat, 320.0, 0.15)
                index += 1
            return out

        for bpm in (90.0, 128.0):
            track = workspace / f"beat-{int(bpm)}.wav"
            write_wav(track, beat_track(bpm, 10.0, offset=0.7))
            result = run("tempo", str(track), "--json")
            check(f"tempo exits cleanly at {int(bpm)} BPM", result.returncode == 0, result.stderr)
            if result.returncode != 0:
                continue
            report = json.loads(result.stdout)
            check(f"{int(bpm)} BPM is found", report["valid"] is True, str(report["valid"]))
            check(f"{int(bpm)} BPM is right to within one",
                  report["bpm"] is not None and abs(report["bpm"] - bpm) < 1.0,
                  str(report["bpm"]))
            check(f"{int(bpm)} BPM is a confident answer",
                  report["confidence"] > 0.5, str(report["confidence"]))

            # The grid runs one beat before the first onset -- a beat where
            # nothing was played, which is what makes it a grid rather than a
            # list of onsets. So beat k lines up with the onset at
            # 0.7 + (k-1)*60/bpm, and every one has to land within one hop
            # (256 samples, 5.3 ms) of where it was played.
            beat = 60.0 / bpm
            beats = report["beats"]
            check(f"{int(bpm)} BPM grid starts one beat before the first onset",
                  abs(beats[0] - (0.7 - beat)) < beat * 0.2, str(beats[0]))
            worst = 0.0
            for index, at in enumerate(beats):
                truth = 0.7 + (index - 1) * beat
                if 0.7 <= truth < 10.0:
                    worst = max(worst, abs(at - truth))
            check(f"{int(bpm)} BPM beats land within one hop of the onsets",
                  worst < 256.0 / SAMPLE_RATE, f"{worst * SAMPLE_RATE:.0f} samples")

        # A held chord has no tempo. Reporting one would be worse than useless,
        # and this is not an error -- it is the right answer.
        chord = workspace / "chord.wav"
        write_wav(chord, [
            0.25 * sum(math.sin(2.0 * math.pi * hz * i / SAMPLE_RATE)
                       for hz in (261.63, 329.63, 392.0)) / 3.0
            for i in range(SAMPLE_RATE * 6)
        ])
        result = run("tempo", str(chord), "--json")
        check("tempo exits cleanly on a held chord", result.returncode == 0, result.stderr)
        if result.returncode == 0:
            check("a held chord has no tempo",
                  json.loads(result.stdout)["valid"] is False, result.stdout)
            check("and the plain output says so",
                  "no tempo found" in run("tempo", str(chord)).stdout)

        check("tempo with no file fails", run("tempo").returncode != 0)
        check("tempo on a missing file fails",
              run("tempo", str(workspace / "nope.wav")).returncode != 0)
        check("tempo with an inverted range fails",
              run("tempo", str(workspace / "beat-90.wav"),
                  "--min", "200", "--max", "60").returncode != 0)

        print("pitch-of:")

        def sawtooth(hz: float, seconds: float) -> list[float]:
            """Harmonics at 1/h, so the detector has to find the fundamental
            rather than the loudest partial. A pure sine would not test that."""
            count = int(SAMPLE_RATE * seconds)
            partials = [hz * h for h in range(1, 20) if hz * h < SAMPLE_RATE * 0.45]
            return [
                0.13 * sum(math.sin(2.0 * math.pi * p * i / SAMPLE_RATE) / (n + 1)
                           for n, p in enumerate(partials))
                for i in range(count)
            ]

        for hz in (110.0, 220.0, 440.0):
            tone = workspace / f"tone-{int(hz)}.wav"
            write_wav(tone, sawtooth(hz, 1.0))
            result = run("pitch-of", str(tone), "--json")
            check(f"pitch-of exits cleanly on {int(hz)} Hz", result.returncode == 0, result.stderr)
            if result.returncode != 0:
                continue
            report = json.loads(result.stdout)
            # Half a per cent, which is a fourteenth of a semitone. Parabolic
            # interpolation is what makes that reachable at all: the nearest
            # integer lag for 440 Hz at 48 kHz is 109 samples, which alone
            # would read 440.4 Hz.
            check(f"{int(hz)} Hz comes back within 0.5%",
                  report["medianHz"] is not None and abs(report["medianHz"] - hz) < 0.005 * hz,
                  str(report["medianHz"]))
            check(f"{int(hz)} Hz is voiced throughout",
                  report["voicedFraction"] > 0.9, str(report["voicedFraction"]))

        # Noise has no period, so nothing should be voiced and no pitch
        # reported -- a detector that always returns a number is worse than one
        # that admits it found nothing.
        hiss = workspace / "hiss.wav"
        rng_pitch = random.Random(29)
        write_wav(hiss, [0.2 * rng_pitch.gauss(0.0, 1.0) for _ in range(SAMPLE_RATE)])
        result = run("pitch-of", str(hiss), "--json")
        check("pitch-of exits cleanly on noise", result.returncode == 0, result.stderr)
        if result.returncode == 0:
            report = json.loads(result.stdout)
            check("noise is mostly unvoiced", report["voicedFraction"] < 0.2,
                  str(report["voicedFraction"]))

        result = run("pitch-of", str(workspace / "tone-220.wav"), "--csv")
        check("pitch-of --csv exits cleanly", result.returncode == 0, result.stderr)
        if result.returncode == 0:
            rows = [line for line in result.stdout.splitlines() if line.strip()]
            check("the contour has a header and many rows", len(rows) > 50, f"{len(rows)} lines")
            check("the header names its columns",
                  rows[0] == "seconds,hz,confidence,voiced", rows[0])

        check("pitch-of with no file fails", run("pitch-of").returncode != 0)
        check("pitch-of on a missing file fails",
              run("pitch-of", str(workspace / "nope.wav")).returncode != 0)

        print("null:")
        base = sawtooth(220.0, 1.0)
        reference = workspace / "null-reference.wav"
        write_wav32(reference, base)

        same = workspace / "null-same.wav"
        write_wav32(same, base)
        result = run("null", str(reference), str(same), "--json")
        check("null exits cleanly", result.returncode == 0, result.stderr)
        if result.returncode == 0:
            report = json.loads(result.stdout)
            check("a file against itself is identical", report["verdict"] == "bit-identical",
                  report["verdict"])
            check("with no delay and no gain",
                  report["delaySamples"] == 0 and abs(report["gainDb"]) < 0.001, str(report))

        # Delayed by 137 samples and halved. Both are exact operations on a
        # float, so this has to null completely: 20*log10(0.5) = -6.0206 dB.
        moved = workspace / "null-moved.wav"
        write_wav32(moved, [0.0] * 137 + [v * 0.5 for v in base])
        result = run("null", str(reference), str(moved), "--json")
        check("null exits cleanly on a shifted copy", result.returncode == 0, result.stderr)
        if result.returncode == 0:
            report = json.loads(result.stdout)
            check("the delay is found exactly", report["delaySamples"] == 137,
                  str(report["delaySamples"]))
            check("and the level", abs(report["gainDb"] - -6.0206) < 0.01, str(report["gainDb"]))
            check("and it nulls", report["residualDb"] < -100.0, str(report["residualDb"]))

        # Noise added at a known level. The residual is that noise, so it has
        # to read back at the level it was added at, within a decibel.
        rng_null = random.Random(31)
        reference_rms = rms(base)
        noise_rms = reference_rms * (10.0 ** (-30.0 / 20.0))
        dirty = workspace / "null-dirty.wav"
        write_wav32(dirty, [v + noise_rms * rng_null.gauss(0.0, 1.0) for v in base])
        result = run("null", str(reference), str(dirty), "--json")
        check("null exits cleanly on an altered copy", result.returncode == 0, result.stderr)
        if result.returncode == 0:
            report = json.loads(result.stdout)
            check("an altered copy is reported as different",
                  report["verdict"] == "different", report["verdict"])
            check("and the residual is the level the noise was added at",
                  abs(report["residualDb"] - -30.0) < 1.5, str(report["residualDb"]))
            check("the band table carries the reference level too",
                  all("referenceDb" in band for band in report["bands"]), str(report["bands"][:1]))

        # Polarity inversion nulls at 0 dB under a magnitude gain, so it has to
        # be reported separately or it reads exactly like two identical files.
        flipped = workspace / "null-flipped.wav"
        write_wav32(flipped, [-v for v in base])
        result = run("null", str(reference), str(flipped), "--json")
        if result.returncode == 0:
            report = json.loads(result.stdout)
            check("a polarity inversion is reported as one",
                  report["polarityInverted"] is True, str(report))

        check("null with one file fails", run("null", str(reference)).returncode != 0)
        check("null on a missing file fails",
              run("null", str(reference), str(workspace / "nope.wav")).returncode != 0)

        print("contour:")
        # Ten seconds at one level then ten exactly 10 dB down. An integrated
        # figure cannot tell those apart from twenty seconds in between; the
        # contour is the thing that can.
        step = workspace / "step.wav"
        step_samples = []
        for i in range(SAMPLE_RATE * 20):
            amplitude = 0.2 if i < SAMPLE_RATE * 10 else 0.2 * (10.0 ** (-10.0 / 20.0))
            step_samples.append(amplitude * math.sin(2.0 * math.pi * 1000.0 * i / SAMPLE_RATE))
        write_stereo(step, step_samples, step_samples)

        result = run("contour", str(step), "--json")
        check("contour exits cleanly", result.returncode == 0, result.stderr)
        if result.returncode == 0:
            report = json.loads(result.stdout)
            # The two short-term extremes are the two levels, so their
            # difference has to be the step that was built.
            spread = report["loudestShortTermLufs"] - report["quietestShortTermLufs"]
            check("the short-term spread is the step that was built",
                  abs(spread - 10.0) < 0.2, f"{spread:.3f}")
            check("and the loudness range agrees",
                  abs(report["loudnessRangeLu"] - 10.0) < 0.5,
                  str(report["loudnessRangeLu"]))

        result = run("contour", str(step), "--csv")
        check("contour --csv exits cleanly", result.returncode == 0, result.stderr)
        if result.returncode == 0:
            rows = [r.split(",") for r in result.stdout.splitlines() if r.strip()]
            check("the contour names its columns",
                  rows[0] == ["seconds", "momentaryLufs", "shortTermLufs", "truePeakDbtp",
                              "psrDb", "crestDb"], str(rows[0]))
            at = {r[0]: r for r in rows[1:]}
            # Before 400 ms there is no momentary window and before 3 s no
            # short-term one, so those cells are empty rather than zero. A
            # zero would read as a measurement of digital silence.
            check("no momentary reading before its window has filled",
                  at["0.2000"][1] == "", str(at.get("0.2000")))
            check("no short-term reading before its window has filled",
                  at["2.0000"][2] == "", str(at.get("2.0000")))
            check("and both are present once they have",
                  at["5.0000"][1] != "" and at["5.0000"][2] != "", str(at.get("5.0000")))
            # A sine's crest factor is 20*log10(sqrt 2) = 3.0103 dB.
            check("crest factor of a sine is 3.01 dB",
                  abs(float(at["5.0000"][5]) - 3.0103) < 0.05, at["5.0000"][5])
            # Momentary reacts in 400 ms and short-term in 3 s, so half a
            # second after the step the first has moved and the second has not.
            momentary_after = float(at["10.5000"][1])
            short_after = float(at["10.5000"][2])
            check("momentary reacts to a step faster than short-term",
                  momentary_after < short_after - 5.0,
                  f"{momentary_after:.2f} vs {short_after:.2f}")

        check("contour with no file fails", run("contour").returncode != 0)
        check("contour with a nonsense interval is refused",
              run("contour", str(step), "--interval", "0").returncode != 0)

        print("dereverb:")
        # A room of known reverberation, measured before and after. The claim
        # is specific: it attenuates the tail without shortening the room, so
        # EDT and clarity move a long way and T30 barely moves. Asserting both
        # halves is what stops the tool quietly starting to claim more.
        reverb_ir = workspace / "room.wav"
        rng_room = random.Random(21)
        t60 = 0.8
        tau = t60 / (3.0 * math.log(10.0))
        room = [math.exp(-(i / SAMPLE_RATE) / tau) * rng_room.gauss(0.0, 1.0)
                for i in range(int(SAMPLE_RATE * t60 * 1.6))]
        room[0] += 3.0
        peak_room = max(abs(v) for v in room)
        write_wav32(reverb_ir, [v / peak_room * 0.9 for v in room])

        before = run("room", str(reverb_ir), "--json")
        check("room measures the reverberant impulse response",
              before.returncode == 0, before.stderr)
        dry_ir = workspace / "room-dry.wav"
        result = run("dereverb", str(reverb_ir), str(dry_ir), "--decay", "0.8")
        check("dereverb exits cleanly", result.returncode == 0, result.stderr)
        check("and says what it does not do",
              "does not shorten the room" in result.stdout, result.stdout)

        after = run("room", str(dry_ir), "--json")
        if before.returncode == 0 and result.returncode == 0 and after.returncode == 0:
            was = json.loads(before.stdout)["channels"][0]
            now = json.loads(after.stdout)["channels"][0]
            # Measured here: EDT 0.817 -> 0.511, C50 1.37 -> 3.47 dB.
            check("early decay is markedly shorter",
                  now["edtSeconds"] < was["edtSeconds"] * 0.75,
                  f'{was["edtSeconds"]:.3f} -> {now["edtSeconds"]:.3f}')
            check("and clarity is up by at least a decibel and a half",
                  now["c50Db"] > was["c50Db"] + 1.5,
                  f'{was["c50Db"]:.2f} -> {now["c50Db"]:.2f}')
            # The other half of the claim, and the one a tool is tempted to
            # overstate: the decay rate is essentially untouched. If T30 ever
            # starts moving like EDT does, something changed that the header
            # does not describe.
            check("but the decay rate is essentially untouched",
                  now["t30Seconds"] > was["t30Seconds"] * 0.85,
                  f'{was["t30Seconds"]:.3f} -> {now["t30Seconds"]:.3f}')

        check("dereverb with no output fails", run("dereverb", str(reverb_ir)).returncode != 0)
        check("dereverb on a missing file fails",
              run("dereverb", str(workspace / "nope.wav"),
                  str(workspace / "out.wav")).returncode != 0)
        check("dereverb with a negative amount is refused",
              run("dereverb", str(reverb_ir), str(workspace / "out.wav"),
                  "--amount", "-5").returncode != 0)

        print("key:")

        def render_progression(path: Path, chords: list[tuple[list[int], int]],
                               transpose: int) -> None:
            """Chord progressions built from sawtooth-ish notes.

            Six harmonics at 1/h, because a fold onto pitch classes has to cope
            with harmonics -- a pure sine would make the test easier than the
            job is.
            """
            beat = SAMPLE_RATE // 2
            total = sum(beat * count for _, count in chords) * 2
            samples = [0.0] * total
            cursor = 0
            for _ in range(2):
                for intervals, count in chords:
                    length = beat * count
                    fade = min(length // 8, SAMPLE_RATE // 100)
                    for interval in intervals:
                        hz = 440.0 * 2.0 ** ((60 + transpose + interval - 69) / 12.0)
                        partials = [hz * h for h in range(1, 7) if hz * h < SAMPLE_RATE * 0.45]
                        gain = 0.12 / len(intervals)
                        for i in range(length):
                            if cursor + i >= total:
                                break
                            t = i / SAMPLE_RATE
                            value = sum(
                                math.sin(2.0 * math.pi * p * t) / (n + 1)
                                for n, p in enumerate(partials)
                            )
                            if i < fade:
                                envelope = i / fade
                            elif i > length - fade:
                                envelope = (length - i) / fade
                            else:
                                envelope = 1.0
                            samples[cursor + i] += value * gain * envelope
                    cursor += length
            write_wav(path, samples)

        major = [([0, 4, 7, 12], 2), ([5, 9, 12], 1), ([7, 11, 14], 1), ([0, 4, 7, 12], 2)]
        minor = [([0, 3, 7, 12], 2), ([5, 8, 12], 1), ([7, 11, 14], 1), ([0, 3, 7, 12], 2)]

        # Two keys a tritone apart in the major, and one in the minor whose
        # relative major is a different answer again -- so a detector that had
        # simply learnt one answer could not pass all three.
        for name, chords, transpose, wanted in (
            ("d-major", major, 2, "D major"),
            ("g#-major", major, 8, "G# major"),
            ("f#-minor", minor, 6, "F# minor"),
        ):
            music = workspace / f"{name}.wav"
            render_progression(music, chords, transpose)
            result = run("key", str(music), "--json")
            check(f"key exits cleanly on {name}", result.returncode == 0, result.stderr)
            if result.returncode != 0:
                continue
            report = json.loads(result.stdout)
            check(f"{name} is read as {wanted}", report["key"] == wanted, report["key"])
            # Tonal music makes a strongly shaped chroma; this is the number a
            # caller would threshold on and it has to be high here.
            check(f"{name} is a confident answer", report["strength"] > 0.6,
                  str(report["strength"]))
            # Rendered at exactly A = 440, so the tuning estimate has to agree.
            check(f"{name} reads as being at concert pitch",
                  abs(report["tuningOffsetCents"]) < 12.0, str(report["tuningOffsetCents"]))
            chroma = report["chroma"]
            check(f"{name} chroma has twelve entries and sums to one",
                  len(chroma) == 12 and abs(sum(chroma) - 1.0) < 0.01, str(sum(chroma)))

        # All twelve notes evenly: there is no key, and the strength has to say
        # so rather than reporting whichever way the noise leaned.
        chromatic = [([semitone], 1) for semitone in range(12)]
        atonal = workspace / "chromatic.wav"
        render_progression(atonal, chromatic, 0)
        result = run("key", str(atonal), "--json")
        check("key exits cleanly on atonal material", result.returncode == 0, result.stderr)
        if result.returncode == 0:
            report = json.loads(result.stdout)
            check("atonal material is not a confident key",
                  report["strength"] < 0.15, str(report["strength"]))
            check("and the plain output says why",
                  "no key here to find" in run("key", str(atonal)).stdout)

        check("key with no file fails", run("key").returncode != 0)
        check("key on a missing file fails",
              run("key", str(workspace / "nope.wav")).returncode != 0)
        check("key on a channel that is not there fails",
              run("key", str(workspace / "d-major.wav"), "--channel", "7").returncode != 0)

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

        # The filter-bank path, which measures the same bands a different way.
        result = run("bands", str(noise_source), "--octave", "--filters", "--csv")
        check("bands --filters exits cleanly", result.returncode == 0, result.stderr)
        if result.returncode == 0:
            rows = [r for r in result.stdout.splitlines() if r.strip()]
            check("bands --filters reports ten octaves", len(rows) == 11, f"{len(rows)} lines")
            centres = [float(r.split(",")[0]) for r in rows[1:]]
            # The exact base-ten centres, not the preferred numbers: 1000 Hz
            # times 10^(3n/10), so the band above 1 kHz is at 1995.26 and not
            # at 2000. Both names are right and they are the same band.
            check("and centres them on the base-ten values",
                  any(abs(c - 1995.26) < 1.0 for c in centres), str(centres))

        # A full-scale sine at a band centre has to read the same through both
        # paths. The bank reports a plain RMS, where a sine sits 3.01 dB below
        # its peak, and the transform path is sine-referenced; if that 3.01 is
        # not applied the two disagree by exactly that and neither says so.
        sine = workspace / "band-sine.wav"
        write_wav32(sine, [0.999 * math.sin(2.0 * math.pi * 1000.0 * i / SAMPLE_RATE)
                           for i in range(SAMPLE_RATE * 2)])

        def level_at(args: list[str], want: float) -> float | None:
            out = run("bands", str(sine), "--octave", *args, "--csv")
            if out.returncode != 0:
                return None
            for row in out.stdout.splitlines()[1:]:
                if not row.strip():
                    continue
                parts = row.split(",")
                if abs(float(parts[0]) - want) < 1.0:
                    return float(parts[3])
            return None

        through_transform = level_at([], 1000.0)
        through_filters = level_at(["--filters"], 1000.0)
        check("a full-scale sine reads the same through both band methods",
              through_transform is not None and through_filters is not None
              and abs(through_transform - through_filters) < 0.1,
              f"{through_transform} vs {through_filters}")
        # And both read it at its actual level: 20*log10(0.999) = -0.0087 dB.
        check("and reads it at the level it was written at",
              through_filters is not None and abs(through_filters - -0.0087) < 0.2,
              str(through_filters))

        check("bands --filters with a nonsense order is refused",
              run("bands", str(noise_source), "--filters", "--order", "7").returncode != 0)

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

        # -------------------------------------------------------------------
        # Output formats
        #
        # The editor reads WAV, AIFF, FLAC and MP3, and for a long time wrote
        # only WAV, so a FLAC opened for a two-second edit came back five times
        # the size. What is checked here is not that the files exist but that
        # they are the files they claim to be: a lossless format that does not
        # give the samples back is worse than no lossless format at all.
        # -------------------------------------------------------------------
        print("output formats:")

        # A 24-bit stereo source on exact codes, so equality is meaningful.
        exact_frames = 3 * SAMPLE_RATE
        left = [int(round(0.6 * 8388607 * math.sin(2.0 * math.pi * 440.0 * i / SAMPLE_RATE)))
                for i in range(exact_frames)]
        right = [int(round(0.35 * 8388607 * math.sin(2.0 * math.pi * 661.0 * i / SAMPLE_RATE)))
                 for i in range(exact_frames)]
        exact_source = workspace / "exact24.wav"
        write_wav_codes(exact_source, [left, right], 3)
        original, channels, width, rate = raw_frames(exact_source)

        as_flac = workspace / "exact.flac"
        result = run("convert", str(exact_source), str(as_flac))
        check("convert writes a FLAC when asked for one", result.returncode == 0, result.stderr)
        check("the FLAC starts with the FLAC magic",
              as_flac.exists() and as_flac.read_bytes()[:4] == b"fLaC")

        if as_flac.exists():
            info = flac_stream_info(as_flac)
            check("STREAMINFO states the rate", info["rate"] == rate, str(info["rate"]))
            check("STREAMINFO states the channels", info["channels"] == channels,
                  str(info["channels"]))
            check("STREAMINFO states the depth", info["bits"] == width * 8, str(info["bits"]))
            check("STREAMINFO states the length", info["samples"] == exact_frames,
                  str(info["samples"]))
            check("STREAMINFO carries frame-size extremes",
                  0 < info["minFrame"] <= info["maxFrame"],
                  f"{info['minFrame']}..{info['maxFrame']}")
            # The signature FLAC specifies is an MD5 of the samples as they
            # would be laid out uncompressed, which is exactly the WAV payload.
            # hashlib has nothing to do with our MD5, so this checks both the
            # digest and the layout it was taken over.
            check("STREAMINFO's MD5 matches the audio it covers",
                  info["md5"] == hashlib.md5(original).hexdigest(), info["md5"])
            check("the FLAC is materially smaller than the WAV",
                  as_flac.stat().st_size < exact_source.stat().st_size * 0.75,
                  f"{as_flac.stat().st_size} vs {exact_source.stat().st_size}")

            # The claim in full: decode it again and it is the same file.
            from_flac = workspace / "from-flac.wav"
            result = run("convert", str(as_flac), str(from_flac))
            check("the FLAC opens again", result.returncode == 0, result.stderr)
            if from_flac.exists():
                back, back_channels, back_width, back_rate = raw_frames(from_flac)
                check("FLAC round-trips bit-identically", back == original,
                      f"{len(back)} vs {len(original)} bytes")
                check("FLAC keeps the shape",
                      (back_channels, back_width, back_rate) == (channels, width, rate))

        as_aiff = workspace / "exact.aiff"
        result = run("convert", str(exact_source), str(as_aiff))
        check("convert writes an AIFF when asked for one", result.returncode == 0, result.stderr)
        if as_aiff.exists():
            form_type, form_size, chunks = aiff_chunks(as_aiff)
            check("the AIFF is a FORM of type AIFF", form_type == "AIFF", form_type)
            check("the FORM size matches the file",
                  form_size == as_aiff.stat().st_size - 8,
                  f"{form_size} vs {as_aiff.stat().st_size - 8}")
            check("the AIFF has COMM and SSND", "COMM" in chunks and "SSND" in chunks)

            if "COMM" in chunks and "SSND" in chunks:
                comm_at = chunks["COMM"][0]
                body = as_aiff.read_bytes()
                check("COMM states the channels",
                      int.from_bytes(body[comm_at : comm_at + 2], "big") == channels)
                check("COMM states the length",
                      int.from_bytes(body[comm_at + 2 : comm_at + 6], "big") == exact_frames)
                check("COMM states the depth",
                      int.from_bytes(body[comm_at + 6 : comm_at + 8], "big") == width * 8)

                # Big-endian is where AIFF writers break, so the first sample is
                # compared against the source's own bytes reversed rather than
                # against a round trip, which would agree either way round.
                samples_at = chunks["SSND"][0] + 8
                check("AIFF samples are big-endian",
                      body[samples_at : samples_at + width] == original[0:width][::-1],
                      body[samples_at : samples_at + width].hex())

            from_aiff = workspace / "from-aiff.wav"
            result = run("convert", str(as_aiff), str(from_aiff))
            check("the AIFF opens again", result.returncode == 0, result.stderr)
            if from_aiff.exists():
                back, back_channels, back_width, back_rate = raw_frames(from_aiff)
                check("AIFF round-trips bit-identically", back == original,
                      f"{len(back)} vs {len(original)} bytes")
                check("AIFF keeps the shape",
                      (back_channels, back_width, back_rate) == (channels, width, rate))

        # An odd number of payload bytes: 24-bit mono at an odd frame count is
        # 3 * odd, which is odd, so IFF wants a pad byte after the chunk and the
        # FORM size has to count it. Getting this wrong leaves a file that only
        # some readers can parse.
        odd_source = workspace / "odd.wav"
        write_wav_codes(odd_source, [[(i * 5077) % 8388607 - 4194304 for i in range(1001)]], 3)
        odd_payload = raw_frames(odd_source)[0]
        check("the odd source really is odd", len(odd_payload) % 2 == 1, str(len(odd_payload)))

        odd_aiff = workspace / "odd.aiff"
        result = run("convert", str(odd_source), str(odd_aiff))
        check("an odd-length AIFF is written", result.returncode == 0, result.stderr)
        if odd_aiff.exists():
            _, form_size, chunks = aiff_chunks(odd_aiff)
            check("the odd AIFF ends on an even boundary",
                  odd_aiff.stat().st_size % 2 == 0, str(odd_aiff.stat().st_size))
            check("SSND's declared size excludes the pad",
                  chunks["SSND"][1] == len(odd_payload) + 8, str(chunks["SSND"][1]))
            check("the FORM size includes the pad",
                  form_size == odd_aiff.stat().st_size - 8, str(form_size))
            odd_back = workspace / "odd-back.wav"
            result = run("convert", str(odd_aiff), str(odd_back))
            check("the odd AIFF opens again", result.returncode == 0, result.stderr)
            if odd_back.exists():
                check("the odd AIFF round-trips bit-identically",
                      raw_frames(odd_back)[0] == odd_payload)

        odd_flac = workspace / "odd.flac"
        result = run("convert", str(odd_source), str(odd_flac))
        check("an odd-length FLAC is written", result.returncode == 0, result.stderr)
        if odd_flac.exists():
            check("the odd FLAC's MD5 covers the odd payload",
                  flac_stream_info(odd_flac)["md5"] == hashlib.md5(odd_payload).hexdigest())
            odd_back = workspace / "odd-back-flac.wav"
            result = run("convert", str(odd_flac), str(odd_back))
            check("the odd FLAC opens again", result.returncode == 0, result.stderr)
            if odd_back.exists():
                check("the odd FLAC round-trips bit-identically",
                      raw_frames(odd_back)[0] == odd_payload)

        # Mono and more than two channels, because the stereo path decorrelates
        # and the others do not, so they are different code.
        for count in (1, 2, 6):
            wide_source = workspace / f"wide{count}.wav"
            write_wav_codes(
                wide_source,
                [[((i * (7 + c * 13)) % 65535) - 32768 for i in range(4000)]
                 for c in range(count)],
                2,
            )
            payload = raw_frames(wide_source)[0]
            wide_flac = workspace / f"wide{count}.flac"
            wide_back = workspace / f"wide{count}-back.wav"
            ok = run("convert", str(wide_source), str(wide_flac)).returncode == 0
            ok = ok and run("convert", str(wide_flac), str(wide_back)).returncode == 0
            check(f"{count}-channel FLAC round-trips bit-identically",
                  ok and raw_frames(wide_back)[0] == payload)

            wide_aiff = workspace / f"wide{count}.aiff"
            wide_back_aiff = workspace / f"wide{count}-back-aiff.wav"
            ok = run("convert", str(wide_source), str(wide_aiff)).returncode == 0
            ok = ok and run("convert", str(wide_aiff), str(wide_back_aiff)).returncode == 0
            check(f"{count}-channel AIFF round-trips bit-identically",
                  ok and raw_frames(wide_back_aiff)[0] == payload)

        # Sample rates other than the one everything else here uses, because
        # FLAC codes the common ones in the frame header and escapes the rest.
        for other_rate in (44100, 22050, 37000, 64000):
            rate_source = workspace / f"rate{other_rate}.wav"
            write_wav_codes(rate_source, [[(i % 30000) - 15000 for i in range(2000)]], 2,
                            other_rate)
            payload = raw_frames(rate_source)[0]
            rate_flac = workspace / f"rate{other_rate}.flac"
            rate_back = workspace / f"rate{other_rate}-back.wav"
            ok = run("convert", str(rate_source), str(rate_flac)).returncode == 0
            ok = ok and run("convert", str(rate_flac), str(rate_back)).returncode == 0
            got = raw_frames(rate_back) if rate_back.exists() else (b"", 0, 0, 0)
            check(f"a FLAC at {other_rate} Hz round-trips bit-identically",
                  ok and got[0] == payload and got[3] == other_rate, str(got[3]))

        # FLAC holds integers only, so a float request has to land somewhere.
        # It lands at 24 bits rather than failing, and --dither is honoured on
        # the way there -- the same ditherer the WAV path uses, not a second
        # one that rounds instead.
        float_flac = workspace / "from-float.flac"
        result = run("convert", str(quiet_path), str(float_flac), "--format", "float")
        check("a float request to FLAC is accepted", result.returncode == 0, result.stderr)
        if float_flac.exists():
            check("and becomes 24 bits", flac_stream_info(float_flac)["bits"] == 24,
                  str(flac_stream_info(float_flac)["bits"]))

        for name, extra in (("f-none", []), ("f-tpdf", ["--dither", "tpdf"])):
            target = workspace / f"{name}.flac"
            result = run("convert", str(quiet_path), str(target), "--format", "16", *extra)
            check(f"convert to FLAC {name} exits cleanly", result.returncode == 0, result.stderr)
            decoded = workspace / f"{name}-decoded.wav"
            if target.exists():
                run("convert", str(target), str(decoded))

        if (workspace / "f-none-decoded.wav").exists() and (
            workspace / "f-tpdf-decoded.wav"
        ).exists():
            def flac_worst_ratio(path: Path) -> float:
                values, _ = read_wav(path)
                fundamental = goertzel(values, 997.0)
                worst = max(goertzel(values, 997.0 * k) for k in (3, 5, 7, 9))
                return 20.0 * math.log10(max(worst, 1e-15) / max(fundamental, 1e-15))

            plain = flac_worst_ratio(workspace / "f-none-decoded.wav")
            dithered = flac_worst_ratio(workspace / "f-tpdf-decoded.wav")
            check("dither reaches the FLAC path too",
                  plain - dithered > 6.0, f"{plain:.1f} -> {dithered:.1f} dB")

        # AIFF is integer PCM here, so a float request narrows the same way.
        float_aiff = workspace / "from-float.aiff"
        result = run("convert", str(quiet_path), str(float_aiff), "--format", "float")
        check("a float request to AIFF is accepted", result.returncode == 0, result.stderr)
        if float_aiff.exists():
            body = float_aiff.read_bytes()
            comm_at = aiff_chunks(float_aiff)[2]["COMM"][0]
            check("and becomes 24 bits",
                  int.from_bytes(body[comm_at + 6 : comm_at + 8], "big") == 24)

        # The extension steers every command that writes audio, not only
        # convert -- otherwise `normalise in.wav out.flac` writes a WAV under a
        # name that says otherwise, which is worse than refusing.
        normalised_flac = workspace / "normalised.flac"
        result = run("normalise", str(exact_source), str(normalised_flac), "--target", "EBU R128")
        check("normalise honours a .flac output", result.returncode == 0, result.stderr)
        check("and really writes a FLAC",
              normalised_flac.exists() and normalised_flac.read_bytes()[:4] == b"fLaC")

        compressed_aiff = workspace / "compressed.aiff"
        result = run("compress", str(exact_source), str(compressed_aiff))
        check("compress honours an .aiff output", result.returncode == 0, result.stderr)
        check("and really writes an AIFF",
              compressed_aiff.exists() and compressed_aiff.read_bytes()[:4] == b"FORM")

        # An unrecognised extension is still a WAV, which is what this tool did
        # before it had a choice.
        plain_output = workspace / "no-extension"
        result = run("convert", str(exact_source), str(plain_output))
        check("an unknown extension still writes a WAV",
              result.returncode == 0 and plain_output.exists()
              and plain_output.read_bytes()[:4] == b"RIFF", result.stderr)

        # Resampling into a FLAC goes through the streamed sink rather than the
        # block writer, which is different code. The samples are not the
        # source's any more, so equality is not available -- what is checked is
        # that the tone comes out where it went in, and that the signature
        # covers what the file really decodes to.
        resampled = workspace / "resampled.flac"
        result = run("convert", str(source), str(resampled), "--rate", "44100")
        check("resampling into a FLAC exits cleanly", result.returncode == 0, result.stderr)
        if resampled.exists():
            info = flac_stream_info(resampled)
            check("the resampled FLAC states the new rate", info["rate"] == 44100,
                  str(info["rate"]))
            decoded = workspace / "resampled-back.wav"
            if run("convert", str(resampled), str(decoded)).returncode == 0:
                values, got_rate = read_wav(decoded)
                middle = values[44100 : 44100 * 5]
                check("the tone survives the trip through FLAC",
                      got_rate == 44100
                      and abs(tone_amplitude(middle, 1000.0, 44100) - 0.5) < 0.01,
                      f"{tone_amplitude(middle, 1000.0, 44100):.4f}")
                check("the resampled FLAC's MD5 covers what it decodes to",
                      info["md5"] == hashlib.md5(raw_frames(decoded)[0]).hexdigest())

        # Upper case, because a file dialog on Windows hands back .FLAC as
        # readily as .flac.
        shouted = workspace / "SHOUTED.FLAC"
        result = run("convert", str(exact_source), str(shouted))
        check("the extension match ignores case",
              result.returncode == 0 and shouted.exists()
              and shouted.read_bytes()[:4] == b"fLaC", result.stderr)

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
