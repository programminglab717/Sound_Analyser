/// The one translation unit that compiles the vendored dr_libs decoders.
///
/// dr_flac and dr_mp3 are header-only C: exactly one translation unit in the
/// whole build defines DR_*_IMPLEMENTATION and carries the object code, and
/// every other file that includes them gets declarations only. This is that
/// unit, and it exists as a file of its own so that it can be a CMake target of
/// its own.
///
/// **That target deliberately does not link sa-warnings.** Everything else here
/// builds with -Wall -Wextra -Wconversion -Wold-style-cast -Werror (/W4 /WX on
/// MSVC), and third-party C does not survive it: it is full of exactly the
/// implicit narrowing and C casts those warnings exist to catch in our own
/// code. Relaxing the warning set for the whole of sa-io to accommodate two
/// vendored headers would cost us the warnings where they are useful, so the
/// vendored code is quarantined in this one file instead. See the comment in
/// src/sa-io/CMakeLists.txt.
///
/// Provenance, for whoever maintains third-party.json:
///   mackron/dr_libs @ dfe8377631000664666519fdb83da193fd8037f4
///     dr_flac.h  v0.13.4   third_party/dr_libs/dr_flac.h
///     dr_mp3.h   v0.7.4    third_party/dr_libs/dr_mp3.h
///   Both are a choice of public domain (Unlicense) or MIT-0, which is what
///   makes them usable in a closed-source build under ADR 0006. The licence
///   text is in third_party/dr_libs/LICENSE and at the foot of each header.
///
/// The build options below are set as PUBLIC compile definitions on the target
/// rather than here, because every translation unit that includes these headers
/// must see the same ones -- DR_MP3_FLOAT_OUTPUT changes the layout of the
/// drmp3 struct, so a mismatch would be a silent ODR violation.

#define DR_FLAC_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION

#include <dr_flac.h>
#include <dr_mp3.h>
