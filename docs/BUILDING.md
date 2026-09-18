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

## Module layout

```
src/
  sa-core/   buffers, channel layouts, time types, errors, RT safety
  sa-io/     peak pyramid  (codecs and streaming reader land next)
```

Dependencies point downward only. `sa-core` and `sa-io` must never include a UI
header -- that rule is what keeps the UI toolkit a replaceable decision rather
than a rewrite (ADR 0006).
