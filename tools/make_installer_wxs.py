#!/usr/bin/env python3
"""Turn a staged application folder into the WiX fragment that installs it.

packaging/windows/auscultate.wxs holds everything that is a decision -- the
install location, the shortcut, the upgrade rules, the licence page. It does
not hold the file list, because the file list is not a decision: it is whatever
windeployqt put next to auscultate.exe, and that changes with the Qt version.
A hand-written list would go stale as a missing Qt plugin, which every test we
have would pass and a user's machine would not.

So this walks the staged folder and emits:

  * a Directory tree under INSTALLFOLDER mirroring the staged subfolders;
  * one Component per folder, holding that folder's files;
  * a ComponentGroup "StagedFiles" the Feature in auscultate.wxs references.

Two rules the generated fragment has to obey, both of them Windows Installer
validation (ICE) rules for a per-user package, and both of them also the right
thing regardless:

  ICE38  A component installing into the user profile must take its key path
         from a registry value under HKCU, not from one of its files. Every
         component here keys off HKCU\\Software\\Delta Creation Co.\\Auscultate
         Setup\\Components. That also means Windows Installer never treats a Qt
         DLL as the thing that proves the product is intact, so a user
         exercising their LGPL right to replace one cannot trip a repair.

  ICE64  A directory created inside the user profile must be removed again on
         uninstall. Every folder gets a RemoveFolder, which Windows Installer
         applies only when the folder is already empty.

Component GUIDs are left as "*" so WiX derives them from the key path. The key
paths are stable across builds, so the GUIDs are too, which is what lets an
upgrade replace files rather than duplicate them.

Usage:
  python3 tools/make_installer_wxs.py --stage package/Auscultate --out installer/files.wxs
  python3 tools/make_installer_wxs.py --print-version
Exit: 0 on success, 1 on a staging folder that cannot be installed.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path
from xml.sax.saxutils import quoteattr

WIX_NAMESPACE = "http://schemas.microsoft.com/wix/2006/wi"

# The one file whose absence means the staging step did not do its job. The CI
# job checks for the Qt DLLs separately, because their absence is a licence
# failure rather than a broken build (ADR 0006).
REQUIRED_FILE = "auscultate.exe"

SETUP_KEY = r"Software\Delta Creation Co.\Auscultate Setup\Components"

# Windows Installer identifiers: letters, digits, underscores and periods, and
# they may not begin with a digit. 72 characters is the limit, and the hash
# suffix is what keeps two different paths from colliding after slugification.
IDENTIFIER_MAX = 72
ILLEGAL_IN_IDENTIFIER = re.compile(r"[^A-Za-z0-9_.]")


def identifier(prefix: str, relative: str) -> str:
    """A stable, legal MSI identifier for a path relative to the stage root."""
    digest = hashlib.sha256(relative.encode("utf-8")).hexdigest()[:8]
    slug = ILLEGAL_IN_IDENTIFIER.sub("_", relative) or "root"
    stem = f"{prefix}_{slug}"
    room = IDENTIFIER_MAX - len(digest) - 1
    return f"{stem[:room]}_{digest}"


def read_version(root: Path) -> str:
    """The project version, from the one place it is written down.

    CMakeLists.txt is the source of truth for the version the executables carry
    in their own resource, so the installer reads it from there rather than
    keeping a second copy that can disagree.
    """
    text = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(
        r"project\s*\([^)]*?VERSION\s+(\d+)\.(\d+)\.(\d+)", text, re.IGNORECASE | re.DOTALL
    )
    if not match:
        raise SystemExit("installer: no 'project(... VERSION x.y.z ...)' in CMakeLists.txt")
    return ".".join(match.groups())


def collect(stage: Path) -> tuple[list[Path], dict[Path, list[Path]]]:
    """Every directory under the stage, and the files directly in each one.

    Sorted throughout: the same staging folder must produce byte-identical WiX,
    or two builds of the same commit would differ for no reason.
    """
    directories: list[Path] = [Path(".")]
    files: dict[Path, list[Path]] = {}

    for path in sorted(stage.rglob("*"), key=lambda p: p.as_posix()):
        relative = path.relative_to(stage)
        if path.is_dir():
            directories.append(relative)
        elif path.is_file():
            files.setdefault(relative.parent, []).append(relative)

    return directories, files


def directory_id(relative: Path) -> str:
    if relative == Path("."):
        return "INSTALLFOLDER"
    return identifier("dir", relative.as_posix())


def render_directories(directories: list[Path], indent: str) -> list[str]:
    """The nested <Directory> tree.

    Built as a tree and walked, rather than tracked with a stack over the sorted
    list. Sorting puts parents before children but it does not put a child
    immediately after its parent -- "a", "a.b", "a/b" sorts in that order,
    because '.' comes before '/' -- and a stack that assumes it does would close
    "a" before nesting "a/b" inside it.
    """
    children: dict[Path, list[Path]] = {}
    for relative in directories:
        if relative == Path("."):
            continue
        children.setdefault(relative.parent, []).append(relative)

    lines: list[str] = []

    def walk(parent: Path, depth: int) -> None:
        for relative in sorted(children.get(parent, []), key=lambda p: p.name):
            pad = "  " * depth
            lines.append(
                f"{indent}{pad}<Directory Id={quoteattr(directory_id(relative))} "
                f"Name={quoteattr(relative.name)}>"
            )
            walk(relative, depth + 1)
            lines.append(f"{indent}{pad}</Directory>")

    walk(Path("."), 0)
    return lines


def render_components(
    stage: Path, directories: list[Path], files: dict[Path, list[Path]], indent: str
) -> list[str]:
    lines: list[str] = []

    for relative in directories:
        # Every folder gets a component, including one that holds nothing but
        # other folders. It is the component that carries the RemoveFolder, so a
        # folder without one is a folder the uninstall leaves behind -- which
        # Windows Installer validation also objects to (ICE64).
        owned = files.get(relative, [])
        key_name = "root" if relative == Path(".") else relative.as_posix()

        lines.append(
            f"{indent}<Component Id={quoteattr(identifier('cmp', key_name))} "
            f"Directory={quoteattr(directory_id(relative))} Guid=\"*\">"
        )
        lines.append(
            f'{indent}  <RegistryValue Root="HKCU" Key={quoteattr(SETUP_KEY)} '
            f'Name={quoteattr(key_name)} Type="integer" Value="1" KeyPath="yes" />'
        )
        lines.append(
            f"{indent}  <RemoveFolder Id={quoteattr(identifier('rmf', key_name))} "
            f"Directory={quoteattr(directory_id(relative))} On=\"uninstall\" />"
        )
        for file_path in owned:
            # Absolute, so it does not matter which directory light.exe is run
            # from when it goes looking for the file to pack.
            source = (stage.resolve() / file_path).as_posix()
            lines.append(
                f"{indent}  <File Id={quoteattr(identifier('fil', file_path.as_posix()))} "
                f"Name={quoteattr(file_path.name)} Source={quoteattr(source)} KeyPath=\"no\" />"
            )
        lines.append(f"{indent}</Component>")

    return lines


def render(stage: Path) -> tuple[str, int]:
    """The fragment, and how many files it describes."""
    directories, files = collect(stage)

    body: list[str] = [
        '<?xml version="1.0" encoding="utf-8"?>',
        "<!-- Generated by tools/make_installer_wxs.py. Do not edit. -->",
        f'<Wix xmlns="{WIX_NAMESPACE}">',
        "  <Fragment>",
        '    <DirectoryRef Id="INSTALLFOLDER">',
    ]
    body += render_directories(directories, indent="      ")
    body += [
        "    </DirectoryRef>",
        '    <ComponentGroup Id="StagedFiles">',
    ]
    body += render_components(stage, directories, files, indent="      ")
    body += [
        "    </ComponentGroup>",
        "  </Fragment>",
        "</Wix>",
        "",
    ]
    return "\n".join(body), sum(len(owned) for owned in files.values())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", help="the staged application folder to install")
    parser.add_argument("--out", help="where to write the generated WiX fragment")
    parser.add_argument("--version-out", help="also write the product version to this file")
    parser.add_argument(
        "--print-version", action="store_true", help="print the product version and stop"
    )
    parser.add_argument("--root", default=".", help="repository root (default: .)")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    version = read_version(root)

    if args.print_version:
        print(version)
        return 0

    if not args.stage or not args.out:
        parser.error("--stage and --out are both required unless --print-version is given")

    stage = Path(args.stage)
    if not stage.is_dir():
        print(f"installer: staging folder not found: {stage}", file=sys.stderr)
        return 1
    if not (stage / REQUIRED_FILE).is_file():
        print(f"installer: {stage / REQUIRED_FILE} is missing -- nothing to install",
              file=sys.stderr)
        return 1

    fragment, file_count = render(stage)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(fragment, encoding="utf-8")

    if args.version_out:
        version_out = Path(args.version_out)
        version_out.parent.mkdir(parents=True, exist_ok=True)
        version_out.write_text(version, encoding="utf-8")

    print(f"installer: {out} describes {file_count} file(s) for Auscultate {version}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
