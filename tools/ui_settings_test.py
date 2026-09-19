#!/usr/bin/env python3
"""End-to-end check that the window really remembers itself between runs.

The windowless tests in src/sa-ui/tests decide what a stored value *means*: what
a corrupt one falls back to, and where a window saved on a monitor that is no
longer attached should be put instead. They cannot say whether any of it reaches
a real window, because they have no window.

This drives the real binary, twice at a time, with a settings file of its own,
and reads back what the second run says it restored. A build that parsed the
file perfectly and then applied none of it would pass every check in that suite
and fail here.

No third-party imports, for the same reason as the other drivers in this folder.
"""

from __future__ import annotations

import argparse
import math
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

SAMPLE_RATE = 48000


def write_probe_wav(path: Path, hz: float = 440.0, seconds: float = 1.0) -> None:
    """A short tone. Nothing here is about the audio; it is about opening one."""
    samples = bytearray()
    for i in range(int(SAMPLE_RATE * seconds)):
        value = 0.4 * math.sin(2.0 * math.pi * hz * i / SAMPLE_RATE)
        samples += struct.pack("<h", int(value * 32767))

    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(SAMPLE_RATE)
        handle.writeframes(bytes(samples))


def run(binary: Path, settings: Path | None, *arguments: str) -> dict[str, str]:
    """One run of the application, reporting the settings it ended up with.

    Returns the key=value lines --print-settings wrote, as a dictionary. A
    non-zero exit is reported as an empty result rather than an exception, so a
    single failing case does not stop the rest.
    """
    command = [str(binary)]
    if settings is not None:
        command += ["--settings", str(settings)]
    command += [*arguments, "--print-settings"]

    completed = subprocess.run(command, capture_output=True, text=True, timeout=300)
    if completed.returncode != 0:
        print(f"FAIL: {' '.join(command)} exited {completed.returncode} -- {completed.stderr}")
        return {}

    values: dict[str, str] = {}
    for line in completed.stdout.splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            values[key] = value
    return values


def rectangle(text: str) -> tuple[int, int, int, int]:
    x, y, width, height = (int(part) for part in text.split(","))
    return x, y, width, height


def overlap(a: tuple[int, int, int, int], b: tuple[int, int, int, int]) -> tuple[int, int]:
    """Width and height of the part of `a` that is inside `b`."""
    left = max(a[0], b[0])
    top = max(a[1], b[1])
    right = min(a[0] + a[2], b[0] + b[2])
    bottom = min(a[1] + a[3], b[1] + b[3])
    return max(0, right - left), max(0, bottom - top)


def write_settings(path: Path, sections: dict[str, dict[str, str]]) -> None:
    """Write an INI by hand.

    Deliberately not through the application: the point of choosing a text
    format over the registry is that a file can be written, read and corrected
    by something other than the program that wrote it, and a test that could
    only produce a settings file by asking the application for one would not be
    testing that.
    """
    lines: list[str] = []
    for section, entries in sections.items():
        lines.append(f"[{section}]")
        for key, value in entries.items():
            lines.append(f"{key}={value}")
        lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="path to the auscultate executable")
    arguments = parser.parse_args()

    failures: list[str] = []

    def check(what: str, ok: bool, detail: str = "") -> None:
        if not ok:
            failures.append(f"{what}{': ' + detail if detail else ''}")

    with tempfile.TemporaryDirectory() as directory:
        workspace = Path(directory)
        first = workspace / "first.wav"
        second = workspace / "second.wav"
        write_probe_wav(first, 440.0)
        write_probe_wav(second, 880.0)

        # -- a first run, with nothing stored ------------------------------
        settings = workspace / "settings.ini"
        fresh = run(arguments.binary, settings, str(first))
        check("a first run starts", bool(fresh))
        if not fresh:
            print("FAIL: the first run produced nothing; the rest cannot be judged")
            return 1

        check(
            "a first run restores no window",
            fresh.get("window_restored") == "0",
            f"window_restored={fresh.get('window_restored')}",
        )
        check("a first run writes its settings", settings.is_file())
        check(
            "the file just opened is the first recent entry",
            fresh.get("recent1", "").endswith("first.wav"),
            fresh.get("recent1", "<none>"),
        )

        screens = int(fresh.get("screen_count", "0"))
        check("the window knows what screens there are", screens >= 1, f"screen_count={screens}")
        screen = rectangle(fresh.get("screen0", "0,0,0,0"))

        # -- a second run picks up where the first left off ----------------
        again = run(arguments.binary, settings, str(second))
        check(
            "a second run restores the window",
            again.get("window_restored") == "1",
            f"window_restored={again.get('window_restored')}",
        )
        check(
            "the second file opened goes to the top of the list",
            again.get("recent1", "").endswith("second.wav")
            and again.get("recent2", "").endswith("first.wav"),
            f"{again.get('recent1')} then {again.get('recent2')}",
        )

        # Opening the first file again moves it up rather than listing it twice.
        third = run(arguments.binary, settings, str(first))
        check(
            "opening a file again moves it up",
            third.get("recent1", "").endswith("first.wav") and third.get("recent_count") == "2",
            f"{third.get('recent1')}, count {third.get('recent_count')}",
        )

        # -- preferences reach the widgets that draw with them --------------
        stored = workspace / "preferences.ini"
        write_settings(
            stored,
            {
                "General": {"version": "1"},
                "preferences": {
                    "colourmap": "viridis",
                    "frequencyscale": "linear",
                    "spectrogramfloordb": "-60",
                    "exportformat": "16",
                    "dither": "shaped",
                    "fadeshape": "scurve",
                    "loudnesstarget": "spotify",
                    "fftsize": "2048",
                    "hopsize": "512",
                    "analysisseconds": "45",
                    "keyseconds": "20",
                    "beatgrid": "false",
                    "octavebands": "true",
                },
            },
        )
        restored = run(arguments.binary, stored, str(first))
        wanted = {
            "pref_colourmap": "viridis",
            "pref_frequencyscale": "linear",
            "pref_floordb": "-60.0",
            "pref_exportformat": "16",
            "pref_dither": "shaped",
            "pref_fadeshape": "scurve",
            "pref_loudnesstarget": "spotify",
            "pref_fftsize": "2048",
            "pref_hopsize": "512",
            "pref_analysisseconds": "45",
            "pref_keyseconds": "20",
            "pref_beatgrid": "0",
            "pref_octavebands": "1",
        }
        for key, value in wanted.items():
            check(
                f"{key} is restored",
                restored.get(key) == value,
                f"wanted {value}, got {restored.get(key)}",
            )

        # -- a window saved where nobody can see it -------------------------
        lost = workspace / "lost.ini"
        write_settings(
            lost,
            {
                "General": {"version": "1"},
                # Where a window saved on the second monitor of a docked laptop
                # would be, reopened on the train with only the laptop panel.
                "window": {
                    "x": str(screen[0] + screen[2] + 2000),
                    "y": str(screen[1] + 400),
                    "width": "900",
                    "height": "600",
                },
            },
        )
        rescued = run(arguments.binary, lost, str(first))
        check(
            "a window saved off every screen is still restored",
            rescued.get("window_restored") == "1",
        )
        placed = (
            int(rescued.get("window_x", "0")),
            int(rescued.get("window_y", "0")),
            int(rescued.get("window_width", "0")),
            int(rescued.get("window_height", "0")),
        )
        shown = overlap(placed, screen)
        # The same rule the windowless tests hold confineToScreens to: enough of
        # the window on a real screen to see it and to grab it.
        check(
            "the rescued window is on a screen",
            shown[0] >= 160 and shown[1] >= 48,
            f"{shown[0]}x{shown[1]} of it visible on {screen}",
        )
        check(
            "the rescued window's title bar is not above the screen",
            placed[1] >= screen[1],
            f"y={placed[1]}, screen top {screen[1]}",
        )

        # The other way a window becomes unreachable, and the one that is
        # specific to Windows: most of it is on screen, and the title bar --
        # the only part of it that can be dragged -- is above the top edge.
        high = workspace / "high.ini"
        write_settings(
            high,
            {
                "General": {"version": "1"},
                "window": {
                    "x": str(screen[0] + 100),
                    "y": str(screen[1] - 400),
                    "width": "700",
                    "height": "560",
                },
            },
        )
        lowered = run(arguments.binary, high, str(first))
        check(
            "a window whose title bar is off the top is brought down",
            int(lowered.get("window_y", "-1")) >= screen[1],
            f"y={lowered.get('window_y')}, screen top {screen[1]}",
        )

        # A window that is where it was left is left where it was.
        kept = workspace / "kept.ini"
        wanted_frame = (screen[0] + 60, screen[1] + 50, 720, 520)
        write_settings(
            kept,
            {
                "General": {"version": "1"},
                "window": {
                    "x": str(wanted_frame[0]),
                    "y": str(wanted_frame[1]),
                    "width": str(wanted_frame[2]),
                    "height": str(wanted_frame[3]),
                },
            },
        )
        unmoved = run(arguments.binary, kept, str(first))
        check(
            "a window that still fits keeps its position",
            (int(unmoved.get("window_x", "-1")), int(unmoved.get("window_y", "-1")))
            == wanted_frame[:2],
            f"{unmoved.get('window_x')},{unmoved.get('window_y')} "
            f"wanted {wanted_frame[0]},{wanted_frame[1]}",
        )

        # -- the splitters -------------------------------------------------
        panes = workspace / "panes.ini"
        write_settings(
            panes,
            {
                "General": {"version": "1"},
                "window": {
                    "x": str(screen[0] + 20),
                    "y": str(screen[1] + 20),
                    "width": "760",
                    "height": "700",
                    # The waveform given most of the height, which is the
                    # opposite of the layout the window is built with.
                    "mainsplit": '"600,100"',
                    # And the spectrum given more than the analysis panel,
                    # which is also the opposite of how it starts.
                    "sidesplit": '"100,100,600"',
                },
            },
        )
        dragged = run(arguments.binary, panes, str(first))
        main_sizes = [int(part) for part in dragged.get("split_main", "0,0").split(",")]
        side_sizes = [int(part) for part in dragged.get("split_side", "0,0,0").split(",")]
        check(
            "a saved main split is restored, not the built-in one",
            len(main_sizes) == 2 and main_sizes[0] > main_sizes[1],
            f"split_main={dragged.get('split_main')}",
        )
        # Not an exact comparison: a splitter fits its sizes to the height it
        # actually has and to each pane's minimum, so the numbers that come
        # back are the stored proportions and not the stored pixels.
        check(
            "a saved side split is restored, not the built-in one",
            len(side_sizes) == 3 and side_sizes[2] > side_sizes[1],
            f"split_side={dragged.get('split_side')}",
        )

        # -- a file that is not a settings file at all ----------------------
        junk = workspace / "junk.ini"
        junk.write_bytes(b"\x00\x01\x02 not an ini [[[ \n====\nwindow/x=\xff\xfe\n")
        survived = run(arguments.binary, junk, str(first))
        check("a corrupt settings file does not stop the application", bool(survived))
        check(
            "a corrupt settings file leaves the defaults in place",
            survived.get("pref_colourmap") == "magma"
            and survived.get("pref_fftsize") == "4096"
            and survived.get("window_restored") == "0",
            f"colourmap={survived.get('pref_colourmap')}, fft={survived.get('pref_fftsize')}, "
            f"restored={survived.get('window_restored')}",
        )

        # -- a file from a build that knows more than this one --------------
        future = workspace / "future.ini"
        write_settings(
            future,
            {
                "General": {"version": "99"},
                "preferences": {"colourmap": "grey"},
                "window": {
                    "x": str(screen[0] + 30),
                    "y": str(screen[1] + 30),
                    "width": "820",
                    "height": "640",
                },
            },
        )
        before = future.read_bytes()
        ahead = run(arguments.binary, future, str(first))
        check(
            "a file from a newer build is still read",
            ahead.get("pref_colourmap") == "grey" and ahead.get("window_restored") == "1",
            f"colourmap={ahead.get('pref_colourmap')}, restored={ahead.get('window_restored')}",
        )
        check(
            "a file from a newer build is not written over",
            future.read_bytes() == before,
            "the file changed",
        )
        check("and the window says so", ahead.get("settings_writeback") == "0")

        # -- portable: a settings file already beside the executable --------
        portable = workspace / "portable"
        portable.mkdir()
        copied = portable / arguments.binary.name
        shutil.copy2(arguments.binary, copied)
        beside = portable / "Auscultate.ini"
        beside.touch()

        carried = run(copied, None, str(first))
        check(
            "an ini beside the executable is used instead of the per-user one",
            carried.get("settings_portable") == "1"
            and carried.get("settings_file", "") == str(beside),
            f"portable={carried.get('settings_portable')}, file={carried.get('settings_file')}",
        )
        check(
            "and it is written to",
            beside.stat().st_size > 0,
            "the file beside the executable is still empty",
        )

    for failure in failures:
        print(f"FAIL: {failure}")
    if failures:
        print(f"\n{len(failures)} check(s) failed.")
        return 1

    print("OK: settings, window geometry, splitters and the recent list all survive a restart")
    return 0


if __name__ == "__main__":
    sys.exit(main())
