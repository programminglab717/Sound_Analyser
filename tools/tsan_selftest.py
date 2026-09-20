#!/usr/bin/env python3
"""Check that ThreadSanitizer is awake in a build that claims to use it.

A clean sanitiser run means something only if the sanitiser would have said so
had there been anything to say. Nothing about a green ThreadSanitizer job
distinguishes "no races" from "not actually instrumented": a preset whose flags
stopped reaching the compiler, a toolchain that quietly dropped the runtime, or
an environment variable that turned reporting off all produce exactly the same
silence.

So this compiles a program that is a data race and nothing else, with the flags
taken out of the build directory itself rather than retyped here, and requires
that the sanitiser reports it. Then it compiles the same program without the
sanitiser and requires that it does *not* -- which is what makes the first
result evidence about the sanitiser rather than about the program.

Where Qt is installed it also records the other side of the same coin: a
program that hands one integer from a worker thread to the main thread through
a Qt queued connection, which is correct and is the supported way to do it, is
reported as a data race, because the ordering lives in a library nothing
instrumented. That is why the sanitiser presets are built without Qt, and this
is the evidence for it rather than the assertion of it.

Usage:  python3 tools/tsan_selftest.py [build directory]
Exit:   0 when the race was reported and the uninstrumented build was silent.
"""

from __future__ import annotations

import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

# Two threads writing one int for long enough that they cannot fail to overlap.
#
# This began as a single write from each of two threads, which is the smallest
# program that is a data race and reads better. It was replaced because it went
# unreported once on a CI runner while passing eight times out of eight here,
# and a check whose whole job is to be trusted cannot be the flaky one. The
# original raced in a window a few instructions wide -- between spawning the
# thread and the parent's own write -- and on a loaded runner the child can be
# scheduled wholly outside it.
#
# The loop removes the timing question rather than answering it: with a hundred
# thousand writes each, there is no schedule on which the two threads do not
# collide. Both writers are spawned, so neither is the parent racing a child it
# has just created, and the widened window is the only thing that changed --
# it is still two unsynchronised writes to one int.
RACY_PROGRAM = """
#include <thread>

int shared = 0;

static void hammer(int value) {
    for (int i = 0; i < 100000; ++i) {
        shared = value;
    }
}

int main() {
    std::thread first{hammer, 1};
    std::thread second{hammer, 2};
    first.join();
    second.join();
    return shared == 0 ? 1 : 0;
}
"""

# One integer handed from a worker thread to the main thread through a Qt
# queued connection, which is the supported way to do exactly that, and
# nothing else shared at all. ThreadSanitizer reports it, because the ordering
# is a QMutex inside libQt6Core: futex-based, in a library built without
# instrumentation, so the sanitiser cannot see it.
#
# That is the whole reason the sanitiser presets are built without Qt, and this
# is the evidence for it rather than the assertion of it.
QT_QUEUE_PROGRAM = """
#include <QCoreApplication>
#include <QObject>
#include <chrono>
#include <cstdio>
#include <thread>

int main(int argc, char** argv) {
    QCoreApplication app{argc, argv};
    QObject receiver;

    int handed = 0;
    int seen = -1;
    bool done = false;

    std::thread worker{[&receiver, &handed, &seen, &done] {
        handed = 42;
        QMetaObject::invokeMethod(
            &receiver,
            [&handed, &seen, &done] {
                seen = handed;
                done = true;
            },
            Qt::QueuedConnection);
    }};

    // A plain sleep, so that the main thread learns nothing from the worker
    // except through the event queue. An atomic flag here would be a
    // happens-before edge of its own and would answer a different question.
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    while (!done) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    worker.join();

    std::printf("seen = %d\\n", seen);
    return seen == 42 ? 0 : 1;
}
"""


def compiler_of(build: Path) -> str:
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    found = re.search(r"^CMAKE_CXX_COMPILER:\w+=(.+)$", cache, re.M)
    if not found:
        raise SystemExit(f"{build}/CMakeCache.txt does not say which compiler it used")
    return found.group(1).strip()


def flags_of(build: Path) -> list[str]:
    """The flags this build compiles its own sources with.

    Read rather than restated, because a self-test run with flags of its own
    would answer a question about those flags and not about the build.
    """
    ninja = build / "build.ninja"
    if ninja.exists():
        for line in ninja.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.strip().startswith("FLAGS = "):
                return shlex.split(line.split("FLAGS = ", 1)[1])
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    found = re.search(r"^CMAKE_CXX_FLAGS:\w+=(.*)$", cache, re.M)
    if not found:
        raise SystemExit(f"could not read the compile flags out of {build}")
    return shlex.split(found.group(1))


def sanitised(flags: list[str]) -> bool:
    return any(flag.startswith("-fsanitize=") and "thread" in flag for flag in flags)


def build_and_run(
    compiler: str,
    flags: list[str],
    source: Path,
    binary: Path,
    extra_environment: dict[str, str] | None = None,
) -> str:
    compiled = subprocess.run(
        [compiler, *flags, "-pthread", str(source), "-o", str(binary)],
        capture_output=True,
        text=True,
    )
    if compiled.returncode != 0:
        raise SystemExit(f"could not compile the probe: {compiled.stderr}")

    environment = dict(os.environ)
    # halt_on_error would stop the process at the report, which is fine, but the
    # exit code is then the only evidence. The report itself is what is wanted.
    environment["TSAN_OPTIONS"] = "halt_on_error=0"
    environment.update(extra_environment or {})
    ran = subprocess.run([str(binary)], capture_output=True, text=True, env=environment, timeout=300)
    return ran.stdout + ran.stderr


def qt_flags() -> list[str] | None:
    """What it takes to compile against Qt6Core here, or nothing if it is absent.

    The sanitiser presets are built without Qt on purpose, so on the machine
    that matters most this returns nothing and the Qt half of the check is
    skipped rather than failed.
    """
    try:
        found = subprocess.run(
            ["pkg-config", "--cflags", "--libs", "Qt6Core"],
            capture_output=True,
            text=True,
            timeout=60,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if found.returncode != 0:
        return None
    return shlex.split(found.stdout)


def main() -> int:
    build = Path(sys.argv[1] if len(sys.argv) > 1 else "build/tsan")
    if not (build / "CMakeCache.txt").exists():
        print(f"FAIL: {build} is not a configured build directory")
        return 1

    compiler = compiler_of(build)
    flags = flags_of(build)
    if not sanitised(flags):
        print(f"FAIL: {build} compiles with {' '.join(flags)}, which is not ThreadSanitizer")
        return 1

    with tempfile.TemporaryDirectory() as directory:
        workspace = Path(directory)
        source = workspace / "race.cpp"
        source.write_text(RACY_PROGRAM, encoding="utf-8")

        reported = build_and_run(compiler, flags, source, workspace / "race-tsan")
        if "WARNING: ThreadSanitizer: data race" not in reported:
            detail = reported.strip() or "(nothing at all -- it ran and said not one word)"
            print(
                "FAIL: a program that is nothing but a data race, compiled with this build's own "
                "flags, was not reported. The sanitiser is not awake, so a clean run of the "
                "suite says nothing.\n"
                f"compiler: {compiler}\n"
                f"flags:    {' '.join(flags)}\n"
                f"--- what the probe printed ---\n{detail}"
            )
            return 1

        # And the same program without the sanitiser, so that the report above
        # is known to have come from the sanitiser rather than from anything the
        # program does on its own.
        plain = [flag for flag in flags if not flag.startswith("-fsanitize=")]
        quiet = build_and_run(compiler, plain, source, workspace / "race-plain")
        if "ThreadSanitizer" in quiet:
            print(f"FAIL: the uninstrumented build reported a race too:\n{quiet}")
            return 1

        qt = qt_flags()
        if qt is None:
            print(
                f"OK: {compiler} with {' '.join(flags)} reports a deliberate data race, and the "
                f"same program built without the sanitiser does not. A clean run of this build "
                f"means something. (Qt6Core is not installed here, so the second half of this "
                f"check -- why these builds leave Qt out -- was not run.)"
            )
            return 0

        # The other half: why a sanitiser build leaves Qt out. One integer
        # across a queued connection is reported, because the ordering is a
        # QMutex inside a library nothing instrumented. QT_NO_GLIB keeps the
        # glib event dispatcher out of it -- its wake-up pipe is something the
        # sanitiser *can* follow, and it masks this intermittently, which is
        # exactly how a run comes back clean without meaning anything.
        qt_source = workspace / "qtqueue.cpp"
        qt_source.write_text(QT_QUEUE_PROGRAM, encoding="utf-8")
        handed = build_and_run(
            compiler, [*flags, *qt], qt_source, workspace / "qtqueue", {"QT_NO_GLIB": "1"}
        )
        if "WARNING: ThreadSanitizer: data race" not in handed:
            print(
                "NOTE: handing one integer across a Qt queued connection was NOT reported here, "
                "so this toolchain can see more of Qt than the one this was written against. "
                "That is good news, and it means the reason recorded in src/sa-ui/CMakeLists.txt "
                "for keeping the widget tests out of sanitiser builds should be re-checked.\n"
                f"--- what it printed ---\n{handed}"
            )

    print(
        f"OK: {compiler} with {' '.join(flags)} reports a deliberate data race, and the same "
        f"program built without the sanitiser does not. A clean run of this build means "
        f"something. Handing one integer across a Qt queued connection is reported too, which "
        f"is why these builds leave Qt out."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
