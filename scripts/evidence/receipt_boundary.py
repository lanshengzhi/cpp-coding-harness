#!/usr/bin/env python3
"""#1202 boundary check: a receipt's cited test must actually read its registered artifact.

A whole-row delivery receipt (``docs/research/<baseline>/1058-deliveries.json``) claims that a
named C++ comparison test reads the frozen pi observation its rows point at. The #1177
reconciliation showed that several receipts cited tests that compared C++ literals while never
opening the registered artifact, so the claim was not reproducible from the pinned evidence.
This read-only check closes that boundary: for every active receipt it asks, per claim, whether
the cited test source code actually references the claim's registered artifact (the
``fixtures/<baseline>`` evidence bundle files: capture outputs, environment rows and the like).

The check is static source analysis, deliberately weaker than execution: the default matcher is
name + path matching over comment-stripped source, the same mechanical heuristic the 2026-11-04
manual audit ran (docs/wiki/pi-alignment/residual-gap-audit-2026-11-04.md). A reference must be
code, not prose: a mention in a comment, a Catch2 tag such as ``[chord][hardware-cursor]`` or a
scenario key such as ``"audit-1057-context"`` is not an anchor. The matcher is an exchangeable
layer (``SourceMatcher``): a stronger AST- or regex-based reader can replace it without touching
claim parsing, artifact registration or reporting.

A claim fails closed when its structure is unparsable, when its artifact is not a registered
evidence artifact of the selected baseline, when a cited test source is missing or unreadable, or
when the source (including the test helpers it quotes-includes under ``tests/``) references
neither the artifact path nor its family directory together with the artifact file name in a
shared file-and-statement context; disjoint tokens spread across files or unrelated
statements are not an anchor (issue #1231). A
receipt passes only when every claim passes; the process exits non-zero when any active receipt
fails. Receipts withdrawn whole (removed from the deliveries file, e.g. #1104 and #1189) are not
active and are not checked; per-claim withdrawals are part of the full receipts and are checked
like every other claim.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts" / "evidence"))
import audit_evidence as audit

DEFAULT_BASELINE = "pi-v1.3.0"
DELIVERIES_SCHEMA = "pike/pi-v1.3.0-capability-deliveries/1"
TEST_INCLUDE_ROOT = Path("tests")
QUOTE_INCLUDE_PATTERN = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.MULTILINE)
ARTIFACT_PATTERN = re.compile(
    r"(?P<family>audit-[0-9]+(?:-[a-z0-9]+(?:-[a-z0-9]+)*)?)/(?P<file>[^/]+\.json)"
)
FIXTURE_RELATIVE_PATTERN = r"(?:fixtures/{baseline}/)?{family}/{file}"
INCLUDE_DEPTH_LIMIT = 4


class BoundaryError(Exception):
    """A selection, registry or receipts input failed validation."""


class ClaimFailure:
    """One exact unmatched artifact reference inside a claim."""

    def __init__(self, claim_id: str, artifact: str, test_source: str, role: str, reason: str) -> None:
        self.claim_id = claim_id
        self.artifact = artifact
        self.test_source = test_source
        self.role = role
        self.reason = reason

    def describe(self) -> str:
        return (
            f"claim {self.claim_id}: {self.artifact} ({self.role}, {self.test_source}): "
            f"{self.reason}"
        )


def require(condition: bool, message: str) -> None:
    if not condition:
        raise BoundaryError(message)


def strip_comments(code: str) -> str:
    """Remove // and / * comments without touching // or quotes inside literals."""
    out: list[str] = []
    index = 0
    state = "code"
    quote = ""
    while index < len(code):
        char = code[index]
        nxt = code[index + 1] if index + 1 < len(code) else ""
        if state == "code":
            if char == "/" and nxt == "/":
                state = "line"
                index += 2
                continue
            if char == "/" and nxt == "*":
                state = "block"
                index += 2
                continue
            if char == "R" and nxt == '"':
                # Raw string R"delimiter( ... )delimiter": copy it verbatim.
                close_paren = code.find("(", index + 2, index + 20)
                if close_paren != -1:
                    delimiter = code[index + 2 : close_paren]
                    terminator = ")" + delimiter + '"'
                    end = code.find(terminator, close_paren + 1)
                    if end != -1:
                        end += len(terminator)
                        out.append(code[index:end])
                        index = end
                        continue
            if char in "\"'":
                state = "string"
                quote = char
            out.append(char)
        elif state == "string":
            out.append(char)
            if char == "\\":
                if nxt:
                    out.append(nxt)
                    index += 2
                    continue
            elif char == quote:
                state = "code"
        elif state == "line":
            if char == "\n":
                out.append(char)
                state = "code"
        elif state == "block":
            if char == "*" and nxt == "/":
                state = "code"
                index += 2
                continue
            if char == "\n":
                out.append(char)
        index += 1
    return "".join(out)


def quoted_includes(code: str) -> list[str]:
    return QUOTE_INCLUDE_PATTERN.findall(code)


class SourceMatcher:
    """The matcher seam: replace with an AST- or regex-based implementation to go stronger."""

    def references(self, baseline: str, code_blobs: list[str], artifact: str) -> bool:
        raise NotImplementedError


class StaticNamePathMatcher(SourceMatcher):
    """Name + path matching over comment-stripped C++ source (the 2026-11-04 heuristic).

    An artifact ``audit-1053/worker-lifetime.json`` is referenced when one cited source blob
    (the test itself or one helper it quoted-includes under ``tests/``) contains, after comment
    stripping, any of:

    - the artifact path (with an optional ``fixtures/<baseline>/`` prefix), as one quoted
      literal or reassembled from path segments;
    - the quoted artifact file name together with the family directory in one statement —
      the family as a path/string boundary segment (``"audit-1053"``) or as its namespace
      spelling (``audit_1053``), e.g. the two-argument loader call
      ``read_evidence("audit-1209", "screen-state.json")``;
    - the family directory as a path segment in a path-building statement together with the
      quoted file name passed to a call in the same file, e.g. a file-local ``audit_root()``
      returning ``... / "audit-1053"`` with call sites quoting ``"worker-lifetime.json"``.

    Disjoint tokens never anchor (issue #1231): a family mention in one file with the file
    name in another, or the two tokens in unrelated statements of the same file, is not a
    reference. The family token must sit on a path, string or namespace boundary, so a
    scenario key like ``"audit-1057-context"`` or a Catch2 tag like ``[chord]`` does not
    count as a reference.
    """

    def references(self, baseline: str, code_blobs: list[str], artifact: str) -> bool:
        match = ARTIFACT_PATTERN.fullmatch(artifact)
        require(match is not None, f"invalid artifact reference: {artifact!r}")
        family, file_name = match.group("family"), match.group("file")
        token = r"[A-Za-z0-9_.-]"
        family_segment = re.compile(rf"(?<!{token}){re.escape(family)}(?!{token})")
        family_namespace = re.compile(rf"(?<!{token}){re.escape(family.replace('-', '_'))}(?!{token})")
        file_literal = re.compile(rf'["\']{re.escape(file_name)}["\']')
        path_pattern = re.compile(
            FIXTURE_RELATIVE_PATTERN.format(
                baseline=re.escape(baseline),
                family=re.escape(family),
                file=re.escape(file_name),
            )
        )
        family_path = re.compile(rf'"/[^"]*{re.escape(family)}[^"]*"|/\s*"{re.escape(family)}"')
        file_in_call = re.compile(rf"\([^()]*{re.escape(file_name)}[\"']")
        for code in code_blobs:
            stripped = strip_comments(code)
            if path_pattern.search(stripped):
                return True
            for statement in stripped.split(";"):
                if file_literal.search(statement) and (
                    family_segment.search(statement) or family_namespace.search(statement)
                ):
                    return True
            if family_path.search(stripped) and file_in_call.search(stripped):
                return True
        return False


class ReceiptClaim:
    """One parsed claim (row): comparisons plus an optional discriminating case."""

    def __init__(self, claim_id: str, comparisons: list[dict], discriminating: dict | None) -> None:
        self.claim_id = claim_id
        self.comparisons = comparisons
        self.discriminating = discriminating

    @property
    def artifacts(self) -> list[str]:
        seen: list[str] = []
        for comparison in self.comparisons:
            artifact = comparison["artifact"]
            if artifact not in seen:
                seen.append(artifact)
        return seen


def load_deliveries(path: Path, baseline: str) -> dict:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise BoundaryError(f"cannot read deliveries {path}: {error}") from error
    require(isinstance(document, dict), f"invalid deliveries document: {path}")
    require(document.get("schema") == DELIVERIES_SCHEMA, f"unexpected deliveries schema: {path}")
    require(
        document.get("baseline") == baseline,
        f"deliveries {path} record baseline {document.get('baseline')!r}, not {baseline!r}",
    )
    records = document.get("records")
    require(isinstance(records, list) and records, f"deliveries {path} record no receipts")
    return document


def registered_artifacts(fixture_root: Path, baseline_name: str) -> set[str]:
    """The artifact paths registered for the baseline, bytes verified against the manifest."""
    registry = audit.load_registry(fixture_root)
    baseline = audit.resolve_baseline(registry, baseline_name)
    registered: set[str] = set()
    for family, record in sorted(baseline["bundles"].items()):
        record = audit.object_value(record, f"invalid registry record for {family}")
        manifest_path = audit.safe_child(fixture_root, record.get("manifest"), f"manifest for {family}")
        digest = record.get("manifestSha256")
        require(
            isinstance(digest, str) and audit.DIGEST_PATTERN.fullmatch(digest) is not None,
            f"invalid manifest digest ({family})",
        )
        try:
            manifest_bytes = manifest_path.read_bytes()
        except OSError as error:
            raise BoundaryError(f"missing evidence bundle for {family}: {manifest_path}") from error
        require(
            audit.sha256_bytes(manifest_bytes) == digest,
            f"evidence manifest digest mismatch ({family})",
        )
        try:
            manifest = json.loads(manifest_bytes)
        except (UnicodeError, json.JSONDecodeError) as error:
            raise BoundaryError(f"invalid evidence manifest ({family}): {error}") from error
        require(isinstance(manifest, dict), f"invalid evidence manifest ({family})")
        require(manifest.get("schema") == audit.MANIFEST_SCHEMA, f"invalid evidence manifest schema ({family})")
        require(manifest.get("baseline") == baseline_name, f"evidence baseline mismatch ({family})")
        require(manifest.get("family") == family, f"evidence family mismatch ({family})")
        artifacts = manifest.get("artifacts")
        require(isinstance(artifacts, list) and artifacts, f"empty evidence bundle ({family})")
        for item in artifacts:
            require(isinstance(item, dict), f"invalid artifact record ({family})")
            require(item.get("family") == family, f"artifact family mismatch ({family})")
            relative = item.get("path")
            target = audit.safe_child(manifest_path.parent, relative, f"{family}: {relative}")
            require(target.is_file(), f"missing artifact ({family}: {relative})")
            size = item.get("bytes")
            digest = item.get("sha256")
            require(type(size) is int and size > 0, f"invalid artifact byte count ({family}: {relative})")
            require(
                isinstance(digest, str) and audit.DIGEST_PATTERN.fullmatch(digest) is not None,
                f"invalid artifact digest ({family}: {relative})",
            )
            payload = target.read_bytes()
            require(audit.sha256_bytes(payload) == digest, f"artifact SHA-256 mismatch ({family}: {relative})")
            require(len(payload) == size, f"artifact byte count mismatch ({family}: {relative})")
            registered.add(f"{family}/{relative}")
    require(bool(registered), f"baseline {baseline_name} registers no artifacts")
    return registered


def parse_claims(record: dict) -> tuple[list[ReceiptClaim], list[ClaimFailure]]:
    """Split one receipt's rows into parsed claims and fail-closed unparsable claims."""
    claims: list[ReceiptClaim] = []
    failures: list[ClaimFailure] = []
    rows = record.get("rows")
    if not isinstance(rows, list) or not rows:
        failures.append(
            ClaimFailure(str(record.get("id", "?")), "<none>", "<none>", "receipt", "receipt records no claims")
        )
        return claims, failures
    for row in rows:
        claim_id = row.get("id") if isinstance(row, dict) else None
        label = str(claim_id) if isinstance(claim_id, str) and claim_id else "<unknown claim>"
        comparisons = row.get("comparisons") if isinstance(row, dict) else None
        parsed: list[dict] = []
        well_formed = isinstance(row, dict) and isinstance(claim_id, str) and bool(claim_id)
        well_formed = well_formed and isinstance(comparisons, list) and bool(comparisons)
        if well_formed:
            for comparison in comparisons:
                if not isinstance(comparison, dict):
                    well_formed = False
                    break
                artifact = comparison.get("artifact")
                test_source = comparison.get("testSource")
                if not (isinstance(artifact, str) and ARTIFACT_PATTERN.fullmatch(artifact)):
                    well_formed = False
                    break
                if not (isinstance(test_source, str) and test_source.endswith(".cpp")):
                    well_formed = False
                    break
                parsed.append({"artifact": artifact, "testSource": test_source})
        discriminating = row.get("discriminatingCase") if isinstance(row, dict) else None
        if well_formed and discriminating is not None:
            well_formed = isinstance(discriminating, dict) and isinstance(
                discriminating.get("testSource"), str
            ) and discriminating["testSource"].endswith(".cpp")
        if not well_formed:
            failures.append(
                ClaimFailure(label, "<unparsable>", "<unparsable>", "claim", "unparsable claim: fail closed")
            )
            continue
        claims.append(ReceiptClaim(claim_id, parsed, discriminating))
    return claims, failures


def resolve_source(repository_root: Path, relative: str) -> Path:
    path = Path(relative)
    if path.is_absolute() or ".." in path.parts:
        raise BoundaryError(f"invalid test source path: {relative}")
    target = (repository_root / path).resolve()
    if not target.is_relative_to(repository_root.resolve()):
        raise BoundaryError(f"test source escapes the repository: {relative}")
    require(target.is_file(), f"missing test source: {relative}")
    return target


def source_closure(repository_root: Path, source: Path, seen: set[Path] | None = None, depth: int = 0) -> list[str]:
    """The cited source plus the helper files it quoted-includes under tests/ (depth-bounded)."""
    seen = seen if seen is not None else set()
    if source in seen or depth >= INCLUDE_DEPTH_LIMIT:
        return []
    seen.add(source)
    try:
        code = source.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise BoundaryError(f"cannot read test source {source}: {error}") from error
    closure = [code]
    tests_root = (repository_root / TEST_INCLUDE_ROOT).resolve()
    for include in quoted_includes(code):
        include_path = Path(include)
        if include_path.is_absolute() or ".." in include_path.parts:
            continue
        resolved = (tests_root / include_path).resolve()
        if not resolved.is_relative_to(tests_root) or not resolved.is_file():
            continue
        closure.extend(source_closure(repository_root, resolved, seen, depth + 1))
    return closure


def check_claim(
    claim: ReceiptClaim,
    repository_root: Path,
    registered: set[str],
    baseline: str,
    matcher: SourceMatcher,
) -> list[ClaimFailure]:
    failures: list[ClaimFailure] = []
    for artifact in claim.artifacts:
        if artifact not in registered:
            failures.append(
                ClaimFailure(
                    claim.claim_id,
                    artifact,
                    "<registered set>",
                    "comparison",
                    "artifact is not a registered evidence artifact of the selected baseline",
                )
            )
    blobs_cache: dict[str, list[str]] = {}

    def referenced(test_source: str, artifact: str) -> bool:
        if test_source not in blobs_cache:
            path = resolve_source(repository_root, test_source)
            blobs_cache[test_source] = source_closure(repository_root, path)
        return matcher.references(baseline, blobs_cache[test_source], artifact)

    for comparison in claim.comparisons:
        artifact = comparison["artifact"]
        test_source = comparison["testSource"]
        if artifact not in registered:
            continue
        try:
            matched = referenced(test_source, artifact)
        except BoundaryError as error:
            failures.append(ClaimFailure(claim.claim_id, artifact, test_source, "comparison", str(error)))
            continue
        if not matched:
            failures.append(
                ClaimFailure(
                    claim.claim_id,
                    artifact,
                    test_source,
                    "comparison",
                    "test source never references the registered artifact",
                )
            )
    if claim.discriminating is not None:
        test_source = claim.discriminating["testSource"]
        anchored = False
        errors: list[str] = []
        for artifact in claim.artifacts:
            if artifact not in registered:
                continue
            try:
                if referenced(test_source, artifact):
                    anchored = True
                    break
            except BoundaryError as error:
                errors.append(str(error))
        if not anchored:
            reason = (
                "; ".join(errors)
                if errors
                else "discriminating case references none of the claim's registered artifacts"
            )
            failures.append(
                ClaimFailure(
                    claim.claim_id,
                    ", ".join(claim.artifacts),
                    test_source,
                    "discriminating case",
                    reason,
                )
            )
    return failures


def check_receipts(
    deliveries: dict,
    fixture_root: Path,
    repository_root: Path,
    baseline: str,
    matcher: SourceMatcher,
    receipt_filter: str | None = None,
) -> tuple[list[tuple[dict, list[ClaimFailure]]], int, int]:
    registered = registered_artifacts(fixture_root, baseline)
    results: list[tuple[dict, list[ClaimFailure]]] = []
    claim_count = 0
    comparison_count = 0
    matched = 0
    for record in deliveries["records"]:
        record_id = record.get("id") if isinstance(record, dict) else None
        ticket = record.get("ticket") if isinstance(record, dict) else None
        if receipt_filter is not None and receipt_filter not in (record_id, ticket):
            continue
        matched += 1
        claims, failures = parse_claims(record if isinstance(record, dict) else {})
        claim_count += len(claims)
        for claim in claims:
            comparison_count += len(claim.comparisons)
            failures.extend(check_claim(claim, repository_root, registered, baseline, matcher))
        results.append((record if isinstance(record, dict) else {}, failures))
    if receipt_filter is not None and matched == 0:
        raise BoundaryError(f"unknown receipt {receipt_filter!r}; no delivery record or ticket matches")
    return results, claim_count, comparison_count


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", default=DEFAULT_BASELINE, help="named baseline to check against")
    parser.add_argument(
        "--receipt",
        help="check only the delivery record with this id or ticket (repeatable is not supported)",
    )
    parser.add_argument(
        "--deliveries",
        type=Path,
        help="deliveries document (default: <repo>/docs/research/<baseline>/1058-deliveries.json)",
    )
    parser.add_argument(
        "--fixture-root",
        type=Path,
        help="evidence fixture root (default: <repo>/fixtures/<baseline>)",
    )
    parser.add_argument("--repo-root", type=Path, help="repository root (default: this checkout)")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repository_root = (args.repo_root or ROOT).resolve()
    fixture_root = (args.fixture_root or repository_root / "fixtures" / args.baseline).resolve()
    deliveries = (
        args.deliveries
        or repository_root / "docs" / "research" / args.baseline / "1058-deliveries.json"
    ).resolve()
    try:
        document = load_deliveries(deliveries, args.baseline)
        matcher = StaticNamePathMatcher()
        results, claim_count, comparison_count = check_receipts(
            document, fixture_root, repository_root, args.baseline, matcher, args.receipt
        )
        failed = 0
        for record, failures in results:
            record_id = record.get("id", "<unknown receipt>")
            if failures:
                failed += 1
                print(f"FAIL {record_id} ({len(failures)} unmatched)")
                for failure in failures:
                    print(f"  {failure.describe()}")
            else:
                print(f"PASS {record_id}")
        receipt_word = "receipt" if len(results) == 1 else "receipts"
        print(
            f"checked {len(results)} {receipt_word}, {claim_count} claims, {comparison_count} comparisons; "
            f"baseline {args.baseline}"
        )
        return 1 if failed else 0
    except (BoundaryError, audit.EvidenceError, OSError, KeyError, TypeError, ValueError) as error:
        print(f"receipt boundary error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
