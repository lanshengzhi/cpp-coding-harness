#!/usr/bin/env python3
"""Pin the System Prompt's documentation references to files Pike actually ships.

Regression cover for the defect ADR 0065 records: the prompt advertised ten
`docs/*.md` paths and an entire `examples/` tree that do not exist in this
repository, so the model was told to read files that could not be read.

Five assertions, matching the five constraints agreed for this slice:

1. every referenced path resolves to a file in the tree
2. every injected directory resolves to a real directory
3. prose wildcards (`docs/...`) are excluded, not treated as paths
4. no reference instructs workflow-around-the-document (doc-maintenance voice)
5. no bare-word resource name survives that has no real directory behind it

Assertion 5 is the one the original defect slipped through: `read the docs and
examples` named `examples` as a bare word, with no slash, so a path-shaped check
passed while the prompt still pointed at a directory that does not exist.
"""

from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
GOLDEN_DIR = ROOT / "tests" / "fixtures" / "prompts" / "goldens"
PROMPT_SOURCE = ROOT / "src" / "coding_agent" / "prompt" / "SystemPromptBuilder.cpp"

# `docs/usage.md` and friends. A trailing `/...` is a prose wildcard, not a path.
PATH_RE = re.compile(r"\b(?:docs|examples)/[A-Za-z0-9._/-]+?\.(?:md|txt|json)\b")
WILDCARD_RE = re.compile(r"\b(?:docs|examples)/\.\.\.")

# Doc-maintenance voice: the prompt should say which topic a document covers, not
# how the document itself is maintained. Per ADR 0066 this list lives here as data
# with a citation to the review that produced it, so the next person who hits a
# false positive reads the reason before weakening it. Fail closed on a hit: a
# legitimate phrase is reviewed and added below deliberately, never silently passed.
WORKFLOW_VOICE_PATTERNS = (
    re.compile(r"\bthis (?:file|document|page)\b", re.I),
    re.compile(r"\bdoes not (?:re-decide|authorize)\b", re.I),
    re.compile(r"\bfor (?:doc|document) maintenance\b", re.I),
    re.compile(r"\bwhen (?:writing|updating) this\b", re.I),
)

# Bare-word resource names that must not survive in the shipped prompt.
FORBIDDEN_BARE_WORDS = ("examples",)


def goldens() -> list[Path]:
    found = sorted(GOLDEN_DIR.glob("system-prompt-*.txt"))
    if not found:
        raise AssertionError(f"no prompt goldens under {GOLDEN_DIR}")
    return found


def referenced_paths(text: str) -> set[str]:
    return {m.group(0) for m in PATH_RE.finditer(text)}


def check_referenced_paths_exist(files: dict[str, str]) -> None:
    """Assertion 1 + 3: every referenced path exists; prose wildcards are excluded."""
    for label, text in files.items():
        for path in sorted(referenced_paths(text)):
            if WILDCARD_RE.search(path):
                continue
            if not (ROOT / path).is_file():
                raise AssertionError(
                    f"{label}: prompt references {path!r}, which is not a file in the tree"
                )


def check_injected_directories_exist(rendered: dict[str, str]) -> None:
    """Assertion 2: an injected `Additional docs:` path must be a real directory.

    Rendered output only. The C++ source splits that header across string literals
    (`text += "\\n- Additional docs: "; text += options.docsPath;`), so a regex over
    the source would match the literal fragment rather than a path. What ships is the
    rendered prompt, so that is what this checks.
    """
    for label, text in rendered.items():
        match = re.search(r"- Additional docs: (\S+)", text)
        if match is None:
            if "<docs>" in text:
                raise AssertionError(f"{label}: has a <docs> section but no docs path header")
            continue
        target = Path(match.group(1))
        if target.name == "docs":
            # Golden paths are scrubbed to /pike/docs; assert the shape and that the
            # real repository has a docs/ tree behind it.
            if not (ROOT / "docs").is_dir():
                raise AssertionError("repository has no docs/ directory")
            continue
        if not target.is_dir():
            raise AssertionError(f"{label}: injected docs directory {target} does not exist")


def check_no_workflow_voice(files: dict[str, str]) -> None:
    """Assertion 4: references name a topic, never how the document is maintained."""
    for label, text in files.items():
        for pattern in WORKFLOW_VOICE_PATTERNS:
            hit = pattern.search(text)
            if hit:
                raise AssertionError(
                    f"{label}: prompt carries doc-maintenance voice {hit.group(0)!r}; "
                    "references must state the topic the document covers"
                )


def check_no_fictional_resource_surface(files: dict[str, str]) -> None:
    """Assertion 5: no bare-word resource name may survive without a real directory.

    This is deliberately a whole-token ban rather than a resolution check: the
    failure mode is a prompt that *claims* a surface, and any resolution logic here
    could be tuned until it stops complaining -- which is how a real check gets
    neutered. A hit is reviewed and allow-listed deliberately, never auto-passed.
    """
    for label, text in files.items():
        for word in FORBIDDEN_BARE_WORDS:
            hits = re.findall(rf"\b{word}\b", text, re.I)
            if hits:
                raise AssertionError(
                    f"{label}: prompt still names {word!r} ({len(hits)} occurrence(s)); "
                    "Pike ships no such tree, so the prompt must not advertise one"
                )


# Digests of the recorded pi docsBlock at the pinned baseline
# (f07218c4d4bbc12bef056a7058c3dd49dfe41abe / @earendil-works/pi-coding-agent@0.87.1).
# A digest is used rather than a git ref on purpose: comparing against `HEAD` passes on
# clean CI, because HEAD already contains whatever was last committed -- it only catches
# an *unstaged* edit. @Tony caught that during #8's spec review. These digests are the
# fixed reference, so committing a rewritten pi side fails here.
PINNED_PI_DOCS_BLOCK_SHA256 = {
    "default": "428b130fba701ac520b8dfed16c239bc96fa99c6494282e6847a297443d516e2",
    "empty-tools": "428b130fba701ac520b8dfed16c239bc96fa99c6494282e6847a297443d516e2",
}
PINNED_BASELINE = "f07218c4d4bbc12bef056a7058c3dd49dfe41abe"


def check_snapshot_delta_is_our_side_only() -> None:
    """Upstream's recorded pi text must still hash to the pinned baseline digest.

    The snapshots pin two things: pi's message text captured at the baseline (evidence)
    and the Pike identity/docs delta (ours). Only the second may ever change. This compares
    against a fixed digest rather than a moving ref, so it fails even when the rewrite is
    committed cleanly.
    """
    for name, expected in PINNED_PI_DOCS_BLOCK_SHA256.items():
        path = ROOT / "fixtures" / "pi-coding-agent" / "prompts" / f"system-prompt-{name}-message.json"
        if not path.is_file():
            raise AssertionError(f"missing pi-differential snapshot: {path}")
        data = json.loads(path.read_text(encoding="utf-8"))
        meta = data.get("meta") or {}
        if meta.get("baseline") != PINNED_BASELINE:
            raise AssertionError(
                f"{path.name}: pinned baseline moved to {meta.get('baseline')!r}; evidence may not "
                f"be re-pinned here without an owner decision (expected {PINNED_BASELINE})"
            )
        pi_docs = (data.get("identityDelta") or {}).get("docsBlock", {}).get("pi")
        if pi_docs is None:
            raise AssertionError(f"{path.name}: recorded pi docsBlock is missing")
        actual = hashlib.sha256(pi_docs.encode()).hexdigest()
        if actual != expected:
            raise AssertionError(
                f"{path.name}: the recorded pi docsBlock was rewritten. Upstream evidence pinned "
                f"at {PINNED_BASELINE} must stay verbatim; only the pike side is ours to change."
            )


def main() -> int:
    rendered = {path.name: path.read_text(encoding="utf-8") for path in goldens()}
    files = dict(rendered)
    files[f"{PROMPT_SOURCE.name} (source)"] = PROMPT_SOURCE.read_text(encoding="utf-8")

    check_referenced_paths_exist(files)
    check_injected_directories_exist(rendered)
    check_no_workflow_voice(files)
    check_no_fictional_resource_surface(files)
    check_snapshot_delta_is_our_side_only()

    for name in files:
        print(f"  checked {name}")
    print("pi-ai prompt references: PASS (paths, directories, phrasing, resource surface)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
