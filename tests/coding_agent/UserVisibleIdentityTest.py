#!/usr/bin/env python3
"""Reject internal package names in user-visible string literals.

ADR 0045: every user-visible Runtime name is `pike`; `cch`, `cpp_harness`, and
`cpp-coding-harness` are repository-internal architecture vocabulary. A
user-visible sentence that carries one of them ships the wrong product identity
(issue #790: the project-trust prompt read "This allows cch to load ..." while
its test asserted the leaking string verbatim).

The scan reads string literals from the user-facing sources, skipping comments,
character literals, and raw strings with any delimiter. A literal is reported
when it contains an internal name as a word *and* reads as prose (contains a
space) — the shape a user actually sees. Identifier-shaped literals such as a
temporary-file prefix are out of scope by construction.

The detector carries a self-test so a check that silently stops matching cannot
pass as acceptance.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = (ROOT / "src" / "coding_agent", ROOT / "src" / "tui", ROOT / "src" / "cli")

INTERNAL_NAME = re.compile(r"\bcch\b|cpp_harness|cpp-coding-harness")

RAW_STRING_OPEN = re.compile(r'R"([^()\\ ]{0,16})\(')


def _skip_raw_string(text: str, start: int) -> int | None:
    """Return the index after a raw string starting at `start`, or None."""
    match = RAW_STRING_OPEN.match(text, start)
    if match is None:
        return None
    delimiter = match.group(1)
    terminator = ')' + delimiter + '"'
    end = text.find(terminator, match.end())
    return len(text) if end == -1 else end + len(terminator)


def string_literals(text: str):
    """Yield (line_number, literal) for every double-quoted literal in `text`."""
    index = 0
    length = len(text)
    while index < length:
        character = text[index]
        if character == "/" and text.startswith("//", index):
            newline = text.find("\n", index)
            index = length if newline == -1 else newline + 1
            continue
        if character == "/" and text.startswith("/*", index):
            end = text.find("*/", index + 2)
            index = length if end == -1 else end + 2
            continue
        if character == "R" and text.startswith('R"', index):
            skipped = _skip_raw_string(text, index)
            if skipped is not None:
                index = skipped
                continue
        if character == "'":
            index += 1
            while index < length and text[index] != "'":
                index += 2 if text[index] == "\\" else 1
            index += 1
            continue
        if character == '"':
            line = text.count("\n", 0, index) + 1
            cursor = index + 1
            literal: list[str] = []
            while cursor < length and text[cursor] != '"':
                if text[cursor] == "\\":
                    literal.append(text[cursor : cursor + 2])
                    cursor += 2
                else:
                    literal.append(text[cursor])
                    cursor += 1
            yield line, "".join(literal)
            index = cursor + 1
            continue
        index += 1


def violating_literals(text: str) -> list[tuple[int, str]]:
    """Report prose literals that carry an internal name."""
    violations = []
    for line, literal in string_literals(text):
        if INTERNAL_NAME.search(literal) and " " in literal:
            violations.append((line, literal))
    return violations


def self_test() -> None:
    """The detector must separate the reported leak from identifier-shaped text."""
    leak = "This allows cch to load .pi settings and resources."
    if not violating_literals(f'"{leak}"'):
        raise AssertionError("detector no longer reports the reported leak")
    for benign in ('"cch-user-bash-"', '"CCH_STRICT_NO_EXCEPTIONS"', '"cch::support"'):
        if violating_literals(benign):
            raise AssertionError(f"detector reports identifier-shaped literal {benign}")
    raw = 'R"CCH_THEME({ "theme": "dark" })CCH_THEME"\n" a cch sentence"'
    lines = [line for line, _ in violating_literals(raw)]
    if lines != [2]:
        raise AssertionError(f"raw-string handling is wrong: {lines}")


def main() -> int:
    self_test()
    findings: list[str] = []
    scanned = 0
    for base in SOURCES:
        for path in sorted(base.rglob("*")):
            if not path.is_file() or path.suffix not in (".cpp", ".hpp"):
                continue
            scanned += 1
            text = path.read_text(encoding="utf-8", errors="replace")
            for line, literal in violating_literals(text):
                findings.append(f"{path.relative_to(ROOT)}:{line}: {literal}")
    if findings:
        print("user-visible literals carry an internal name (ADR 0045):", file=sys.stderr)
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        return 1
    print(f"user-visible identity: PASS ({scanned} sources scanned)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
