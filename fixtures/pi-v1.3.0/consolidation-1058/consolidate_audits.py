#!/usr/bin/env python3
"""#1058: build and check the single pi-v1.3.0 consolidated capability ledger.

The eleven audit ledgers keep their own committed bytes. This tool folds their rows into one
canonical ledger with stable package-qualified ids and an explicit alias for every recorded source
row, applies the provenance-corrections index (pointer spellings only, never observed values),
resolves the named overlaps, and routes every row's evidence to a real registered artifact scenario
or to an honest missing-observation statement. `build` is deterministic; `check` rebuilds in memory,
compares bytes and runs the semantic validations, so the committed ledger cannot silently drift from
its sources, the bundles or the target owner graph. Delivery records additionally bind whole-row
comparisons to pinned C++ inputs and actual CTest results, while preserving source audit status.

A correction rewrites a pointer only in the rows its `appliesTo` scope names and only when the
recorded pointer matches exactly (the same full pointer, or the same symbol list on a path-tail
shorthand). A longer already-qualified pointer, a different symbol list on the same path, and a row
outside the scope keep the lane's recorded provenance, and `appliedCorrections` names only the
corrections that actually rewrote a pointer.

Usage:
    python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py build
    python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py check [--pi-root <frozen>]
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import re
import sys
import xml.etree.ElementTree as ET
from collections.abc import Iterable
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts/evidence"))
import audit_evidence as audit

DELIVERIES_PATH = ROOT / "docs/research/pi-v1.3.0/1058-deliveries.json"
PLAN_PATH = ROOT / "fixtures/pi-v1.3.0/consolidation-1058/consolidation-plan.json"
LEDGER_PATH = ROOT / "docs/research/pi-v1.3.0/1058-consolidated-capability-ledger.json"
FRONTIER_PATH = ROOT / "docs/research/pi-v1.3.0/1058-implementation-frontier.json"
REPORT_PATH = ROOT / "docs/research/pi-v1.3.0/1058-consolidation-frontier.md"
REGISTRY_PATH = ROOT / "fixtures/pi-v1.3.0/baselines.json"
LEDGER_SCHEMA = "pike/pi-v1.3.0-consolidated-capability-ledger/1"
FRONTIER_SCHEMA = "pike/pi-v1.3.0-implementation-frontier/1"
REVISION = "71023aec1546d6d677a65669c5665c0927ce0dd0"

OWNERS = [
    "cch_ai",
    "cch_agent_core",
    "cch_tui",
    "cch_coding_agent",
    "cch_support",
    "cch_durable",
    "cch_mcp",
    "cch_codemode",
    "cch_chord",
]
MEMBERSHIP = ("supported", "excluded", "no-decision")
IMPLEMENTATION = ("implemented", "partial", "missing", "not-applicable")
VERIFICATION = ("verified", "upstream-captured", "unverified", "not-applicable")
ROUTES = (
    "captured",
    "artifact-only",
    "related-captured",
    "declared-comparison",
    "cpp-regression-only",
    "unobserved",
    "unobserved-static",
    "historical-v1.0.4",
    "not-audited",
    "not-applicable",
    "excluded",
)
DRAFT_KINDS = ("prefactor", "behavior")


class ConsolidationError(Exception):
    """A consolidation invariant that must fail closed."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ConsolidationError(message)


def load_json(path: Path):
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def json_bytes(value) -> bytes:
    return (json.dumps(value, indent=2, ensure_ascii=False) + "\n").encode()


def owner_override(plan: dict, lane: str, row_id: str):
    return (plan["owner"]["overrides"].get(lane) or {}).get(row_id)


def resolve_owner(plan: dict, lane: str, raw: dict) -> str:
    override = owner_override(plan, lane, raw["id"])
    if override is not None:
        require(override in OWNERS, f"{lane}:{raw['id']} override owner {override} is not an owner")
        return override
    rule = plan["owner"]["defaults"][lane]
    if rule == "row-owner":
        owner = raw.get("owner")
        require(isinstance(owner, str) and owner, f"{lane}:{raw['id']} records no owner")
        return owner
    if rule == "row-owner-or-cch_coding_agent":
        owner = raw.get("owner")
        return owner if isinstance(owner, str) and owner in OWNERS else "cch_coding_agent"
    if rule == "id-prefix":
        head = raw["id"].split(":", 1)[0]
        mapping = {"agent-core": "cch_agent_core", "coding-agent": "cch_coding_agent"}
        require(head in mapping, f"{lane}:{raw['id']} has no owner mapping for prefix {head}")
        return mapping[head]
    require(rule in OWNERS, f"{lane}: unknown owner default {rule}")
    return rule


def qualify_id(lane: str, raw_id: str, owner: str) -> str:
    if lane == "1047":
        head, rest = raw_id.split(":", 1)
        mapping = {"agent-core": "cch_agent_core", "coding-agent": "cch_coding_agent"}
        return f"{mapping[head]}:{rest}"
    if raw_id.startswith("cch_"):
        _, rest = raw_id.split(":", 1)
        return f"{owner}:{rest}"
    if ":" in raw_id and raw_id.split(":", 1)[0] in OWNERS:
        return raw_id
    return f"{owner}:{raw_id}"


def status_of(lane: str, raw: dict) -> tuple[str, str, str]:
    top = (raw.get("membership"), raw.get("implementation"), raw.get("verification"))
    if all(isinstance(value, str) for value in top):
        membership, implementation, verification = top
    else:
        nested = raw.get("status") or {}
        require(isinstance(nested, dict), f"{lane}:{raw['id']} status is not an object")
        membership = nested.get("membership")
        implementation = nested.get("implementation")
        verification = nested.get("verification")
    require(membership in MEMBERSHIP or membership is None, f"{lane}:{raw['id']} membership {membership!r}")
    require(implementation in IMPLEMENTATION or implementation is None, f"{lane}:{raw['id']} implementation {implementation!r}")
    require(verification in VERIFICATION or verification is None, f"{lane}:{raw['id']} verification {verification!r}")
    return membership, implementation, verification


def string_list(value) -> list[str]:
    if value is None:
        return []
    if isinstance(value, str):
        return [value]
    if isinstance(value, list):
        return [item for item in value if isinstance(item, str) and item]
    return []


def extract_sources(lane: str, raw: dict) -> tuple[list[str], list[str]]:
    """Return (frozen source refs, frozen symbol names recorded without a path)."""
    symbols: list[str] = []
    if lane == "1047":
        sources = string_list(raw.get("piTests"))
        symbols = string_list(raw.get("piSymbols"))
        return sources, symbols
    sources = string_list(raw.get("source"))
    if not sources:
        upstream = raw.get("upstreamEvidence")
        if isinstance(upstream, dict):
            sources = string_list(upstream.get("sources"))
    return sources, symbols


def extract_declared_scenarios(lane: str, raw: dict, plan: dict | None = None) -> list[str]:
    names: list[str] = []
    upstream = raw.get("upstreamEvidence")
    if isinstance(upstream, dict):
        names.extend(string_list(upstream.get("scenarios")))
    evidence = raw.get("evidence")
    if isinstance(evidence, dict) and isinstance(evidence.get("scenario"), str):
        names.append(evidence["scenario"])
    scenario = raw.get("scenario")
    if isinstance(scenario, str) and scenario:
        names.append(scenario)
    if plan is not None:
        for link in plan.get("scenarioLinks", []):
            if link["ticket"] == lane and raw["id"] in link["rows"]:
                names.append(link["scenario"])
    ordered: list[str] = []
    for name in names:
        if name not in ordered:
            ordered.append(name)
    return ordered


def extract_declared_artifacts(lane: str, raw: dict) -> list[str]:
    artifacts: list[str] = []
    evidence = raw.get("evidence")
    if isinstance(evidence, dict) and isinstance(evidence.get("artifact"), str):
        artifacts.append(evidence["artifact"])
    if isinstance(evidence, list):
        artifacts.extend([item for item in evidence if isinstance(item, str)])
    return artifacts


def extract_gaps(lane: str, raw: dict) -> list[str]:
    gaps = string_list(raw.get("gaps")) + string_list(raw.get("blockers"))
    return gaps


def extract_notes(lane: str, raw: dict) -> str:
    parts: list[str] = []
    notes = raw.get("notes")
    if isinstance(notes, str) and notes:
        parts.append(notes)
    recommendation = raw.get("recommendation")
    if isinstance(recommendation, str) and recommendation:
        parts.append(f"Recommendation: {recommendation}")
    python = raw.get("consumer")
    if isinstance(python, dict) and python.get("chainTo"):
        parts.append(f"Consumer chain: {python['chainTo']}")
    return " ".join(parts)


def split_pointer(pointer: str) -> tuple[str, list[str]]:
    """Split a pointer into its path and its comma-separated symbol list."""
    path, separator, symbols = pointer.partition(":")
    return path, [item for item in symbols.split(",") if item] if separator else []


def matches_recorded_pointer(recorded: str, source: str) -> bool:
    """True when `source` carries exactly the recorded pointer spelling.

    A full pointer matches by equality, or by a path-tail shorthand with the identical symbol
    list: #1052 records `extensions/mcp/cli.ts:runMcpCommand` for the full
    `packages/coding-agent/src/...` path. A file-only spelling matches only a pointer that is
    exactly that file, never a longer qualified pointer: rewriting
    `submissions.ts:Submissions.status` to `submissions.ts:Submissions` would drop the observed
    member, and widening `tool-search.ts:createToolSearchTool` would add an export the lane never
    recorded.
    """
    if source == recorded:
        return True
    recorded_path, recorded_symbols = split_pointer(recorded)
    source_path, source_symbols = split_pointer(source)
    return bool(recorded_symbols) and source_symbols == recorded_symbols and source_path.endswith(
        f"/{recorded_path}"
    )


def scope_ids(row_id: str, source_row_ids: Iterable[str]) -> set[str]:
    """The identities a correction's `appliesTo` may name for one consolidated row.

    The plan may scope a correction by the consolidated id or by the lane's own source row id.
    Both are accepted here so the builder and both checker loops share one identity rule; the
    source row ids come from this row's `sourceRows`, so an id belonging to another lane cannot
    match.
    """
    return {row_id, *source_row_ids}


def scope_names(scope: str | list[str], identities: set[str]) -> bool:
    """True when a correction's `appliesTo` scope covers this row."""
    return scope == "*" or bool(identities.intersection(scope))


def apply_corrections(
    lane: str, identities: Iterable[str], sources: list[str], plan: dict
) -> tuple[list[str], list[str]]:
    """Apply pointer-spelling corrections; return (corrected sources, applied correction ids).

    A correction replaces the whole recorded pointer with the authoritative spelling, and only for
    the rows its `appliesTo` names (a source row id, a consolidated id, or `*`). The recorded
    pointer must match exactly under `matches_recorded_pointer`; a longer, already-qualified
    pointer and a different symbol on the same path are left as recorded. A pointer that already
    carries the authoritative spelling is left as is and the correction is not marked applied.
    """
    applied: list[str] = []
    result = list(sources)
    row_identities = set(identities)
    for correction in plan["corrections"]:
        if correction["ticket"] != lane or correction["kind"] == "record-only":
            continue
        if not scope_names(correction["appliesTo"], row_identities):
            continue
        changed = False
        for index, source in enumerate(result):
            if not matches_recorded_pointer(correction["recorded"], source):
                continue
            if source != correction["actual"]:
                result[index] = correction["actual"]
                changed = True
        if changed:
            applied.append(correction["id"])
    return result, applied


def resolve_consumer_owners(plan: dict, lane: str, raw: dict, owner: str) -> list[str]:
    override = (plan["consumer"]["overrides"].get(lane) or {}).get(raw["id"])
    if override is not None:
        return list(override)
    texts: list[str] = []
    consumer = raw.get("consumer")
    if isinstance(consumer, str):
        texts.append(consumer)
    elif isinstance(consumer, dict):
        texts.extend([value for value in consumer.values() if isinstance(value, str)])
    python = raw.get("cpp")
    if isinstance(python, dict):
        for key in ("consumer", "interface"):
            if isinstance(python.get(key), str):
                texts.append(python[key])
    blob = " ".join(texts)
    lowered = blob.lower()
    rule = plan["consumer"]["defaults"][lane]
    if rule in OWNERS:
        return [rule]
    if any(marker in lowered for marker in plan["consumer"]["noneMarkers"]) and owner not in blob:
        return []
    ordered: list[str] = [owner]
    mentioned = [token for token in OWNERS if token in blob]
    for token in mentioned:
        if token not in ordered:
            ordered.append(token)
    path_owners = [
        ("src/coding_agent", "cch_coding_agent"),
        ("src/tui", "cch_tui"),
        ("src/ai", "cch_ai"),
        ("src/agent", "cch_agent_core"),
        ("src/support", "cch_support"),
    ]
    for prefix, path_owner in path_owners:
        if prefix in blob and path_owner not in ordered:
            ordered.append(path_owner)
    if rule == "scan-then-row-owner" and lane == "1050":
        if any(keyword in lowered for keyword in plan["consumer"]["productKeywords"]):
            if "cch_coding_agent" not in ordered:
                ordered.append("cch_coding_agent")
    return ordered


def consumer_text(raw: dict) -> str:
    consumer = raw.get("consumer")
    if isinstance(consumer, str):
        return consumer
    if isinstance(consumer, dict):
        path = consumer.get("path", "")
        chain = consumer.get("chainTo", "")
        return " ".join(part for part in (path, chain) if part)
    return ""


def consumer_text_override(plan: dict, lane: str, raw_id: str, default: str) -> str:
    return ((plan["consumer"].get("textOverrides") or {}).get(lane) or {}).get(raw_id, default)


def boundary_override(plan: dict, lane: str, raw_id: str) -> dict | None:
    for item in plan.get("boundaryOverrides") or []:
        if item["ticket"] == lane and item["rowId"] == raw_id:
            return item
    return None


def registered_families() -> set[str]:
    """The families the named-baseline registry accepts as in-scope evidence."""
    registry = load_json(REGISTRY_PATH)
    return set(registry["baselines"]["pi-v1.3.0"]["bundles"])


def build_bundle_index(families: set[str] | None = None) -> dict[str, dict[str, list[str]]]:
    """Index the artifact scenarios of the in-scope families.

    A lane may commit its `audit-<ticket>/` bundle directory before the serial merger registers
    the family, so the registry decides which families are in scope. `families` defaults to the
    registered families; pass an explicit set to index a different scope. A family whose manifest
    is on disk but not registered stays out of the index: its bytes stay untouched and its
    scenarios are not part of this consolidation.
    """
    if families is None:
        families = registered_families()
    index: dict[str, dict[str, list[str]]] = {}
    for manifest_path in sorted((ROOT / "fixtures/pi-v1.3.0").glob("audit-*/manifest.json")):
        manifest = load_json(manifest_path)
        family = manifest["family"]
        if family not in families:
            continue
        index[family] = {}
        for artifact in manifest["artifacts"]:
            index[family][artifact["path"]] = [scenario["name"] for scenario in artifact["scenarios"]]
    return index


def on_disk_families() -> set[str]:
    """The families whose manifest is committed on disk, registered or not."""
    families: set[str] = set()
    for manifest_path in sorted((ROOT / "fixtures/pi-v1.3.0").glob("audit-*/manifest.json")):
        families.add(load_json(manifest_path)["family"])
    return families


def build_scenario_index(bundles: dict[str, dict[str, list[str]]]) -> dict[str, list[tuple[str, str]]]:
    index: dict[str, list[tuple[str, str]]] = {}
    for family, artifacts in bundles.items():
        for artifact, scenarios in artifacts.items():
            for scenario in scenarios:
                index.setdefault(scenario, []).append((family, artifact))
    return index


def family_for_artifact(path: str, bundles: dict[str, dict[str, list[str]]]) -> tuple[str, str] | None:
    match = re.search(r"fixtures/pi-v1\.3\.0/(audit-\d+)/([^/]+)$", path)
    if not match:
        return None
    family, artifact = match.group(1), match.group(2)
    if family in bundles and artifact in bundles[family]:
        return family, artifact
    return None


def resolve_evidence(
    lane: str,
    raw: dict,
    membership: str,
    verification: str,
    bundles: dict[str, dict[str, list[str]]],
    scenario_index: dict[str, list[tuple[str, str]]],
    plan: dict,
) -> dict:
    declared_scenarios = extract_declared_scenarios(lane, raw, plan)
    declared_artifacts = extract_declared_artifacts(lane, raw)
    evidence = raw.get("evidence")
    result = evidence.get("result") if isinstance(evidence, dict) else None
    route = "unobserved"
    artifact = None
    captured_scenarios: list[str] = []
    scenario_match = None
    if membership == "excluded":
        if verification == "upstream-captured":
            route, artifact, captured_scenarios, scenario_match = match_artifact(
                declared_artifacts, declared_scenarios, bundles, scenario_index
            )
            if route != "captured":
                raise ConsolidationError(
                    f"{lane}:{raw['id']}: excluded upstream-captured row does not resolve a registered scenario"
                )
        else:
            route = "excluded"
    elif lane == "1050":
        comparison = raw.get("comparisonRoute")
        if comparison == "not-applicable":
            route = "not-applicable"
        elif comparison == "not-audited":
            route = "not-audited"
        elif isinstance(evidence, dict) and evidence.get("historicalBaseline"):
            route = "historical-v1.0.4"
            artifact = evidence.get("artifact")
        else:
            route, artifact, captured_scenarios, scenario_match = match_artifact(
                declared_artifacts, declared_scenarios, bundles, scenario_index
            )
    else:
        route, artifact, captured_scenarios, scenario_match = match_artifact(
            declared_artifacts, declared_scenarios, bundles, scenario_index
        )
    if route in ("declared-comparison", "unobserved") and isinstance(result, str):
        lowered = result.lower()
        if "c++" in lowered and "test" in lowered:
            route = "cpp-regression-only"
        elif "static" in lowered:
            route = "unobserved-static"
    if route in ("captured", "artifact-only") and verification != "upstream-captured":
        # The declared comparison scenario resolves to a real registered observation, but this row
        # keeps its own recorded verification: a cross-reference is not status inflation.
        route = "related-captured"
    missing = None
    if route in (
        "related-captured",
        "declared-comparison",
        "unobserved",
        "unobserved-static",
        "cpp-regression-only",
        "historical-v1.0.4",
        "not-audited",
    ):
        missing = {
            "related-captured": "declared comparison scenario resolves to a registered artifact, but this row records its own status; no cross-row promotion",
            "declared-comparison": "recorded comparison scenario has no captured artifact pairing; C++ comparison not run",
            "unobserved": "no frozen capture recorded for this row; source-only audit",
            "unobserved-static": "no frozen capture recorded for this row; the recorded evidence is a static source/consumer trace",
            "cpp-regression-only": f"no frozen comparison; existing C++ regression check only: {result}",
            "historical-v1.0.4": "only historical pi-v1.0.4 evidence; no v1.3.0 capture",
            "not-audited": "not individually audited against v1.3.0; comparison route is the changed-source diff",
        }[route]
    return {
        "route": route,
        "artifact": artifact,
        "scenarioMatch": scenario_match,
        "capturedScenarios": captured_scenarios,
        "declaredScenarios": declared_scenarios,
        "result": result if isinstance(result, str) else None,
        "missingObservation": missing,
    }


def match_artifact(
    declared_artifacts: list[str],
    declared_scenarios: list[str],
    bundles: dict[str, dict[str, list[str]]],
    scenario_index: dict[str, list[tuple[str, str]]],
):
    """Pair a row with a real registered artifact; direct scenario pairing when possible."""
    resolved = []
    for path in declared_artifacts:
        found = family_for_artifact(path, bundles)
        if found:
            resolved.append(found)
    for family, artifact in resolved:
        for scenario in declared_scenarios:
            if scenario in bundles[family][artifact]:
                return "captured", f"fixtures/pi-v1.3.0/{family}/{artifact}", [scenario], "direct"
    for scenario in declared_scenarios:
        matches = scenario_index.get(scenario) or []
        if matches:
            family, artifact = matches[0]
            return "captured", f"fixtures/pi-v1.3.0/{family}/{artifact}", [scenario], "direct-global"
    if resolved:
        family, artifact = resolved[0]
        return "artifact-only", f"fixtures/pi-v1.3.0/{family}/{artifact}", [], None
    if declared_scenarios:
        return "declared-comparison", None, [], None
    return "unobserved", None, [], None


def attach_gap_register(plan: dict, ledger_rows: list[dict]) -> list[dict]:
    register: list[dict] = []
    for lane in ("1047", "1048"):
        source = next(item for item in plan["sources"] if item["ticket"] == lane)
        ledger = load_json(ROOT / source["ledger"])
        records = []
        for key in ("gaps", "ownershipGaps", "excluded", "consumerChains"):
            value = ledger.get(key)
            if value:
                records.append({"register": key, "records": value})
        attached_index: dict[str, list[str]] = {}
        if lane == "1047":
            for entry in ledger.get("gaps") or []:
                attached: list[str] = []
                for row in ledger_rows:
                    if row["lane"] != lane:
                        continue
                    if gap_matches(entry, row["_raw"]):
                        attached.append(row["id"])
                attached_index[entry["id"]] = attached
        register.append({"ticket": lane, "registers": records, "attachedRows": attached_index})
    return register


def gap_matches(gap: dict, raw: dict) -> bool:
    needles = string_list(gap.get("evidence"))
    if not needles:
        return False
    targets = []
    for target in raw.get("cchTargets") or []:
        if isinstance(target, dict) and isinstance(target.get("path"), str):
            targets.append(target["path"])
    scenarios = extract_declared_scenarios("1047", raw)
    for needle in needles:
        for target in targets:
            if needle.startswith(target):
                return True
        for scenario in scenarios:
            if needle.endswith(scenario):
                return True
    return False


def pinned_delivery_file(record: dict) -> Path:
    path = audit.resolve_repository_file(ROOT, record.get("path"), "delivery evidence")
    require(audit.sha256_file(path) == record.get("sha256"), f"delivery digest mismatch: {record['path']}")
    return path


def passed_delivery_tests(validation: dict) -> set[str]:
    require(isinstance(validation.get("command"), list) and validation["command"], "missing validation command")
    require(re.fullmatch(r"[0-9a-f]{40}", validation.get("testedRevision", "")) is not None,
            "validation requires a full C++ revision")
    audit.validate_timestamp(validation.get("recordedAt"), "C++ validation")
    suite = ET.parse(pinned_delivery_file(validation["junit"])).getroot()
    require(suite.tag == "testsuite", "delivery result is not a CTest JUnit testsuite")
    cases = suite.findall("testcase")
    require(type(validation.get("selected")) is int and validation["selected"] > 0,
            "delivery selection is empty")
    require(len(cases) == validation["selected"] == int(suite.get("tests", "-1")),
            "delivery selected count differs from JUnit cases")
    names: set[str] = set()
    for case in cases:
        name = case.get("name")
        require(name and name not in names, "delivery has an unnamed or duplicate test")
        require(case.get("status") == "run" and all(case.find(tag) is None for tag in ("failure", "error", "skipped")),
                f"delivery test did not pass: {name}")
        names.add(name)
    require(all(int(suite.get(key, "0")) == 0 for key in ("failures", "errors", "disabled", "skipped")),
            "delivery suite contains failed or skipped cases")
    return names


def delivery_routing_digest(rows: list[dict], claims: list[dict], artifacts: dict[str, bytes]) -> str:
    """Hash each claimed row's pre-delivery scenarios and their registered artifact identities."""
    by_id = {row["id"]: row for row in rows}
    scenarios = {
        path: {item["name"] for item in json.loads(data)["scenarios"]}
        for path, data in artifacts.items()
    }
    routing = []
    for claim in sorted(claims, key=lambda item: item["id"]):
        row = by_id[claim["id"]]
        evidence = row.get("auditStatus", {}).get("evidence", row.get("evidence", {}))
        preferred = (evidence.get("artifact") or "").removeprefix("fixtures/pi-v1.3.0/")
        mappings = []
        for scenario in sorted(set(row["comparisonScenarios"])):
            matches = sorted(path for path, names in scenarios.items() if scenario in names)
            require(matches, f"{row['id']}: scenario has no registered artifact: {scenario}")
            path = preferred if preferred in matches else matches[0]
            mappings.append({"scenario": scenario, "artifact": path,
                             "sha256": hashlib.sha256(artifacts[path]).hexdigest()})
        routing.append({"row": row["id"], "scenarios": mappings})
    return hashlib.sha256(json.dumps(routing, sort_keys=True, ensure_ascii=False,
                                     separators=(",", ":")).encode("utf-8")).hexdigest()


def delivery_updates(rows: list[dict], deliveries: dict) -> dict[str, dict]:
    """Accept explicit whole-row comparisons; audit bytes and membership stay unchanged.

    Digests bind the selected implementation, test, fixture and JUnit files. Scenario-to-test
    mappings and the completeness of those selected inputs remain review obligations; a green
    JUnit result alone cannot establish them. No command stored in a receipt is executed here.
    """
    require(deliveries.get("schema") == "pike/pi-v1.3.0-capability-deliveries/1", "delivery schema mismatch")
    require(deliveries.get("baseline") == "pi-v1.3.0" and deliveries.get("revision") == REVISION,
            "delivery baseline/revision mismatch")
    records = deliveries.get("records")
    require(isinstance(records, list), "delivery records must be a list")
    if not records:
        return {}
    baseline, artifacts, _ = audit.verify(ROOT / "fixtures/pi-v1.3.0", "pi-v1.3.0")
    require(baseline["revision"] == REVISION, "delivery registry revision mismatch")
    by_id = {row["id"]: row for row in rows}
    record_ids: set[str] = set()
    updates: dict[str, dict] = {}
    for record in records:
        identity = record.get("id")
        require(isinstance(identity, str) and identity and identity not in record_ids, "duplicate or missing delivery id")
        record_ids.add(identity)
        require(re.fullmatch(r"[0-9]+", record.get("ticket", "")) is not None, "delivery requires a ticket")
        commits = record.get("implementationCommits")
        require(isinstance(commits, list) and commits and all(
            isinstance(commit, str) and re.fullmatch(r"[0-9a-f]{40}", commit) for commit in commits),
            f"{identity}: missing full implementation commits")
        audit.resolve_repository_file(ROOT, record.get("report"), "delivery report")
        inputs = record.get("inputs")
        require(isinstance(inputs, list) and inputs, f"{identity}: missing pinned inputs")
        paths: set[str] = set()
        for entry in inputs:
            pinned_delivery_file(entry)
            require(entry["path"] not in paths, f"{identity}: duplicate pinned input")
            paths.add(entry["path"])
        require(any(path.startswith("src/") for path in paths), f"{identity}: no production input")
        passed = passed_delivery_tests(record["validation"])
        claims = record.get("rows")
        require(isinstance(claims, list) and claims, f"{identity}: delivery covers no rows")
        for claim in claims:
            row_id = claim["id"]
            require(row_id in by_id and row_id not in updates, f"{identity}: unknown or duplicate delivered row {row_id}")
            row = by_id[row_id]
            require(row["membership"] == "supported", f"{row_id}: delivery cannot change membership")
            require(claim.get("remainingBoundary") == [], f"{row_id}: incomplete delivery cannot verify a whole row")
            comparisons = claim.get("comparisons")
            require(isinstance(comparisons, list) and comparisons, f"{row_id}: missing comparisons")
            obligations: set[str] = set()
            for comparison in comparisons:
                scenario = comparison["scenario"]
                require(scenario not in obligations, f"{row_id}: duplicate comparison scenario")
                obligations.add(scenario)
                artifact = comparison["artifact"]
                require(artifact in artifacts, f"{row_id}: comparison artifact is not registered: {artifact}")
                observed = json.loads(artifacts[artifact])["scenarios"]
                require(any(item["name"] == scenario for item in observed), f"{row_id}: comparison scenario is missing")
                require(comparison["test"] in passed, f"{row_id}: comparison test did not run")
                source = comparison["testSource"]
                require(source in paths and source.startswith("tests/"), f"{row_id}: comparison source is not pinned")
            require(set(row["comparisonScenarios"]) == obligations,
                    f"{row_id}: comparisons do not cover exactly the row's audit obligations")
            control = claim.get("discriminatingCase") or {}
            require(control.get("test") in passed and control.get("reason", "").strip(),
                    f"{row_id}: missing passing discriminating case")
            require(control.get("testSource") in paths and control["testSource"].startswith("tests/"),
                    f"{row_id}: discriminating source is not pinned")
            first = comparisons[0]
            updates[row_id] = {
                "delivery": identity,
                "implementation_ticket": f"#{record['ticket']}",
                "implementation": "implemented",
                "verification": "verified",
                "evidence": {
                    "route": "captured",
                    "artifact": f"fixtures/pi-v1.3.0/{first['artifact']}",
                    "scenarioMatch": "direct",
                    "capturedScenarios": [item["scenario"] for item in comparisons if item["artifact"] == first["artifact"]],
                    "result": f"C++ comparisons passed; see {record['report']}",
                },
                "missingObservation": None,
                "gaps": [],
                "remainingWork": None,
                "notes": f"Delivered by #{record['ticket']}; whole-row comparisons and remaining boundaries: {record['report']}",
            }
        require(record.get("routingSha256") == delivery_routing_digest(rows, claims, artifacts),
                f"{identity}: delivery routing digest mismatch")
    return updates


def build_ledger(
    plan: dict, bundles: dict[str, dict[str, list[str]]], scenario_index: dict[str, list[tuple[str, str]]]
) -> dict:
    registry = load_json(REGISTRY_PATH)
    registered = set(registry["baselines"]["pi-v1.3.0"]["bundles"])
    # The registry is the authority for which families are in scope: it names the families whose
    # evidence the serial merger has accepted. A lane may commit its bundle directory before the
    # merger registers it, so a manifest on disk that is not registered is out of scope, not a
    # disagreement. The reverse stays fatal: a registered family without its manifest on disk
    # would leave a row's evidence pointing at bytes nobody can read.
    missing = registered - set(bundles)
    require(not missing, f"registered bundle manifests are missing on disk: {sorted(missing)}")

    raw_rows: list[dict] = []
    source_aliases: dict[str, dict[str, str]] = {}
    for source in plan["sources"]:
        lane = source["ticket"]
        ledger = load_json(ROOT / source["ledger"])
        require(ledger.get("ticket") == lane or ledger.get("ticket") is None, f"{lane} ledger ticket mismatch")
        source_aliases[lane] = {}
        for raw in ledger["rows"]:
            owner = resolve_owner(plan, lane, raw)
            consolidated_id = qualify_id(lane, raw["id"], owner)
            require(
                raw["id"] not in source_aliases[lane],
                f"{lane} source row {raw['id']} appears twice",
            )
            source_aliases[lane][raw["id"]] = consolidated_id
            membership, implementation, verification = status_of(lane, raw)
            sources, frozen_symbols = extract_sources(lane, raw)
            sources, applied = apply_corrections(
                lane, scope_ids(consolidated_id, [raw["id"]]), sources, plan
            )
            raw_rows.append(
                {
                    "id": consolidated_id,
                    "lane": lane,
                    "_raw": raw,
                    "owner": owner,
                    "membership": membership,
                    "implementation": implementation,
                    "verification": verification,
                    "sources": sources,
                    "frozenSymbols": frozen_symbols,
                    "appliedCorrections": applied,
                }
            )

    by_id: dict[str, list[dict]] = {}
    for row in raw_rows:
        by_id.setdefault(row["id"], []).append(row)

    rows: list[dict] = []
    for consolidated_id in sorted(by_id):
        entries = by_id[consolidated_id]
        primary = entries[0]
        raw = primary["_raw"]
        lane = primary["lane"]
        evidence = resolve_evidence(
            lane, raw, primary["membership"], primary["verification"], bundles, scenario_index, plan
        )
        membership, implementation, verification = primary["membership"], primary["implementation"], primary["verification"]
        for entry in entries[1:]:
            if entry["membership"] == "excluded":
                membership, implementation, verification = entry["membership"], entry["implementation"], entry["verification"]
        gaps = extract_gaps(lane, raw)
        notes = extract_notes(lane, raw)
        implementation_ticket = raw.get("implementation_ticket")
        if implementation_ticket in ("unfiled implementation probe", ""):
            implementation_ticket = None
        capability = raw.get("capability") or raw.get("operation") or display_capability(primary["id"])
        consumer_owners = resolve_consumer_owners(plan, lane, raw, primary["owner"])
        boundary_item = boundary_override(plan, lane, raw["id"])
        source_rows = [
            {
                "ticket": entry["lane"],
                "rowId": entry["_raw"]["id"],
                "membership": entry["membership"],
                "implementation": entry["implementation"],
                "verification": entry["verification"],
                "owner": entry["owner"],
            }
            for entry in entries
        ]
        row = {
            "id": consolidated_id,
            "capability": capability,
            "source": primary["sources"],
            "frozenSymbols": primary["frozenSymbols"],
            "membership": membership,
            "implementation": implementation,
            "verification": verification,
            "owner": primary["owner"],
            "consumer": consumer_text_override(plan, lane, raw["id"], consumer_text(raw)) or None,
            "consumerOwners": consumer_owners,
            "boundary": (
                boundary_item["boundary"]
                if boundary_item is not None
                else (raw.get("boundary") or (raw.get("cppEvidence") if membership == "excluded" else None))
            ),
            "implementation_ticket": implementation_ticket,
            "scenario": evidence["declaredScenarios"][0] if evidence["declaredScenarios"] else None,
            "comparisonScenarios": evidence["declaredScenarios"],
            "evidence": {
                "route": evidence["route"],
                "artifact": evidence["artifact"],
                "scenarioMatch": evidence["scenarioMatch"],
                "capturedScenarios": evidence["capturedScenarios"],
                "result": evidence["result"],
            },
            "missingObservation": evidence["missingObservation"],
            "gaps": gaps,
            "remainingWork": remaining_work(
                membership, implementation, evidence["route"], gaps, evidence["missingObservation"], notes
            ),
            "notes": notes,
            "lane": primary["lane"],
            "frozenSymbolsSource": "1047 piSymbols" if primary["frozenSymbols"] else None,
            "appliedCorrections": sorted({item for entry in entries for item in entry["appliedCorrections"]}),
            "sourceRows": source_rows,
        }
        rows.append(row)

    deliveries = load_json(DELIVERIES_PATH)
    updates = delivery_updates(rows, deliveries)
    for row in rows:
        if row["id"] not in updates:
            continue
        update = updates[row["id"]]
        row["auditStatus"] = {key: copy.deepcopy(row[key]) for key in update if key != "delivery"}
        row.update(update)

    register = attach_gap_register(plan, raw_rows)
    counts = count_rows(rows)
    counts["scenarioCoverage"] = scenario_coverage(rows, bundles, plan.get("orphanScenarios", []))
    ledger = {
        "schema": LEDGER_SCHEMA,
        "ticket": "1058",
        "title": "Consolidated pi-v1.3.0 capability ledger",
        "baseline": plan["baseline"],
        "revision": plan["revision"],
        "platform": "linux-x86_64-glibc",
        "method": {
            "rule": "Every audit source row keeps its identity through sourceAliases; membership is unchanged by deliveries. Source status is retained in sourceRows and auditStatus. Only complete, pinned C++ comparison deliveries may verify a supported row; every other included row states remaining work.",
            "sourceRowCount": len(raw_rows),
            "consolidatedRowCount": len(rows),
            "groups": [group["id"] for group in plan["groups"]],
            "overlaps": [overlap["id"] for overlap in plan["overlaps"]],
        },
        "deliveries": deliveries,
        "sources": [
            {
                "ticket": source["ticket"],
                "ledger": source["ledger"],
                "report": source["report"],
                "family": source["family"],
                "schema": source["schema"],
                "rows": sum(1 for row in raw_rows if row["lane"] == source["ticket"]),
            }
            for source in plan["sources"]
        ],
        "schemaMappings": plan["schemaMappings"],
        "counts": counts,
        "sourceAliases": source_aliases,
        "sourceGapRegisters": register,
        "capabilityGroups": [
            {**group, "memberCount": len(group["members"])} for group in plan["groups"]
        ],
        "overlapResolutions": plan["overlaps"],
        "retainedLimitations": plan["retainedLimitations"],
        "orphanScenarios": plan.get("orphanScenarios", []),
        "correctionsIndex": plan["corrections"],
        "rows": rows,
    }
    return ledger


def display_capability(consolidated_id: str) -> str:
    tail = consolidated_id.split(":", 1)[1]
    parts = [part for part in re.split(r"[:.]", tail) if part]
    words: list[str] = []
    for part in parts:
        words.extend(part.replace("-", " ").replace("_", " ").split())
    return " ".join(words) if words else tail


def check_product_routing(rows_by_id: dict[str, dict], overlaps: list[dict]) -> None:
    """The toolkit/product resolution names only real 1051 counterparts and promotes nothing."""
    overlap = next(item for item in overlaps if item["id"] == "overlap-toolkit-product")
    adjacent = overlap.get("adjacentRows") or []
    require(adjacent, "overlap-toolkit-product records no adjacent 1051 counterpart")
    for row_id in adjacent:
        row = rows_by_id.get(row_id)
        require(row is not None, f"overlap-toolkit-product names unknown row {row_id}")
        require(row["lane"] == "1051", f"overlap-toolkit-product names {row_id}, which is not a 1051 row")
        require(row["verification"] != "verified", f"{row_id}: an adjacent counterpart may not be verified")
    routing = overlap.get("rowRouting") or []
    require(
        [item["row"] for item in routing] == overlap["rows"],
        "overlap-toolkit-product routing does not cover its seven rows in order",
    )
    for item in routing:
        row = rows_by_id[item["row"]]
        require(row["lane"] == "1050", f"{item['row']}: routed product row is not a 1050 row")
        require(item["observation"].strip(), f"{item['row']}: no routing observation")
        require(row["evidence"]["route"] == "not-audited", f"{item['row']}: product row left the not-audited route")
        require(row["verification"] == "unverified", f"{item['row']}: product row claims verification")
        require(row["missingObservation"], f"{item['row']}: product row hides its missing observation")
        for adjacent_id in item["adjacent"]:
            require(
                adjacent_id in adjacent,
                f"{item['row']}: adjacent row {adjacent_id} is not a named 1051 counterpart",
            )
    for item in routing:
        # The lane gap is the source statement; it must agree with the routing instead of claiming an
        # audit the 1051 lane does not hold.
        gap_text = " ".join(rows_by_id[item["row"]]["gaps"])
        require(
            "audited by #1051" not in gap_text,
            f"{item['row']}: lane gap still claims #1051 audits the product behavior",
        )
        require(
            "unaudited" in gap_text,
            f"{item['row']}: lane gap does not state that the v1.3.0 product behavior is unaudited",
        )
        for adjacent_id in item["adjacent"]:
            require(
                adjacent_id.split(":")[-1] in gap_text,
                f"{item['row']}: lane gap does not name its adjacent #1051 row {adjacent_id}",
            )
    require(
        any(not item["adjacent"] for item in routing),
        "overlap-toolkit-product must keep unmatched product rows visibly unmatched",
    )


def coverage_note(coverage: dict) -> str:
    return (
        f"scenario coverage: {coverage['scenarios']} scenarios, {coverage['scenarioNamed']} named by a row, "
        f"{coverage['artifactCovered']} covered by a referenced artifact, "
        f"{len(coverage['orphaned'])} explicit orphan(s), {len(coverage['uncovered'])} uncovered"
    )


def scenario_coverage(rows: list[dict], bundles: dict[str, dict[str, list[str]]], orphans: list[dict]) -> dict:
    referenced_artifacts = {row["evidence"]["artifact"] for row in rows if row["evidence"]["artifact"]}
    named = {scenario for row in rows for scenario in row["comparisonScenarios"]}
    orphan_keys = {(item["family"], item["artifact"], item["scenario"]) for item in orphans}
    total = 0
    scenario_named = 0
    artifact_covered = 0
    uncovered: list[str] = []
    for family, artifacts in bundles.items():
        if family == "audit-1046":
            continue
        for artifact, scenarios in artifacts.items():
            path = f"fixtures/pi-v1.3.0/{family}/{artifact}"
            for scenario in scenarios:
                total += 1
                if scenario in named:
                    scenario_named += 1
                elif path in referenced_artifacts:
                    artifact_covered += 1
                elif (family, artifact, scenario) in orphan_keys:
                    continue
                else:
                    uncovered.append(f"{family}/{artifact}:{scenario}")
    return {
        "scenarios": total,
        "scenarioNamed": scenario_named,
        "artifactCovered": artifact_covered,
        "orphaned": [f"{item['family']}/{item['artifact']}:{item['scenario']}" for item in orphans],
        "uncovered": uncovered,
    }


def remaining_work(membership: str, implementation: str, route: str, gaps: list[str], missing: str | None, notes: str) -> str | None:
    """Every included row states what still stands between it and acceptance."""
    if membership == "excluded":
        return None
    if missing:
        return missing
    if route in ("captured", "artifact-only"):
        return "C++ comparison against the registered artifact is not run; the row stays unverified"
    if gaps:
        return " ".join(gaps)
    if notes:
        return notes
    if implementation == "implemented":
        return "implemented in C++, but no C++ comparison is recorded; the row stays unverified"
    return "implementation status recorded without a gap; provenance review required"


def count_rows(rows: list[dict]) -> dict:
    def tally(field: str) -> dict[str, int]:
        result: dict[str, int] = {}
        for row in rows:
            value = row[field] if row[field] is not None else "unrecorded"
            result[value] = result.get(value, 0) + 1
        return dict(sorted(result.items()))

    by_owner_implementation: dict[str, dict[str, int]] = {}
    for row in rows:
        tally_for_owner = by_owner_implementation.setdefault(row["owner"], {})
        tally_for_owner[row["implementation"]] = tally_for_owner.get(row["implementation"], 0) + 1

    return {
        "rows": len(rows),
        "byLane": tally("lane"),
        "byOwner": tally("owner"),
        "byOwnerImplementation": {
            owner: dict(sorted(values.items())) for owner, values in sorted(by_owner_implementation.items())
        },
        "membership": tally("membership"),
        "implementation": tally("implementation"),
        "verification": tally("verification"),
        "evidenceRoute": {route: sum(1 for row in rows if row["evidence"]["route"] == route) for route in ROUTES},
        "unobservedSupported": sum(
            1
            for row in rows
            if row["membership"] == "supported" and row["evidence"]["route"] != "captured"
        ),
        "noDecision": [row["id"] for row in rows if row["membership"] == "no-decision"],
        "excluded": [row["id"] for row in rows if row["membership"] == "excluded"],
        "missingObservationRows": sum(1 for row in rows if row["missingObservation"]),
    }


def check_ledger(
    plan: dict,
    ledger: dict,
    bundles: dict[str, dict[str, list[str]]],
    scenario_index: dict[str, list[tuple[str, str]]],
    pi_root: Path | None,
) -> list[str]:
    notes: list[str] = []
    rows = ledger["rows"]
    require(ledger["schema"] == LEDGER_SCHEMA, "ledger schema mismatch")
    require(ledger["revision"] == REVISION, "ledger revision mismatch")

    # 1. Source coverage: every source row appears once and the alias map is complete.
    source_total = 0
    alias_targets: dict[str, str] = {}
    for source in plan["sources"]:
        lane = source["ticket"]
        raw_ledger = load_json(ROOT / source["ledger"])
        source_total += len(raw_ledger["rows"])
        aliases = ledger["sourceAliases"][lane]
        require(len(aliases) == len(raw_ledger["rows"]), f"{lane}: alias count differs from source rows")
        for raw in raw_ledger["rows"]:
            require(raw["id"] in aliases, f"{lane}: source row {raw['id']} has no alias")
            alias_targets[f"{lane}:{raw['id']}"] = aliases[raw["id"]]
    require(len(alias_targets) == source_total, "alias map is not a bijection over source rows")
    mapped = 0
    for row in rows:
        for entry in row["sourceRows"]:
            mapped += 1
            key = f"{entry['ticket']}:{entry['rowId']}"
            require(key in alias_targets, f"{key} is not in the alias map")
            require(alias_targets[key] == row["id"], f"{key} aliases {alias_targets[key]}, not {row['id']}")
    require(mapped == source_total, f"sourceRows cover {mapped} of {source_total} source rows")
    require(len({row["id"] for row in rows}) == len(rows), "consolidated ids are not unique")
    notes.append(f"source coverage: {mapped} source rows aliased into {len(rows)} consolidated rows")

    updates = delivery_updates(rows, ledger["deliveries"])
    for row in rows:
        require((row["id"] in updates) == ("delivery" in row), f"{row['id']}: delivery association mismatch")
        if row["id"] in updates:
            require(all(row.get(key) == value for key, value in updates[row["id"]].items()),
                    f"{row['id']}: status differs from its validated delivery")
            require(row.get("auditStatus"), f"{row['id']}: missing original audit status")
    notes.append(f"deliveries: {len(updates)} rows backed by pinned C++ comparison results")

    # 2. Vocabularies, ownership and remaining work.
    for row in rows:
        require(row["membership"] in MEMBERSHIP, f"{row['id']}: bad membership")
        require(row["implementation"] in IMPLEMENTATION, f"{row['id']}: bad implementation")
        require(row["verification"] in VERIFICATION, f"{row['id']}: bad verification")
        require(row["owner"] in OWNERS, f"{row['id']}: bad owner {row['owner']}")
        require(row["evidence"]["route"] in ROUTES, f"{row['id']}: bad evidence route")
        if row["membership"] != "excluded":
            require(row["source"], f"{row['id']}: supported/no-decision row has no frozen source")
        for entry in row["source"]:
            for pointer in entry.split("; "):
                require(
                    pointer.count("packages/") <= 1,
                    f"{row['id']}: pointer {pointer} double-qualifies a package path",
                )
        if row["membership"] == "excluded":
            require(row["boundary"], f"{row['id']}: excluded row records no boundary")
            require(
                row["boundary"].strip().casefold() not in ("none", "null"),
                f"{row['id']}: excluded row records a placeholder boundary",
            )
            require(
                row["evidence"]["route"] in ("excluded", "not-applicable", "captured"),
                f"{row['id']}: excluded row has an evidence route",
            )
        else:
            require(row["consumer"] and row["consumer"].strip(), f"{row['id']}: included row has no consumer text")
        if row["verification"] == "verified":
            require(row["id"] in updates, f"{row['id']}: verified without a validated delivery")
            require(row["implementation"] == "implemented" and row["membership"] == "supported",
                    f"{row['id']}: verified requires an implemented supported capability")
            require(not row["missingObservation"] and not row["gaps"], f"{row['id']}: verified with unresolved gaps")
        if row["membership"] == "excluded" or row["verification"] == "verified":
            require(not row["remainingWork"], f"{row['id']}: excluded/verified row records remaining work")
        else:
            require(row["remainingWork"], f"{row['id']}: no remaining work recorded")
        if row["verification"] == "upstream-captured":
            require(
                row["evidence"]["route"] == "captured"
                and row["evidence"]["artifact"]
                and row["evidence"]["capturedScenarios"],
                f"{row['id']}: upstream-captured without a registered artifact/scenario",
            )
        if row["membership"] == "excluded" and row["evidence"]["route"] == "captured":
            require(
                row["verification"] == "upstream-captured",
                f"{row['id']}: excluded row with a captured route must keep upstream-captured verification",
            )
        if row["membership"] == "supported" and row["implementation"] != "implemented":
            require(
                row["gaps"] or row["missingObservation"] or row["notes"],
                f"{row['id']}: supported non-implemented row has no remaining work",
            )
        for owner in row["consumerOwners"]:
            require(owner in OWNERS, f"{row['id']}: bad consumer owner {owner}")
        if not row["consumerOwners"]:
            text = (row["consumer"] or "").lower()
            require(
                any(marker in text for marker in ("none", "unconsumable", "no production", "tests only", "no c++ caller")),
                f"{row['id']}: no consumer owner and no none/unconsumable reason",
            )
        if row["scenario"] is not None:
            require(isinstance(row["scenario"], str) and row["scenario"], f"{row['id']}: empty scenario")
        blob = json.dumps(
            {"membership": row["membership"], "implementation": row["implementation"], "verification": row["verification"], "gaps": row["gaps"]}
        )
        require("Deferred" not in blob and '"deferred"' not in blob, f"{row['id']}: deferred escape hatch")
    notes.append("vocabulary/ownership/remaining-work: checked")

    # 2b. Curated boundary and consumer overrides must be applied exactly.
    rows_by_id = {row["id"]: row for row in rows}
    for item in plan.get("boundaryOverrides") or []:
        alias = ledger["sourceAliases"][item["ticket"]].get(item["rowId"])
        require(alias is not None, f"boundary override names unknown row {item['ticket']}:{item['rowId']}")
        require(rows_by_id[alias]["boundary"] == item["boundary"], f"{alias}: boundary override not applied")
    for lane, overrides in (plan["consumer"].get("textOverrides") or {}).items():
        for raw_id, text in overrides.items():
            alias = ledger["sourceAliases"][lane].get(raw_id)
            require(alias is not None, f"consumer text override names unknown row {lane}:{raw_id}")
            require(rows_by_id[alias]["consumer"] == text, f"{alias}: consumer text override not applied")

    # 3. Evidence routes point at real registered scenarios or an honest missing observation.
    captured = 0
    for row in rows:
        route = row["evidence"]["route"]
        artifact = row["evidence"]["artifact"]
        if route in ("captured", "artifact-only", "related-captured"):
            require(artifact, f"{row['id']}: captured route without artifact")
            found = family_for_artifact(artifact, bundles)
            require(found is not None, f"{row['id']}: artifact {artifact} is not registered")
            family, name = found
            require(bundles[family][name], f"{row['id']}: artifact {artifact} has no scenario")
            captured += 1
            if route == "captured":
                require(
                    row["evidence"]["scenarioMatch"] in ("direct", "direct-global"),
                    f"{row['id']}: captured route without a direct scenario match",
                )
                require(
                    any(scenario in bundles[family][name] for scenario in row["comparisonScenarios"]),
                    f"{row['id']}: captured scenario is not in the artifact",
                )
        if route in (
            "related-captured",
            "declared-comparison",
            "unobserved",
            "unobserved-static",
            "cpp-regression-only",
            "historical-v1.0.4",
            "not-audited",
        ):
            require(row["missingObservation"], f"{row['id']}: {route} without a missing-observation statement")
        if row["missingObservation"] and route in ("captured", "artifact-only"):
            raise ConsolidationError(f"{row['id']}: captured row must not record a missing observation")
    notes.append(f"evidence routes: {captured} rows anchored to registered artifacts")
    coverage = scenario_coverage(rows, bundles, ledger["orphanScenarios"])
    require(not coverage["uncovered"], f"orphaned captured scenarios: {coverage['uncovered'][:10]}")
    notes.append(coverage_note(coverage))

    # 4. Groups and overlaps resolve overlapping rows without a catch-all.
    group_ids = {group["id"] for group in ledger["capabilityGroups"]}
    member_ids = [member for group in ledger["capabilityGroups"] for member in group["members"]]
    row_ids = {row["id"] for row in rows}
    require(len(group_ids) == len(ledger["capabilityGroups"]), "duplicate group id")
    require(len(member_ids) == len(set(member_ids)), "a row belongs to more than one group")
    for member in member_ids:
        require(member in row_ids, f"group member {member} is not a consolidated row")
    overlap_ids = {overlap["id"] for overlap in ledger["overlapResolutions"]}
    require(len(overlap_ids) == len(ledger["overlapResolutions"]), "duplicate overlap id")
    for overlap in ledger["overlapResolutions"]:
        require(overlap["rows"], f"{overlap['id']}: overlap records no rows")
        for row_id in overlap["rows"]:
            require(row_id in row_ids, f"{overlap['id']}: unknown row {row_id}")
        require(overlap["resolution"].strip(), f"{overlap['id']}: no resolution text")
    required_overlaps = {"overlap-toolkit-product", "overlap-session-storage", "overlap-tasks-harness", "overlap-codemode-chord"}
    require(required_overlaps <= overlap_ids, f"named overlap resolutions missing: {sorted(required_overlaps - overlap_ids)}")
    check_product_routing(rows_by_id, ledger["overlapResolutions"])
    for orphan in ledger["orphanScenarios"]:
        require(orphan["reason"].strip(), f"orphan {orphan['scenario']}: no reason")
        require(orphan["rows"], f"orphan {orphan['scenario']}: no related row")
        for row_id in orphan["rows"]:
            require(row_id in row_ids, f"orphan {orphan['scenario']}: unknown row {row_id}")
        require(
            orphan["scenario"] in bundles.get(orphan["family"], {}).get(orphan["artifact"], []),
            f"orphan {orphan['scenario']}: not a scenario of {orphan['family']}/{orphan['artifact']}",
        )
        require(orphan.get("disposition", "").strip(), f"orphan {orphan['scenario']}: no disposition")
    notes.append(f"groups/overlaps: {len(group_ids)} groups, {len(overlap_ids)} overlap resolutions")

    # 5. Corrections index rewrote exactly the recorded pointer in exactly the scoped rows.
    index = {correction["id"]: correction for correction in ledger["correctionsIndex"]}
    for row in rows:
        identities = scope_ids(row["id"], (source["rowId"] for source in row["sourceRows"]))
        for correction_id in row["appliedCorrections"]:
            item = index.get(correction_id)
            require(item is not None, f"{row['id']}: applied unknown correction {correction_id}")
            require(
                item["kind"] != "record-only",
                f"{row['id']}: record-only correction {correction_id} claims a rewrite",
            )
            require(
                item["ticket"] == row["lane"],
                f"{row['id']}: correction {correction_id} belongs to lane {item['ticket']}",
            )
            require(
                scope_names(item["appliesTo"], identities),
                f"{row['id']}: correction {correction_id} was applied outside its scope",
            )
            require(
                item["actual"] in row["source"],
                f"{row['id']}: correction {correction_id} is marked applied but the row does not carry "
                f"the corrected pointer {item['actual']}",
            )
    for correction in ledger["correctionsIndex"]:
        if correction["kind"] == "record-only":
            continue
        for row in rows:
            if row["lane"] != correction["ticket"]:
                continue
            if not scope_names(
                correction["appliesTo"],
                scope_ids(row["id"], (source["rowId"] for source in row["sourceRows"])),
            ):
                continue
            require(
                not any(
                    matches_recorded_pointer(correction["recorded"], source)
                    for source in row["source"]
                ),
                f"{row['id']}: recorded pointer {correction['recorded']} survived the corrections index",
            )
        require(
            any(correction["id"] in row["appliedCorrections"] for row in rows),
            f"{correction['id']}: correction rewrote no pointer and is not record-only",
        )
    notes.append(f"corrections index: {len(ledger['correctionsIndex'])} items")

    # 6. Optional frozen-source existence check.
    if pi_root is not None:
        frozen = Path(pi_root)
        revision = subprocess_revision(frozen)
        require(revision == REVISION, f"pi checkout is at {revision}, not the pi-v1.3.0 revision")
        checked = 0
        missing: list[str] = []
        for row in rows:
            for source in row["source"]:
                match = re.match(r"(packages/[^:]+)(?::(.*))?$", source)
                if not match:
                    continue
                path = frozen / match.group(1)
                checked += 1
                if not path.exists():
                    missing.append(f"{row['id']}: {source}")
        require(not missing, f"frozen source references missing: {missing[:10]}")
        notes.append(f"frozen sources: {checked} package references resolve at {revision[:12]}")

    return notes


def subprocess_revision(path: Path) -> str:
    import subprocess

    return subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()


def draft_ancestors(frontier: dict) -> dict[str, set[str]]:
    edges = {draft["id"]: list(draft["blockingEdges"]) for draft in frontier["drafts"]}
    result: dict[str, set[str]] = {}
    for start in edges:
        seen: set[str] = set()
        stack = list(edges[start])
        while stack:
            dependency = stack.pop()
            if dependency in seen:
                continue
            seen.add(dependency)
            stack.extend(edges.get(dependency, []))
        result[start] = seen
    return result


def validate_migration_placement(frontier: dict) -> None:
    sequence = frontier["migrationSequence"]
    first: dict[str, int] = {}
    for index, step in enumerate(sequence):
        for draft_id in step.get("drafts", []):
            first.setdefault(draft_id, index)
    draft_ids = {draft["id"] for draft in frontier["drafts"]}
    require(
        set(first) == draft_ids,
        f"migration sequence does not schedule every draft: {sorted(draft_ids - set(first))}",
    )
    ancestors = draft_ancestors(frontier)
    for draft_id, index in sorted(first.items()):
        for prerequisite in sorted(ancestors[draft_id]):
            require(
                prerequisite in first,
                f"{draft_id} is placed at {sequence[index]['id']} but prerequisite {prerequisite} is not scheduled",
            )
            require(
                first[prerequisite] <= index,
                f"{draft_id} is placed at {sequence[index]['id']} before prerequisite "
                f"{prerequisite} at {sequence[first[prerequisite]]['id']}",
            )


def migration_placement_probe(frontier: dict) -> str:
    """Positive control plus a negative probe reproducing the pre-correction M3/M4 defect."""
    validate_migration_placement(frontier)
    invalid = copy.deepcopy(frontier)
    steps = {step["id"]: step for step in invalid["migrationSequence"]}
    for step in invalid["migrationSequence"]:
        step["drafts"] = [draft for draft in step.get("drafts", []) if draft not in {"F2", "F7", "P7", "P8"}]
    steps["M3"]["drafts"].extend(["F2", "F7"])
    steps["M4"]["drafts"].extend(["P7", "P8"])
    try:
        validate_migration_placement(invalid)
    except ConsolidationError as error:
        message = str(error)
        require("F7" in message and "P7" in message, f"negative migration probe failed for the wrong reason: {message}")
        return message
    raise ConsolidationError("negative probe: invalid F7-before-P7 migration placement was accepted")


def check_frontier(
    frontier: dict, ledger: dict, bundles: dict[str, dict[str, list[str]]]
) -> list[str]:
    notes: list[str] = []
    require(frontier["schema"] == FRONTIER_SCHEMA, "frontier schema mismatch")
    row_ids = {row["id"] for row in ledger["rows"]}
    graph = frontier["ownerGraph"]
    owners = {node["owner"] for node in graph["nodes"]}
    require(owners <= set(OWNERS), f"owner graph has unknown owners: {sorted(owners - set(OWNERS))}")
    edges = {(edge["from"], edge["to"]) for edge in graph["edges"]}
    for edge in graph["edges"]:
        require(edge["from"] in owners and edge["to"] in owners, f"edge {edge} names an unknown owner")
        require(edge["evidence"], f"edge {edge} has no evidence")
    require(("cch_durable", "cch_agent_core") not in edges, "forbidden durable -> agent-core edge")
    require(("cch_durable", "cch_coding_agent") not in edges, "forbidden durable -> coding-agent edge")
    require(("cch_coding_agent", "cch_durable") not in edges, "application must not depend on durable")
    acyclic(graph["nodes"], graph["edges"], "ownerGraph")
    legal = edges | {(owner, owner) for owner in owners}
    for row in ledger["rows"]:
        for consumer in row["consumerOwners"]:
            require(
                (row["owner"], consumer) in legal or (consumer, row["owner"]) in legal,
                f"{row['id']}: owner/consumer pair {row['owner']}/{consumer} is not adjacent in the target graph",
            )
    notes.append(f"owner graph: {len(owners)} owners, {len(edges)} edges, acyclic")

    draft_ids = [draft["id"] for draft in frontier["drafts"]]
    require(len(draft_ids) == len(set(draft_ids)), "duplicate draft id")
    draft_set = set(draft_ids)
    for draft in frontier["drafts"]:
        require(draft["kind"] in DRAFT_KINDS, f"{draft['id']}: unknown draft kind")
        require(draft["owner"] in OWNERS, f"{draft['id']}: unknown owner")
        require(draft["demo"].strip(), f"{draft['id']}: no demoable public behavior")
        require(len(draft["acceptance"]) >= 2, f"{draft['id']}: fewer than two acceptance cases")
        for case in draft["acceptance"]:
            require(case.get("property") and case.get("case") and case.get("failureCase"), f"{draft['id']}: incomplete acceptance case")
        for dep in draft["blockingEdges"]:
            require(dep in draft_set, f"{draft['id']}: unknown blocking edge {dep}")
            require(dep != draft["id"], f"{draft['id']}: self edge")
        for prefix in draft.get("coverage", []):
            require(any(row_id.startswith(prefix) for row_id in row_ids), f"{draft['id']}: coverage prefix {prefix} matches no row")
        for capture in draft.get("evidence", {}).get("captures", []):
            match = re.match(r"fixtures/pi-v1\.3\.0/(audit-\d+)/([^:]+): (.*)$", capture)
            require(match, f"{draft['id']}: capture is not a registered artifact scenario: {capture}")
            family, artifact, scenario = match.groups()
            names = bundles.get(family, {}).get(artifact)
            require(names is not None, f"{draft['id']}: capture artifact is not registered: {capture}")
            require(scenario in names, f"{draft['id']}: capture scenario is not registered: {capture}")
    acyclic(
        [{"owner": draft["id"]} for draft in frontier["drafts"]],
        [{"from": draft["id"], "to": dep} for draft in frontier["drafts"] for dep in draft["blockingEdges"]],
        "draftGraph",
    )
    rank = {draft["id"]: index for index, draft in enumerate(frontier["drafts"])}
    for draft in frontier["drafts"]:
        for dep in draft["blockingEdges"]:
            require(rank[dep] < rank[draft["id"]], f"{draft['id']}: dependency {dep} is not ordered before it")
    notes.append(f"implementation frontier: {len(draft_ids)} drafts, acyclic and dependency-ordered")

    sequence_ids = [step["id"] for step in frontier["migrationSequence"]]
    require(len(sequence_ids) == len(set(sequence_ids)), "duplicate migration step id")
    sequence_index = {step_id: index for index, step_id in enumerate(sequence_ids)}
    steps_by_id = {step["id"]: step for step in frontier["migrationSequence"]}
    for step in frontier["migrationSequence"]:
        require(step["kind"] in ("expand", "migrate", "contract", "integrate-verify"), f"{step['id']}: unknown kind")
        if step.get("forwardingForm"):
            require(step.get("removalSlice"), f"{step['id']}: forwarding form without a removal slice")
            require(step["kind"] in ("expand", "migrate"), f"{step['id']}: forwarding form in a {step['kind']} step")
            removal = step["removalSlice"]
            require(
                sequence_index[removal] > sequence_index[step["id"]],
                f"{step['id']}: removal slice {removal} is not after the forwarding form",
            )
            require(
                steps_by_id[removal]["kind"] == "contract",
                f"{step['id']}: removal slice {removal} is not a contract step",
            )
        for removal in [step.get("removalSlice")] if step.get("removalSlice") else []:
            require(removal in sequence_ids, f"{step['id']}: removal slice {removal} is not in the sequence")
    notes.append(f"migration sequence: {len(sequence_ids)} steps with removal slices")
    validate_migration_placement(frontier)
    probe = migration_placement_probe(frontier)
    notes.append(f"migration placement: scheduled drafts satisfy transitive prerequisites; negative probe rejected: {probe}")

    residual_issues = {item["issue"] for item in frontier["residualReconciliation"]}
    for required in ("#1043", "#1035", "#1033", "#1032", "#831", "#812", "#810", "#808"):
        require(required in residual_issues, f"residual reconciliation misses {required}")
    for item in frontier["residualReconciliation"]:
        require(item["recommendation"].strip(), f"{item['issue']}: no recommendation")
        require(item["evidence"], f"{item['issue']}: no evidence reference")
        if item.get("draft"):
            require(item["draft"] in draft_set, f"{item['issue']}: unknown draft {item['draft']}")
    notes.append(f"residual reconciliation: {len(residual_issues)} trackers")

    close_out = frontier["closeOut"]
    for key in ("implemented", "specifiedOnly", "retainedBoundaries", "openBlockers"):
        require(close_out.get(key), f"closeOut.{key} is empty")
    require(
        any("umbrella" in item.lower() and "#1044" in item for item in close_out["openBlockers"]),
        "closeOut must keep the #1044 umbrella open",
    )
    notes.append("close-out: implemented/specified split, retained boundaries and open blockers present")
    return notes


def acyclic(nodes: list[dict], edges: list[dict], label: str) -> None:
    graph: dict[str, list[str]] = {node["owner"]: [] for node in nodes}
    for edge in edges:
        graph.setdefault(edge["from"], []).append(edge["to"])
    state: dict[str, int] = {}

    def visit(node: str) -> None:
        state[node] = 1
        for child in graph.get(node, []):
            if state.get(child) == 1:
                raise ConsolidationError(f"{label}: cycle through {node} -> {child}")
            if state.get(child) is None:
                visit(child)
        state[node] = 2

    for node in list(graph):
        if state.get(node) is None:
            visit(node)


def render_status_counts(ledger: dict, report: str) -> str:
    """Keep the report's three changing status rows derived from the ledger."""
    for label, field in (("Implementation", "implementation"), ("Verification", "verification"),
                         ("Evidence route", "evidenceRoute")):
        values = ", ".join(f"{key}:{value}" for key, value in ledger["counts"][field].items() if value)
        pattern = rf"(?m)^\| {label} \|.*\|$"
        require(len(re.findall(pattern, report)) == 1, f"report needs one {label} count row")
        report = re.sub(pattern, f"| {label} | {values} |", report)
    return report


def check_docs(ledger: dict, frontier: dict) -> list[str]:
    notes: list[str] = []
    report = REPORT_PATH.read_text(encoding="utf-8")
    require(report == render_status_counts(ledger, report), "report status counts differ from the ledger; run build")
    for path in (LEDGER_PATH.name, FRONTIER_PATH.name):
        require(path in report, f"report does not link {path}")
    links = re.findall(r"\]\(([^)#]+)\)", report)
    for link in links:
        if link.startswith("http"):
            continue
        target = (REPORT_PATH.parent / link).resolve()
        require(target.exists(), f"report link does not resolve: {link}")
    require(frontier["drafts"], "frontier has no drafts")
    notes.append(f"docs: {len(links)} relative links resolve; ledger and frontier linked")
    return notes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("build", "check"))
    parser.add_argument("--pi-root", default=None)
    args = parser.parse_args()

    plan = load_json(PLAN_PATH)
    bundles = build_bundle_index(registered_families())
    scenario_index = build_scenario_index(bundles)
    ledger = build_ledger(plan, bundles, scenario_index)
    payload = json_bytes(ledger)
    if args.command == "build":
        report = render_status_counts(ledger, REPORT_PATH.read_text(encoding="utf-8"))
        LEDGER_PATH.write_bytes(payload)
        REPORT_PATH.write_text(report, encoding="utf-8")
        print(f"wrote {LEDGER_PATH.relative_to(ROOT)} ({len(payload)} bytes, {len(ledger['rows'])} rows)")
        return 0

    require(LEDGER_PATH.exists(), f"missing {LEDGER_PATH}; run build first")
    require(LEDGER_PATH.read_bytes() == payload, "committed ledger differs from a deterministic rebuild")
    notes = check_ledger(
        plan, ledger, bundles, scenario_index, Path(args.pi_root) if args.pi_root else None
    )
    require(FRONTIER_PATH.exists(), f"missing {FRONTIER_PATH}")
    frontier = load_json(FRONTIER_PATH)
    notes.extend(check_frontier(frontier, ledger, bundles))
    notes.extend(check_docs(ledger, frontier))
    print("consolidation check: PASS")
    for note in notes:
        print(f"  - {note}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ConsolidationError, audit.EvidenceError, OSError, ValueError, KeyError, TypeError, ET.ParseError) as error:
        print(f"consolidation check: FAIL: {error}", file=sys.stderr)
        sys.exit(1)
