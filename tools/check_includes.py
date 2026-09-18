#!/usr/bin/env python3
"""Fail on a standard header used but never included, directly or through ours.

Every standard library ships a different set of transitive includes. libstdc++
pulls <algorithm> in through several common headers; the MSVC STL does not. So a
file that says std::max with only <vector> included compiles here and fails on
Windows, and the first anyone knows is a red CI run twenty minutes later.

This resolves the include graph across *our* headers only. A standard header
included by one of ours counts, because that is a real include we control. A
standard header that some other standard header happens to pull in does not,
because that is exactly the assumption that breaks.

Usage:  python3 tools/check_includes.py [paths...]
Exit:   0 clean, 1 on any violation.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# Symbol patterns and the header that is meant to provide them. Deliberately
# incomplete: it covers what has actually bitten, not everything that could.
REQUIRED = {
    "<algorithm>": r"\bstd::(max|min|clamp|sort|stable_sort|fill|fill_n|copy|copy_n|reverse|unique|find|find_if|lower_bound|upper_bound|swap_ranges|all_of|any_of|none_of|count_if)\s*[(<]",
    "<numeric>": r"\bstd::(accumulate|inner_product|iota|reduce)\s*[(<]",
    "<cstring>": r"\bstd::(memcpy|memset|memmove|strlen|strncmp|strcmp)\s*\(",
    "<cmath>": r"\bstd::(sqrt|pow|log10|log2|log|exp|sin|cos|tan|atan2|fabs|floor|ceil|round|lround|hypot|fmod|isfinite|isnan)\s*\(",
    "<limits>": r"\bstd::numeric_limits\s*<",
    "<cstdint>": r"\bstd::(u?int(8|16|32|64)_t)\b",
    "<memory>": r"\bstd::(unique_ptr|shared_ptr|weak_ptr|make_unique|make_shared)\s*[(<]",
    "<utility>": r"\bstd::(move|forward|exchange|as_const)\s*[(<]",
    "<string>": r"\bstd::(string|to_string)\b",
    "<vector>": r"\bstd::vector\s*<",
    "<optional>": r"\bstd::(optional|nullopt)\b",
    "<atomic>": r"\bstd::atomic\b",
    "<mutex>": r"\bstd::(mutex|lock_guard|unique_lock|scoped_lock)\b",
    "<thread>": r"\bstd::(thread|jthread|this_thread)\b",
    "<complex>": r"\bstd::complex\s*<",
    "<array>": r"\bstd::array\s*<",
    "<functional>": r"\bstd::function\s*<",
    "<numbers>": r"\bstd::numbers::",
    "<filesystem>": r"\bstd::filesystem::",
    "<cstdio>": r"\bstd::(printf|fprintf|snprintf|fputs|fflush)\s*\(",
}


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def direct_includes(text: str) -> tuple[set[str], set[str]]:
    """(standard headers, project headers) this file includes itself."""
    standard, project = set(), set()
    for match in re.finditer(r'^\s*#\s*include\s*([<"][^>"]+[>"])', text, re.M):
        token = match.group(1)
        if token.startswith("<sa/"):
            project.add(token[1:-1])
        elif token.startswith("<"):
            standard.add(token)
        else:
            project.add(token[1:-1])
    return standard, project


def main() -> int:
    roots = [Path(p) for p in sys.argv[1:]] or [Path("src")]
    files: dict[Path, str] = {}
    for root in roots:
        for path in sorted(root.rglob("*")):
            if path.suffix in (".cpp", ".h"):
                files[path] = path.read_text(encoding="utf-8", errors="replace")

    # Map a project header's include-path spelling to its file, so "sa/dsp/Fft.h"
    # resolves wherever it lives.
    by_include_path: dict[str, Path] = {}
    for path in files:
        parts = path.parts
        if "include" in parts:
            index = len(parts) - 1 - parts[::-1].index("include")
            by_include_path["/".join(parts[index + 1 :])] = path
        # A header beside its source is included by bare name.
        by_include_path.setdefault(path.name, path)

    def reachable_standard(path: Path, seen: set[Path]) -> set[str]:
        if path in seen or path not in files:
            return set()
        seen.add(path)
        standard, project = direct_includes(files[path])
        for include in project:
            target = by_include_path.get(include)
            if target is not None:
                standard |= reachable_standard(target, seen)
        return standard

    problems = []
    for path, text in files.items():
        code = strip_comments(text)
        available = reachable_standard(path, set())
        for header, pattern in REQUIRED.items():
            if re.search(pattern, code) and header not in available:
                symbol = re.search(pattern, code).group(0).strip("(<")
                problems.append(f"{path}: uses {symbol} but {header} is never included")

    for problem in sorted(problems):
        print(f"ERROR: {problem}")
    if problems:
        print(f"\n{len(problems)} missing include(s). Every standard library ships a different")
        print("set of transitive includes; relying on one is how a Windows build breaks.")
        return 1
    print(f"includes: {len(files)} files checked, every standard header used is included")
    return 0


if __name__ == "__main__":
    sys.exit(main())
