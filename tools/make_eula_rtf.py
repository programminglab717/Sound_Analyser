#!/usr/bin/env python3
"""Convert docs/EULA.md to the RTF the installer's licence page displays.

The licence page has to be RTF -- it is a rich edit control -- and docs/EULA.md
is the document that is going to a solicitor. Keeping a second, hand-maintained
RTF copy would mean the agreement a user accepts and the agreement under review
could quietly diverge, which is the one kind of drift a licence document must
not have. So the RTF is built from the Markdown every time the installer is
built, and there is only ever one text.

The subset of Markdown handled is the subset docs/EULA.md uses: headings,
paragraphs, block quotes, bullet lists, pipe tables, bold and italic. Anything
else is passed through as text rather than guessed at. A table becomes one line
per row with the cells separated by dashes, because a rich edit control in a
setup dialog renders RTF tables badly and an unreadable clause is worse than an
unaligned one.

Usage:  python3 tools/make_eula_rtf.py docs/EULA.md build/EULA.rtf
Exit:   0 on success.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# Half-points. 20 is 10pt body text, which is what the WiX licence page expects.
BODY = 20
HEADING_SIZES = {1: 32, 2: 26, 3: 22}

BOLD = re.compile(r"\*\*(.+?)\*\*", re.DOTALL)
ITALIC = re.compile(r"(?<!\*)\*(?!\*)(.+?)(?<!\*)\*(?!\*)", re.DOTALL)


def escape(text: str) -> str:
    """RTF-escape, including the three non-ASCII characters the EULA uses.

    A rich edit control reading a raw em dash out of a \\ansi document shows
    whatever the code page says, which is not an em dash. \\uNNNN? gives it the
    real character and an ASCII fallback for anything that cannot show it.
    """
    out: list[str] = []
    for character in text:
        if character in "\\{}":
            out.append("\\" + character)
        elif ord(character) < 128:
            out.append(character)
        else:
            code = ord(character)
            # RTF takes a signed 16-bit code unit.
            if code > 0xFFFF:
                out.append("?")
                continue
            if code > 0x7FFF:
                code -= 0x10000
            out.append(f"\\u{code}?")
    return "".join(out)


def inline(text: str) -> str:
    """Escape first, then turn the emphasis markers into RTF control words."""
    escaped = escape(text.strip())
    escaped = BOLD.sub(lambda m: r"\b " + m.group(1) + r"\b0 ", escaped)
    escaped = ITALIC.sub(lambda m: r"\i " + m.group(1) + r"\i0 ", escaped)
    return escaped


def table_rows(lines: list[str]) -> list[list[str]]:
    rows: list[list[str]] = []
    for line in lines:
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        # The |---|---| separator carries no text.
        if all(re.fullmatch(r":?-{2,}:?", cell) for cell in cells if cell):
            continue
        rows.append(cells)
    return rows


def convert(markdown: str) -> str:
    lines = markdown.replace("\r\n", "\n").split("\n")
    body: list[str] = []
    index = 0

    while index < len(lines):
        line = lines[index]
        stripped = line.strip()

        if not stripped:
            index += 1
            continue

        heading = re.match(r"(#{1,6})\s+(.*)", stripped)
        if heading:
            level = min(len(heading.group(1)), 3)
            size = HEADING_SIZES[level]
            body.append(
                rf"\pard\sa120\sb180\b\fs{size} {inline(heading.group(2))}\b0\fs{BODY}\par"
            )
            index += 1
            continue

        if stripped.startswith("|"):
            block = []
            while index < len(lines) and lines[index].strip().startswith("|"):
                block.append(lines[index])
                index += 1
            rows = table_rows(block)
            for position, row in enumerate(rows):
                joined = " — ".join(cell for cell in row if cell)
                prefix = r"\b " if position == 0 else ""
                suffix = r"\b0 " if position == 0 else ""
                body.append(rf"\pard\sa40\li360 {prefix}{inline(joined)}{suffix}\par")
            continue

        if stripped.startswith(">"):
            block = []
            while index < len(lines) and lines[index].strip().startswith(">"):
                block.append(lines[index].strip().lstrip(">").strip())
                index += 1
            body.append(rf"\pard\sa120\li360\i {inline(' '.join(block))}\i0\par")
            continue

        if re.match(r"[-*]\s+", stripped):
            while index < len(lines) and re.match(r"[-*]\s+", lines[index].strip()):
                item = re.sub(r"^[-*]\s+", "", lines[index].strip())
                body.append(rf"\pard\sa40\fi-240\li600\bullet\tab {inline(item)}\par")
                index += 1
            continue

        block = []
        while index < len(lines) and lines[index].strip() and not re.match(
            r"(#{1,6}\s|\||>|[-*]\s)", lines[index].strip()
        ):
            block.append(lines[index].strip())
            index += 1
        body.append(rf"\pard\sa120 {inline(' '.join(block))}\par")

    header = (
        r"{\rtf1\ansi\ansicpg1252\uc1\deff0\deflang1033"
        r"{\fonttbl{\f0\fnil\fcharset0 Segoe UI;}}"
        rf"\fs{BODY}"
    )
    return header + "\n" + "\n".join(body) + "\n}\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", help="the Markdown licence document")
    parser.add_argument("destination", help="the RTF file to write")
    args = parser.parse_args()

    source = Path(args.source)
    if not source.is_file():
        print(f"licence page: {source} not found", file=sys.stderr)
        return 1

    destination = Path(args.destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    rtf = convert(source.read_text(encoding="utf-8"))
    # ASCII only: the escaping above has already turned everything else into
    # \uNNNN?, so a non-ASCII byte here would mean a hole in it.
    destination.write_bytes(rtf.encode("ascii"))

    print(f"licence page: {source} -> {destination} ({destination.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
