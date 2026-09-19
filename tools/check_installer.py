#!/usr/bin/env python3
"""Check the Windows installer sources from a machine that is not Windows.

The installer is built by candle and light on a Windows runner at the end of a
job that first compiles and tests the whole product. A misspelled component
reference costs twenty minutes to find out about there. Everything in this file
can be decided from the text alone, so it is decided here instead, in a second,
the same bargain tools/check_includes.py makes for standard headers.

What it checks:

  1. tools/make_eula_rtf.py still produces a well-formed, ASCII-only RTF from
     docs/EULA.md, with the clauses that matter present in it.
  2. tools/make_installer_wxs.py produces well-formed WiX from a staging folder
     shaped like the one windeployqt leaves behind, including awkward shapes
     (nested folders, a folder with a dot in its name).
  3. The two WiX sources agree: every ComponentGroupRef, ComponentRef and
     Directory reference resolves, and no identifier is duplicated, illegal or
     over the Windows Installer length limit.
  4. Every component takes its key path from an HKCU registry value. That is
     required of a per-user package (ICE38), and it is also what stops Windows
     Installer treating a Qt DLL as proof the product is intact -- a user
     exercising their LGPL right to replace one must not trip a repair.
  5. Every folder the installer creates is removed again on uninstall (ICE64).
  6. The publisher is Delta Creation Co., in the installer and in the version
     resource the executables carry.

What it does NOT check, and cannot: this is not Windows Installer validation.
Only light.exe running the ICE suite on the real package can tell you the MSI
is sound, and only running the MSI on Windows can tell you the product installs
and uninstalls. Passing here means the sources are consistent, not that the
installer works.

Usage:  python3 tools/check_installer.py [--root .]
Exit:   0 clean, 1 on any problem.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ElementTree
from pathlib import Path

WIX = "{http://schemas.microsoft.com/wix/2006/wi}"
PUBLISHER = "Delta Creation Co."

# Windows Installer identifier rules.
IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_.]*")
IDENTIFIER_MAX = 72

# Folders Windows owns. We neither create nor remove these.
SYSTEM_DIRECTORIES = {
    "TARGETDIR",
    "LocalAppDataFolder",
    "ProgramMenuFolder",
    "AppDataFolder",
    "DesktopFolder",
    "ProgramFilesFolder",
    "ProgramFiles64Folder",
}

# The staging folder windeployqt produces, plus two shapes chosen to break a
# generator that assumes one flat level: a nested folder, and a folder whose
# name sorts between a parent and its child.
SAMPLE_TREE = [
    "auscultate.exe",
    "auscultate-cli.exe",
    "Qt6Core.dll",
    "Qt6Gui.dll",
    "Qt6Widgets.dll",
    "EULA.md",
    "PRIVACY.md",
    "licences/WiX-Toolset-MS-RL.txt",
    "platforms/qwindows.dll",
    "styles/qmodernwindowsstyle.dll",
    "imageformats/qjpeg.dll",
    "iconengines/qsvgicon.dll",
    "generic/qtuiotouchplugin.dll",
    "nested/deeper/plugin.dll",
    "nested.dotted/plugin.dll",
]


def fail(problems: list[str], message: str) -> None:
    problems.append(message)


def check_eula(root: Path, work: Path, problems: list[str]) -> None:
    rtf_path = work / "EULA.rtf"
    result = subprocess.run(
        [sys.executable, str(root / "tools" / "make_eula_rtf.py"),
         str(root / "docs" / "EULA.md"), str(rtf_path)],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        fail(problems, f"make_eula_rtf.py failed: {result.stderr.strip()}")
        return

    try:
        rtf = rtf_path.read_bytes().decode("ascii")
    except UnicodeDecodeError:
        fail(problems, "the licence RTF is not ASCII: a rich edit control would "
                       "render the stray bytes as whatever the code page says")
        return

    if not rtf.startswith("{\\rtf1"):
        fail(problems, "the licence RTF does not begin with the RTF signature")

    depth = 0
    lowest = 0
    escaped = False
    for character in rtf:
        if escaped:
            escaped = False
        elif character == "\\":
            escaped = True
        elif character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            lowest = min(lowest, depth)
    if depth != 0 or lowest < 0:
        fail(problems, f"the licence RTF has unbalanced braces (ends at depth {depth})")

    # Not a rendering check -- nothing here can do that -- but enough to catch a
    # converter that silently dropped the half of the document it did not
    # understand.
    for clause in (PUBLISHER, "AS IS", "LGPL", "Reverse engineering"):
        if clause not in rtf:
            fail(problems, f"the licence RTF has lost {clause!r} from docs/EULA.md")


def write_sample_stage(stage: Path) -> None:
    for relative in SAMPLE_TREE:
        path = stage / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"\0")


def load(path: Path, substitutions: dict[str, str]) -> ElementTree.Element:
    text = path.read_text(encoding="utf-8")
    for name, value in substitutions.items():
        text = text.replace(f"$(var.{name})", value)
    return ElementTree.fromstring(text)


#  Elements whose Id names a thing rather than pointing at one. A *Ref repeats
#  an identifier on purpose, a Property Id is a Windows Installer property name,
#  and Product/@Id is a GUID or "*".
DEFINING_ELEMENTS = {
    "Component",
    "ComponentGroup",
    "Directory",
    "Feature",
    "File",
    "RegistryKey",
    "RegistryValue",
    "RemoveFolder",
    "Shortcut",
}


def collect_identifiers(trees: list[ElementTree.Element], problems: list[str]) -> None:
    seen: dict[str, str] = {}
    for tree in trees:
        for element in tree.iter():
            identifier = element.get("Id")
            if identifier is None or not element.tag.startswith(WIX):
                continue
            tag = element.tag[len(WIX):]
            if tag not in DEFINING_ELEMENTS:
                continue
            if not IDENTIFIER.fullmatch(identifier):
                fail(problems, f"{tag} Id {identifier!r} is not a legal Windows "
                               f"Installer identifier")
            if len(identifier) > IDENTIFIER_MAX:
                fail(problems, f"{tag} Id {identifier!r} is {len(identifier)} "
                               f"characters; the limit is {IDENTIFIER_MAX}")
            if identifier in seen:
                fail(problems, f"Id {identifier!r} is used twice: {seen[identifier]} "
                               f"and {tag}")
            seen[identifier] = tag


def check_wix(root: Path, work: Path, problems: list[str]) -> None:
    stage = work / "stage"
    write_sample_stage(stage)

    fragment_path = work / "files.wxs"
    result = subprocess.run(
        [sys.executable, str(root / "tools" / "make_installer_wxs.py"),
         "--root", str(root), "--stage", str(stage), "--out", str(fragment_path)],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        fail(problems, f"make_installer_wxs.py failed: {result.stderr.strip()}")
        return

    product_path = root / "packaging" / "windows" / "auscultate.wxs"
    try:
        product = load(product_path, {"ProductVersion": "0.0.0", "LicenceRtf": "EULA.rtf"})
        fragment = load(fragment_path, {})
    except ElementTree.ParseError as error:
        fail(problems, f"the WiX sources are not well-formed XML: {error}")
        return

    collect_identifiers([product, fragment], problems)

    # --- the publisher, asserted rather than assumed ------------------------
    for element in product.iter(f"{WIX}Product"):
        if element.get("Manufacturer") != PUBLISHER:
            fail(problems, f"the product's Manufacturer is "
                           f"{element.get('Manufacturer')!r}, not {PUBLISHER!r}")
    for element in product.iter(f"{WIX}Package"):
        if element.get("Manufacturer") != PUBLISHER:
            fail(problems, f"the package's Manufacturer is "
                           f"{element.get('Manufacturer')!r}, not {PUBLISHER!r}")

    # --- per-user, which is the whole reason for the HKCU key paths ---------
    directories = {
        element.get("Id"): element
        for tree in (product, fragment)
        for element in tree.iter(f"{WIX}Directory")
    }
    directories.update(
        {element.get("Id"): element for element in product.iter(f"{WIX}DirectoryRef")}
    )
    if "INSTALLFOLDER" not in directories:
        fail(problems, "there is no INSTALLFOLDER directory")
    if not any(
        element.get("InstallScope") == "perUser" for element in product.iter(f"{WIX}Package")
    ):
        fail(problems, "the package is not declared InstallScope=\"perUser\"; the HKCU "
                       "key paths and RemoveFolder entries here are written for one that is")

    # --- every reference resolves ------------------------------------------
    components: dict[str, ElementTree.Element] = {}
    for tree in (product, fragment):
        for element in tree.iter(f"{WIX}Component"):
            components[element.get("Id")] = element

    groups = {
        element.get("Id"): element
        for tree in (product, fragment)
        for element in tree.iter(f"{WIX}ComponentGroup")
    }

    for element in product.iter(f"{WIX}ComponentRef"):
        if element.get("Id") not in components:
            fail(problems, f"ComponentRef {element.get('Id')!r} names no component")
    for element in product.iter(f"{WIX}ComponentGroupRef"):
        if element.get("Id") not in groups:
            fail(problems, f"ComponentGroupRef {element.get('Id')!r} names no component group")

    referenced = {element.get("Id") for element in product.iter(f"{WIX}ComponentRef")}
    for group in groups.values():
        for element in group.iter(f"{WIX}Component"):
            referenced.add(element.get("Id"))
    orphans = sorted(set(components) - referenced)
    if orphans:
        fail(problems, f"components no feature installs: {', '.join(orphans)}")

    # --- nothing may stand in the way of replacing a Qt DLL -----------------
    # An advertised shortcut is a Windows Installer resiliency entry point:
    # launching it re-checks the feature and can put back files the user
    # changed. Someone who had swapped in their own Qt6Core.dll, which
    # LGPL-3.0 says they may, could find it quietly overwritten.
    for shortcut in product.iter(f"{WIX}Shortcut"):
        if shortcut.get("Advertise") == "yes":
            fail(problems, f"shortcut {shortcut.get('Id')!r} is advertised. That makes it a "
                           f"repair trigger, which can overwrite a Qt DLL the user replaced "
                           f"under LGPL-3.0 (ADR 0006)")

    # --- ICE38, and the LGPL reason for obeying it --------------------------
    removed: set[str] = set()
    for identifier, component in components.items():
        directory = component.get("Directory")
        if directory and directory not in directories:
            fail(problems, f"component {identifier!r} installs into directory "
                           f"{directory!r}, which is not declared")

        key_paths = [child for child in component if child.get("KeyPath") == "yes"]
        nested_keys = [
            child
            for key in component.iter(f"{WIX}RegistryKey")
            for child in key
            if child.get("KeyPath") == "yes"
        ]
        key_paths += nested_keys
        if len(key_paths) != 1:
            fail(problems, f"component {identifier!r} has {len(key_paths)} key paths; "
                           f"it must have exactly one")
            continue
        key_path = key_paths[0]
        if key_path.tag != f"{WIX}RegistryValue":
            fail(problems, f"component {identifier!r} keys off "
                           f"{key_path.tag[len(WIX):]}, not a registry value. A per-user "
                           f"package must key off HKCU (ICE38), and keying off a shipped "
                           f"DLL would let a replaced Qt library trigger a repair")
            continue
        root_attribute = key_path.get("Root")
        if root_attribute is None:
            parent = next(
                (key for key in component.iter(f"{WIX}RegistryKey") if key_path in list(key)),
                None,
            )
            root_attribute = parent.get("Root") if parent is not None else None
        if root_attribute != "HKCU":
            fail(problems, f"component {identifier!r} keys off registry root "
                           f"{root_attribute!r}; a per-user package must use HKCU")

        for child in component.iter(f"{WIX}RemoveFolder"):
            removed.add(child.get("Directory"))

    # --- ICE64: put back every folder we took ------------------------------
    for identifier in sorted(directories):
        if identifier in SYSTEM_DIRECTORIES or identifier is None:
            continue
        element = directories[identifier]
        if element.tag == f"{WIX}DirectoryRef":
            continue
        if identifier not in removed:
            fail(problems, f"directory {identifier!r} is created under the user profile "
                           f"but nothing removes it on uninstall (ICE64)")


def check_version_resource(root: Path, problems: list[str]) -> None:
    template = root / "packaging" / "windows" / "version.rc.in"
    if not template.is_file():
        fail(problems, f"{template} is missing: the executables would ship with no "
                       f"version resource and no publisher")
        return
    text = template.read_text(encoding="utf-8")
    if PUBLISHER not in text:
        fail(problems, f"the version resource does not name {PUBLISHER!r} as CompanyName")
    if any(ord(character) > 127 for character in text):
        fail(problems, "the version resource template is not ASCII; the resource compiler "
                       "reads it in the system code page unless it carries a byte order "
                       "mark, so a stray character would become mojibake in the file "
                       "properties dialog")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".")
    args = parser.parse_args()
    root = Path(args.root).resolve()

    problems: list[str] = []
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        check_eula(root, work, problems)
        check_wix(root, work, problems)
    check_version_resource(root, problems)

    if problems:
        print(f"installer check FAILED with {len(problems)} problem(s):\n")
        for problem in problems:
            print(f"  ERROR: {problem}")
        return 1

    print("installer check: OK — the WiX sources, the licence page and the version "
          "resource are consistent")
    print("installer check: this is NOT Windows Installer validation; only light.exe "
          "and a real Windows machine can tell you the package works")
    return 0


if __name__ == "__main__":
    sys.exit(main())
