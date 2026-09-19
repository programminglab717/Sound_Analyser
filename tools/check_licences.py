#!/usr/bin/env python3
"""Fail the build on any dependency that breaks the ADR 0006 licence policy.

The policy is that every dependency must be free in perpetuity for
closed-source commercial distribution, with no revenue, seat or unit cap. A
human reviewing licences will eventually miss one under delivery pressure, so
this runs in CI instead.

Checks performed:

  1. Every declared dependency and model carries an allowlisted licence.
  2. Nothing carries a denied licence -- GPL/AGPL, non-commercial, research-only
     or unverified.
  3. LGPL dependencies are marked for dynamic linking. Static linking an LGPL
     library imposes relink obligations we cannot meet in a closed-source build.
  3a. Build tools -- programs we run to produce the product rather than link
     into it -- carry an allowlisted licence too, from a separate and narrower
     list, and each one says where its licence text is kept. The installer
     compiler is one of these: none of it is in auscultate.exe, but part of it
     is in the .msi we hand to a user, and that is still something we ship.
  4. Anything CMake fetches, finds with find_package, vendors under
     third_party/, or vcpkg installs is
     actually declared in third-party.json, so a dependency cannot arrive
     undocumented. A system facility that carries no licence of ours -- the
     platform's own threading or OpenGL -- must still be listed under
     'systemPackages' with a reason, so every exemption is written down.

Usage:  python3 tools/check_licences.py [--manifest third-party.json] [--root .]
Exit:   0 clean, 1 on any violation.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


class Violations:
    def __init__(self) -> None:
        self.errors: list[str] = []
        self.warnings: list[str] = []

    def error(self, message: str) -> None:
        self.errors.append(message)

    def warn(self, message: str) -> None:
        self.warnings.append(message)


def load_manifest(path: Path) -> dict:
    if not path.is_file():
        raise SystemExit(f"licence gate: manifest not found: {path}")
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise SystemExit(f"licence gate: {path} is not valid JSON: {exc}") from exc


def check_entry(entry: dict, kind: str, policy: dict, found: Violations) -> None:
    name = entry.get("name")
    if not name:
        found.error(f"{kind} entry with no 'name': {entry!r}")
        return

    licence = entry.get("licence")
    if not licence:
        found.error(f"{kind} '{name}' declares no licence")
        return

    # Denied wins over allowed, so a licence listed in both is still rejected.
    if licence in policy.get("deniedLicences", []):
        found.error(
            f"{kind} '{name}' uses DENIED licence '{licence}'. "
            f"It cannot ship in a closed-source product under ADR 0006."
        )
        return

    if licence not in policy.get("allowedLicences", []):
        found.error(
            f"{kind} '{name}' uses unrecognised licence '{licence}'. "
            f"Add it to policy.allowedLicences only after a written determination."
        )
        return

    if licence in policy.get("dynamicLinkRequired", []):
        linkage = entry.get("linkage", "").lower()
        if linkage != "dynamic":
            found.error(
                f"{kind} '{name}' is {licence} but declares linkage "
                f"'{entry.get('linkage', '<missing>')}'. LGPL requires dynamic "
                f"linking so users can replace the library."
            )

    if kind == "model" and not entry.get("weightsLicenceVerified"):
        found.error(
            f"model '{name}' has no written weights-licence determination. "
            f"A model's code licence does not cover its weights."
        )


def check_build_tool(entry: dict, policy: dict, root: Path, found: Violations) -> None:
    """A tool we run, not a library we link -- but still one we have to be free of debt to.

    The allowlist is deliberately a different one. A licence can be perfectly
    safe for a program we merely execute and quite wrong for a library we link
    into a closed-source product, and collapsing the two lists into one would
    lose exactly that distinction. Reciprocal licences live here and nowhere
    else.

    The denied list still applies in full: a build tool whose own code or data
    ends up inside something we hand to a user is shipping, whatever we call it.
    """
    name = entry.get("name")
    if not name:
        found.error(f"build tool entry with no 'name': {entry!r}")
        return

    licence = entry.get("licence")
    if not licence:
        found.error(f"build tool '{name}' declares no licence")
        return

    if licence in policy.get("deniedLicences", []):
        found.error(
            f"build tool '{name}' uses DENIED licence '{licence}'. "
            f"It cannot be part of a closed-source product's toolchain under ADR 0006."
        )
        return

    if licence not in policy.get("allowedBuildToolLicences", []):
        found.error(
            f"build tool '{name}' uses unrecognised licence '{licence}'. "
            f"Add it to policy.allowedBuildToolLicences only after a written "
            f"determination, and never by moving it from the dependency list."
        )
        return

    if not entry.get("notes"):
        found.error(
            f"build tool '{name}' has no 'notes' saying what of it, if anything, "
            f"ends up in what we ship."
        )

    licence_text = entry.get("licenceTextPath")
    if not licence_text:
        found.error(
            f"build tool '{name}' does not say where its licence text is kept "
            f"('licenceTextPath'). Retaining the notice is a condition of most of "
            f"the licences on this list."
        )
    elif not (root / licence_text).is_file():
        found.error(
            f"build tool '{name}' points at licence text '{licence_text}', which "
            f"does not exist."
        )


def declared_names(manifest: dict) -> set[str]:
    """Every name a declared dependency can appear under in a build file.

    A package is rarely called the same thing twice: the Qt project ships as
    'qtbase' and is found as 'Qt6'; ALSA is 'alsa-lib' upstream and 'ALSA' to
    CMake. Without 'cmakeNames' the gate would either miss those or have to
    match loosely, and a gate that matches loosely is one that lets the next
    one through.
    """
    names: set[str] = set()
    for key in ("dependencies", "models"):
        for entry in manifest.get(key, []):
            name = entry.get("name")
            if name:
                names.add(name.lower())
            for alias in entry.get("cmakeNames", []):
                names.add(str(alias).lower())
    return names


def scan_cmake_fetches(root: Path, declared: set[str], found: Violations) -> None:
    pattern = re.compile(r"FetchContent_Declare\s*\(\s*([A-Za-z0-9_\-]+)", re.IGNORECASE)
    for cmake in root.rglob("CMakeLists.txt"):
        if "build" in cmake.parts or "_deps" in cmake.parts:
            continue
        for match in pattern.finditer(cmake.read_text(encoding="utf-8", errors="replace")):
            name = match.group(1)
            if name.lower() not in declared:
                found.error(
                    f"{cmake.relative_to(root)} fetches '{name}' but it is not "
                    f"declared in the third-party manifest."
                )


def scan_cmake_find_package(
    root: Path, declared: set[str], allowed_system: dict[str, str], found: Violations
) -> None:
    """Every find_package() must name a declared dependency or an allowed system package.

    This is the hole the gate had until ALSA fell through it: fetched and vcpkg
    dependencies were cross-checked, but a library found on the system was not,
    so linking one imposed its licence on the product with nothing to notice.
    Qt only passed because it was declared by hand.

    A system package -- the C++ threading runtime, the platform's own OpenGL --
    is not a third-party dependency and has no licence of ours to carry, but it
    still has to be listed with a reason, so that every exemption is visible
    rather than assumed.
    """
    pattern = re.compile(r"find_package\s*\(\s*([A-Za-z0-9_\-]+)", re.IGNORECASE)
    for cmake in root.rglob("CMakeLists.txt"):
        if "build" in cmake.parts or "_deps" in cmake.parts:
            continue
        for match in pattern.finditer(cmake.read_text(encoding="utf-8", errors="replace")):
            name = match.group(1)
            lowered = name.lower()
            if lowered in declared or lowered in allowed_system:
                continue
            found.error(
                f"{cmake.relative_to(root)} links '{name}' via find_package but it is "
                f"neither declared in the third-party manifest nor listed under "
                f"'systemPackages'."
            )


def scan_vendored(root: Path, manifest: dict, found: Violations) -> None:
    """Every directory under third_party/ must be claimed by a declared entry.

    Vendored source is the third way a dependency arrives, after fetching and
    finding, and it is the one that leaves no trace in a build file at all --
    someone copies a header in and the gate has nothing to scan. Requiring each
    directory to be named by a 'vendoredPath' means copying a library in without
    declaring it fails the build.
    """
    vendored = root / "third_party"
    if not vendored.is_dir():
        return

    claimed = {
        str(entry["vendoredPath"]).rstrip("/")
        for key in ("dependencies", "models")
        for entry in manifest.get(key, [])
        if entry.get("vendoredPath")
    }

    for child in sorted(vendored.iterdir()):
        if not child.is_dir():
            continue
        relative = child.relative_to(root).as_posix()
        if relative not in claimed:
            found.error(
                f"{relative} contains vendored source but no manifest entry claims it "
                f"with 'vendoredPath'."
            )


def scan_vcpkg(root: Path, declared: set[str], found: Violations) -> None:
    manifest_path = root / "vcpkg.json"
    if not manifest_path.is_file():
        return
    try:
        data = json.loads(manifest_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        found.error(f"vcpkg.json is not valid JSON: {exc}")
        return

    for dependency in data.get("dependencies", []):
        name = dependency if isinstance(dependency, str) else dependency.get("name", "")
        if name and name.lower() not in declared:
            found.error(
                f"vcpkg.json requires '{name}' but it is not declared in the "
                f"third-party manifest."
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", default="third-party.json")
    parser.add_argument("--root", default=".")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    manifest = load_manifest(root / args.manifest)
    policy = manifest.get("policy", {})
    if not policy.get("allowedLicences"):
        raise SystemExit("licence gate: manifest has no policy.allowedLicences")

    found = Violations()

    dependencies = manifest.get("dependencies", [])
    models = manifest.get("models", [])
    build_tools = manifest.get("buildTools", [])
    for entry in dependencies:
        check_entry(entry, "dependency", policy, found)
    for entry in models:
        check_entry(entry, "model", policy, found)
    for entry in build_tools:
        check_build_tool(entry, policy, root, found)

    declared = declared_names(manifest)
    allowed_system = {
        str(name).lower(): reason
        for name, reason in (manifest.get("systemPackages") or {}).items()
    }
    scan_cmake_fetches(root, declared, found)
    scan_cmake_find_package(root, declared, allowed_system, found)
    scan_vendored(root, manifest, found)
    scan_vcpkg(root, declared, found)

    print(
        f"licence gate: {len(dependencies)} dependencies, {len(models)} models, "
        f"{len(build_tools)} build tools checked"
    )

    for warning in found.warnings:
        print(f"  warning: {warning}")

    if found.errors:
        print(f"\nlicence gate FAILED with {len(found.errors)} violation(s):\n")
        for error in found.errors:
            print(f"  ERROR: {error}")
        print("\nSee docs/05-licensing-and-dependencies.md and docs/adr/0006.")
        return 1

    print("licence gate: OK -- every dependency is free in perpetuity for "
          "closed-source distribution")
    return 0


if __name__ == "__main__":
    sys.exit(main())
