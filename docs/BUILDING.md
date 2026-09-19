# Building

## Requirements

| | Windows (target) | Linux (engine development only) |
| --- | --- | --- |
| Compiler | MSVC 2022 (v143) | GCC 13+ or Clang 16+ |
| CMake | 3.24+ | 3.24+ |
| Generator | Visual Studio 17 2022 | Ninja |
| Python | 3.9+ (licence gate) | 3.9+ |

Windows is the shipping target. The engine modules (`sa-core` and below) carry
no UI or platform dependency, so they build and test on Linux too -- which keeps
the portability honest and makes CI cheaper.

## Build and test

```sh
# Windows
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug

# Linux
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

## Presets

| Preset | Purpose |
| --- | --- |
| `debug` / `windows-debug` | Development. **RT safety checks on.** |
| `release` / `windows-release` | RelWithDebInfo. RT checks off. |
| `asan` | AddressSanitizer + UBSan. Linux/Clang. |

## Options

| Option | Default | Effect |
| --- | --- | --- |
| `SA_BUILD_TESTS` | ON | Build the test suite (fetches Catch2) |
| `SA_RT_SAFETY_CHECKS` | ON | Instrument global `operator new` to trap audio-thread allocation |
| `SA_WARNINGS_AS_ERRORS` | ON | `-Werror` / `/WX` |

### About `SA_RT_SAFETY_CHECKS`

The audio thread must never allocate. With this on, `sa-core` replaces the
global `operator new`/`delete` and counts every allocation made while a thread
is inside a `rt::ScopedAudioThread`. Tests assert the count is zero:

```cpp
const rt::ScopedAudioThread guard;
const rt::AllocationScope scope;
processor.process(view);
CHECK(scope.count() == 0);
```

Keep it on in development. Turn it off for release and for profiling builds,
where the instrumented allocator would distort timings.

## Licence gate

```sh
python3 tools/check_licences.py
```

Validates `third-party.json` against the ADR 0006 policy: free in perpetuity,
closed-source permitted, no revenue cap. It rejects GPL/AGPL, non-commercial and
research-only terms, requires LGPL dependencies to be dynamically linked,
requires every ML model to carry a written weights-licence determination, and
fails if CMake or vcpkg pulls in anything the manifest does not declare.

**Adding a dependency means adding it to `third-party.json` first.** CI runs this
before anything else.

## The Windows installer

CI builds an `.msi` alongside the portable zip. Both come out of the same staged
folder, so they always contain the same binaries.

| Piece | What it is |
| --- | --- |
| `packaging/windows/auscultate.wxs` | Everything that is a decision: install location, shortcut, upgrade rules, licence page |
| `packaging/windows/version.rc.in` | The version resource both executables carry. Configured by `sa_add_version_resource()` in the top-level `CMakeLists.txt` |
| `tools/make_installer_wxs.py` | Walks the staged folder and writes the file list as a second WiX source |
| `tools/make_eula_rtf.py` | Turns `docs/EULA.md` into the RTF the licence page shows |
| `tools/check_installer.py` | Cross-checks all of the above, on any platform, in a second |

### Why WiX, and why an MSI

WiX Toolset 3.14 is pre-installed on the GitHub Actions `windows-latest` image
with its `bin` directory on `PATH`. NSIS is not: it was last on the Windows
Server 2022 image and did not follow to Server 2025. Both are free forever with
no cap, so the licence rule does not choose between them; the runner does.

An MSI also means Windows Installer — not a script we wrote — decides how to
take the product away again. The uninstaller is the part of an installer that is
hardest to get right and least likely to be tested, so the option where we write
the least of it wins.

WiX is MS-RL, and it is declared under `buildTools` in `third-party.json`. See
that entry for what of it ends up in the `.msi` and what that obliges.

### Building it by hand on Windows

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
# stage as the CI job does: copy both exes into package\Auscultate, run
# windeployqt against auscultate.exe, copy docs\EULA.md, docs\PRIVACY.md and
# packaging\windows\licences in beside them
python tools/make_installer_wxs.py --stage package/Auscultate `
    --out installer/files.wxs --version-out installer/version.txt
python tools/make_eula_rtf.py docs/EULA.md installer/EULA.rtf
candle -arch x64 -dProductVersion=0.1.0 `
       -dLicenceRtf=(Resolve-Path installer/EULA.rtf).Path `
       -ext WixUIExtension -out installer\obj\ `
       packaging\windows\auscultate.wxs installer\files.wxs
light -ext WixUIExtension -out installer\auscultate-0.1.0-x64.msi `
      installer\obj\auscultate.wixobj installer\obj\files.wixobj
```

`light` runs the Windows Installer validation suite (the ICE checks) as part of
that last step. **If it reports something, fix the authoring.** Do not reach for
`-sval` or `-sice:`; the two rules this package leans on hardest, ICE38 and
ICE64, are the same two that keep the uninstall complete and keep a replaced Qt
DLL from triggering a repair.

### It ships unsigned

There is no signing step and there is not going to be one until a certificate is
affordable, which under the no-purchases rule means not at all. Do not add one,
and do not make the build fail when no certificate is configured.
`docs/INSTALLING.md` tells users plainly what they will see instead.

## Module layout

```
src/
  sa-core/   buffers, channel layouts, time types, errors, RT safety
  sa-io/     peak pyramid  (codecs and streaming reader land next)
```

Dependencies point downward only. `sa-core` and `sa-io` must never include a UI
header -- that rule is what keeps the UI toolkit a replaceable decision rather
than a rewrite (ADR 0006).
