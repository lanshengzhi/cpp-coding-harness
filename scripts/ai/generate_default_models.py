#!/usr/bin/env python3
"""Generate the offline C++ model catalog from the pinned T0 snapshot.

The generator has one source of truth for upstream data:
``fixtures/pi-ai/models/provenance.json`` and the six artifacts it names.
Every artifact hash, byte count, model set, and API grouping is checked before
it can contribute to the output.  The upstream Kimi artifact is verified as
part of that gate but is deliberately not used for production output;
``models/vendors/kimi-coding.json`` is the separately pinned vendor oracle.

The output is deterministic:

* providers and models are sorted by their identifier;
* object keys are sorted recursively;
* arrays retain their data order (the input modality order is meaningful);
* JSON uses UTF-8, two-space indentation, and one final LF;
* the generated C++ wrapper uses a fixed raw-string delimiter, warning-safe
  chunks, and LF endings.

Run ``python3 scripts/ai/generate_default_models.py`` from the repository root
to regenerate ``src/ai/DefaultModelsJson.hpp`` and
``src/ai/DefaultModelsJson.cpp``.  Add ``--check`` to verify the committed
outputs without writing them.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import subprocess
import sys
from pathlib import Path
from typing import Any

from record_pi_ai_provenance import BASELINE_REVISION, TARGET_PROVIDERS


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_FIXTURE_ROOT = REPOSITORY_ROOT / "fixtures" / "pi-ai"
DEFAULT_HEADER = REPOSITORY_ROOT / "src" / "ai" / "DefaultModelsJson.hpp"
DEFAULT_OUTPUT = REPOSITORY_ROOT / "src" / "ai" / "DefaultModelsJson.cpp"
DEFAULT_VENDOR_PROVENANCE = (
    DEFAULT_FIXTURE_ROOT / "models" / "vendors" / "provenance.json"
)
PROVENANCE_SCHEMA = "cpp-coding-harness/pi-ai-provenance/1"
VENDOR_PROVENANCE_SCHEMA = "cpp-coding-harness/vendor-model-provenance/1"
GENERATOR_NAME = "packages/ai/scripts/generate-models.ts"
GENERATOR_COMMAND = "node packages/ai/scripts/generate-models.ts --strict"
KIMI_PROVIDER = "kimi-coding"
KIMI_VENDOR_PATH = "models/vendors/kimi-coding.json"
KIMI_BASE_URL = "https://api.kimi.com/coding/v1"
RAW_STRING_DELIMITER = "cch_catalog"
RAW_STRING_MAX_BYTES = 60 * 1024

REQUIRED_MODEL_FIELDS = (
    "id",
    "name",
    "api",
    "provider",
    "baseUrl",
    "reasoning",
    "input",
    "cost",
    "contextWindow",
    "maxTokens",
)
COST_FIELDS = ("input", "output", "cacheRead", "cacheWrite")
INPUT_MODALITIES = {"text", "image"}


class GenerationError(RuntimeError):
    """A provenance or catalog contract violation."""


def _error(message: str) -> GenerationError:
    return GenerationError(message)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _read_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise _error(f"cannot read JSON {path}: {error}") from error


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _is_integer(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _is_finite_number(value: Any) -> bool:
    return _is_number(value) and math.isfinite(value)


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise _error(message)


def _validate_model(provider_id: str, api: str, model_id: str, model: Any) -> None:
    _require(isinstance(model, dict), f"{provider_id}/{model_id} is not an object")
    missing = [field for field in REQUIRED_MODEL_FIELDS if field not in model]
    _require(not missing, f"{provider_id}/{model_id} is missing {', '.join(missing)}")
    _require(model["id"] == model_id, f"{provider_id}/{model_id} has a mismatched id")
    _require(model["api"] == api, f"{provider_id}/{model_id} has a mismatched api")
    _require(
        model["provider"] == provider_id,
        f"{provider_id}/{model_id} has a mismatched provider",
    )
    _require(isinstance(model["name"], str) and model["name"], f"{provider_id}/{model_id} has no name")
    _require(isinstance(model["baseUrl"], str) and model["baseUrl"], f"{provider_id}/{model_id} has no baseUrl")
    _require(isinstance(model["reasoning"], bool), f"{provider_id}/{model_id} has invalid reasoning")

    input_modalities = model["input"]
    _require(
        isinstance(input_modalities, list) and input_modalities,
        f"{provider_id}/{model_id} has invalid input modalities",
    )
    _require(
        all(isinstance(value, str) and value in INPUT_MODALITIES for value in input_modalities),
        f"{provider_id}/{model_id} has an unknown input modality",
    )

    cost = model["cost"]
    _require(isinstance(cost, dict), f"{provider_id}/{model_id} has invalid cost")
    # pi uses -1e6 for OpenRouter's provider-routed models to mean that the
    # service does not publish a fixed rate. Preserve that catalog value; the
    # generator must not turn it into an invented zero/default price.
    _require(
        all(field in cost and _is_finite_number(cost[field]) for field in COST_FIELDS),
        f"{provider_id}/{model_id} has incomplete cost",
    )
    tiers = cost.get("tiers")
    if tiers is not None:
        _require(isinstance(tiers, list), f"{provider_id}/{model_id} has invalid cost tiers")
        for index, tier in enumerate(tiers):
            _require(isinstance(tier, dict), f"{provider_id}/{model_id} tier {index} is not an object")
            _require(
                all(
                    field in tier
                    and _is_finite_number(tier[field])
                    and (
                        field != "inputTokensAbove"
                        or (_is_integer(tier[field]) and tier[field] >= 0)
                    )
                    for field in (*COST_FIELDS, "inputTokensAbove")
                ),
                f"{provider_id}/{model_id} tier {index} is incomplete",
            )

    _require(
        _is_integer(model["contextWindow"]) and model["contextWindow"] > 0,
        f"{provider_id}/{model_id} has invalid contextWindow",
    )
    _require(
        _is_integer(model["maxTokens"]) and model["maxTokens"] > 0,
        f"{provider_id}/{model_id} has invalid maxTokens",
    )

    thinking_map = model.get("thinkingLevelMap")
    if thinking_map is not None:
        _require(isinstance(thinking_map, dict), f"{provider_id}/{model_id} has invalid thinkingLevelMap")
        _require(
            all(
                isinstance(key, str)
                and key in {"off", "minimal", "low", "medium", "high", "xhigh", "max"}
                for key in thinking_map
            ),
            f"{provider_id}/{model_id} has an unknown thinking level",
        )
        _require(
            all(value is None or isinstance(value, str) for value in thinking_map.values()),
            f"{provider_id}/{model_id} has an invalid thinkingLevelMap value",
        )

    headers = model.get("headers")
    if headers is not None:
        _require(isinstance(headers, dict), f"{provider_id}/{model_id} has invalid headers")
        _require(
            all(isinstance(name, str) and isinstance(value, str) for name, value in headers.items()),
            f"{provider_id}/{model_id} has invalid header values",
        )


def _validate_catalog(provider_id: str, catalog: Any) -> dict[str, list[dict[str, Any]]]:
    _require(isinstance(catalog, dict) and catalog, f"{provider_id} catalog is empty or invalid")
    normalized: dict[str, list[dict[str, Any]]] = {}
    model_ids: set[str] = set()
    for api, models in catalog.items():
        _require(isinstance(api, str) and api, f"{provider_id} has an invalid API group")
        _require(isinstance(models, dict) and models, f"{provider_id}/{api} is empty or invalid")
        entries: list[dict[str, Any]] = []
        for model_id, model in models.items():
            _require(isinstance(model_id, str) and model_id, f"{provider_id}/{api} has an invalid model id")
            _require(model_id not in model_ids, f"{provider_id} repeats model id {model_id}")
            _validate_model(provider_id, api, model_id, model)
            model_ids.add(model_id)
            entries.append(copy.deepcopy(model))
        normalized[api] = entries
    return normalized


def _validate_upstream_provenance(
    fixture_root: Path,
    pi_root: Path | None,
) -> dict[str, dict[str, list[dict[str, Any]]]]:
    provenance_path = fixture_root / "models" / "provenance.json"
    provenance = _read_json(provenance_path)
    _require(
        isinstance(provenance, dict) and provenance.get("schema") == PROVENANCE_SCHEMA,
        f"unexpected provenance schema in {provenance_path}",
    )
    source = provenance.get("source")
    _require(isinstance(source, dict), "provenance source is missing")
    _require(
        source.get("revision") == BASELINE_REVISION,
        "provenance source revision is not the pinned baseline",
    )
    _require(source.get("generator") == GENERATOR_NAME, "provenance generator is not the pinned generator")
    _require(source.get("command") == GENERATOR_COMMAND, "provenance command is not the pinned command")
    _require(isinstance(source.get("generated_at"), str) and source["generated_at"], "provenance timestamp is missing")

    if pi_root is not None:
        try:
            revision = subprocess.check_output(
                ["git", "-C", str(pi_root.resolve()), "rev-parse", "HEAD"],
                text=True,
            ).strip()
        except (OSError, subprocess.CalledProcessError) as error:
            raise _error(f"cannot read pi checkout revision: {error}") from error
        _require(revision == BASELINE_REVISION, "pi checkout is not at the pinned baseline")

    providers = provenance.get("providers")
    _require(isinstance(providers, dict), "provenance providers are missing")
    _require(set(providers) == set(TARGET_PROVIDERS), "provenance provider set does not match T0")

    catalogs: dict[str, dict[str, list[dict[str, Any]]]] = {}
    for provider_id in TARGET_PROVIDERS:
        record = providers[provider_id]
        _require(isinstance(record, dict), f"provenance record is invalid: {provider_id}")
        expected_path = f"models/providers/{provider_id}.json"
        _require(record.get("path") == expected_path, f"provenance path is invalid: {provider_id}")
        artifact_path = fixture_root / record["path"]
        _require(artifact_path.is_file(), f"missing pinned artifact: {artifact_path}")
        actual_hash = _sha256(artifact_path)
        _require(actual_hash == record.get("sha256"), f"SHA-256 mismatch: {artifact_path}")
        _require(artifact_path.stat().st_size == record.get("bytes"), f"byte count mismatch: {artifact_path}")
        catalog = _validate_catalog(provider_id, _read_json(artifact_path))
        model_ids = sorted(model_id for models in catalog.values() for model_id in (model["id"] for model in models))
        _require(model_ids == record.get("model_ids"), f"model set mismatch: {provider_id}")
        _require(len(model_ids) == record.get("model_count"), f"model count mismatch: {provider_id}")
        actual_apis = {
            api: sorted(model["id"] for model in models)
            for api, models in sorted(catalog.items())
        }
        _require(actual_apis == record.get("apis"), f"API grouping mismatch: {provider_id}")
        catalogs[provider_id] = catalog
    return catalogs


def _validate_vendor_provenance(fixture_root: Path) -> dict[str, list[dict[str, Any]]]:
    provenance = _read_json(DEFAULT_VENDOR_PROVENANCE if fixture_root == DEFAULT_FIXTURE_ROOT else fixture_root / "models" / "vendors" / "provenance.json")
    _require(
        isinstance(provenance, dict) and provenance.get("schema") == VENDOR_PROVENANCE_SCHEMA,
        "unexpected vendor provenance schema",
    )
    specs = provenance.get("specs")
    _require(isinstance(specs, dict) and set(specs) == {KIMI_PROVIDER}, "vendor provenance must name Kimi only")
    record = specs[KIMI_PROVIDER]
    _require(isinstance(record, dict) and record.get("path") == KIMI_VENDOR_PATH, "Kimi vendor path is invalid")
    authority = record.get("authority")
    _require(
        isinstance(authority, dict)
        and isinstance(authority.get("documentation"), str)
        and isinstance(authority.get("decision_record"), str),
        "Kimi vendor authority is missing",
    )
    spec_path = fixture_root / record["path"]
    _require(spec_path.is_file(), f"missing Kimi vendor specification: {spec_path}")
    _require(_sha256(spec_path) == record.get("sha256"), "Kimi vendor specification hash mismatch")
    _require(spec_path.stat().st_size == record.get("bytes"), "Kimi vendor specification byte count mismatch")
    return _validate_catalog(KIMI_PROVIDER, _read_json(spec_path))


def _canonicalize(value: Any) -> Any:
    if isinstance(value, dict):
        return {key: _canonicalize(value[key]) for key in sorted(value)}
    if isinstance(value, list):
        return [_canonicalize(item) for item in value]
    return value


def _build_document(
    upstream_catalogs: dict[str, dict[str, list[dict[str, Any]]]],
    vendor_catalog: dict[str, list[dict[str, Any]]],
) -> str:
    upstream_kimi_ids = {
        model["id"]
        for models in upstream_catalogs[KIMI_PROVIDER].values()
        for model in models
    }
    vendor_kimi_ids = {
        model["id"] for models in vendor_catalog.values() for model in models
    }
    _require(vendor_kimi_ids == upstream_kimi_ids, "Kimi vendor model set differs from the verified snapshot")
    _require(set(vendor_catalog) == {"openai-completions"}, "Kimi vendor catalog must use OpenAI Completions")

    providers: dict[str, dict[str, list[dict[str, Any]]]] = {}
    for provider_id in sorted(TARGET_PROVIDERS):
        source_catalog = vendor_catalog if provider_id == KIMI_PROVIDER else upstream_catalogs[provider_id]
        models = [
            copy.deepcopy(model)
            for api_models in source_catalog.values()
            for model in api_models
        ]
        models.sort(key=lambda model: model["id"])
        if provider_id == KIMI_PROVIDER:
            _require(
                all(model["baseUrl"] == KIMI_BASE_URL for model in models),
                "Kimi vendor catalog contains a non-vendor endpoint",
            )
            _require(
                all("headers" not in model for model in models),
                "Kimi vendor catalog must not carry another tool's headers",
            )
        providers[provider_id] = {"models": [_canonicalize(model) for model in models]}

    document = {"providers": providers}
    return json.dumps(
        _canonicalize(document),
        ensure_ascii=False,
        indent=2,
        separators=(",", ": "),
    ) + "\n"


def _render_cpp(document: str) -> bytes:
    _require(
        f"){RAW_STRING_DELIMITER}\"" not in document,
        "catalog contains the generated C++ raw-string delimiter",
    )
    chunks: list[str] = []
    current: list[str] = []
    current_bytes = 0
    for line in document.splitlines(keepends=True):
        line_bytes = len(line.encode("utf-8"))
        _require(line_bytes <= RAW_STRING_MAX_BYTES, "catalog line exceeds the raw-string chunk limit")
        if current and current_bytes + line_bytes > RAW_STRING_MAX_BYTES:
            chunks.append("".join(current))
            current = []
            current_bytes = 0
        current.append(line)
        current_bytes += line_bytes
    if current:
        chunks.append("".join(current))

    raw_literals: list[str] = []
    for index, chunk in enumerate(chunks):
        prefix = "\n" if index == 0 else ""
        raw_literals.append(f'R"{RAW_STRING_DELIMITER}({prefix}{chunk}){RAW_STRING_DELIMITER}"')
    total_bytes = sum(len(chunk.encode("utf-8")) for chunk in chunks) + 1
    part_declarations = [
        f"constexpr char kCatalogPart{index}[] = {literal};"
        for index, literal in enumerate(raw_literals)
    ]
    part_appends = [
        f"    append_catalog_part(result, offset, kCatalogPart{index});"
        for index in range(len(raw_literals))
    ]
    source_lines = [
        '#include "DefaultModelsJson.hpp"',
        "",
        "#include <array>",
        "#include <cstddef>",
        "",
        "namespace cch::ai {",
        "",
        "namespace {",
        "",
        *part_declarations,
        "",
        "template <std::size_t DestinationSize, std::size_t PartSize>",
        "void append_catalog_part(",
        "        std::array<char, DestinationSize>& destination, std::size_t& offset, const char (&part)[PartSize]) {",
        "    for (std::size_t index = 0; index + 1 < PartSize; ++index) {",
        "        destination[offset++] = part[index];",
        "    }",
        "}",
        "",
        f"const std::array<char, {total_bytes}> kCatalogData = [] {{",
        f"    std::array<char, {total_bytes}> result{{}};",
        "    std::size_t offset = 0;",
        *part_appends,
        "    return result;",
        "}();",
        "",
        "} // namespace",
        "",
        "[[nodiscard]] std::string_view default_models_json() noexcept { return {kCatalogData.data(), kCatalogData.size()}; }",
        "",
        "} // namespace cch::ai",
        "",
    ]
    source = "\n".join(source_lines)
    return source.encode("utf-8")


def _render_header() -> bytes:
    return (
        "#pragma once\n"
        "\n"
        "#include <string_view>\n"
        "\n"
        "namespace cch::ai {\n"
        "\n"
        "/// The bundled model catalog generated from the hash-pinned T0 snapshot.\n"
        "/// Provider display names, authentication, and dispatch remain private\n"
        "/// policy in BuiltinProviders.cpp and the provider implementation.\n"
        "[[nodiscard]] std::string_view default_models_json() noexcept;\n"
        "\n"
        "} // namespace cch::ai\n"
    ).encode("utf-8")


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture-root", type=Path, default=DEFAULT_FIXTURE_ROOT)
    parser.add_argument("--header", type=Path, default=DEFAULT_HEADER)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--pi-root", type=Path, help="optionally verify an external pi checkout revision")
    parser.add_argument("--check", action="store_true", help="verify output without writing it")
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    fixture_root = args.fixture_root.resolve()
    header = args.header.resolve()
    output = args.output.resolve()
    try:
        upstream_catalogs = _validate_upstream_provenance(fixture_root, args.pi_root)
        vendor_catalog = _validate_vendor_provenance(fixture_root)
        expected = _render_cpp(_build_document(upstream_catalogs, vendor_catalog))
        expected_header = _render_header()
        if args.check:
            if not output.is_file() or output.read_bytes() != expected:
                print(f"generated catalog is stale: {output}", file=sys.stderr)
                return 1
            if not header.is_file() or header.read_bytes() != expected_header:
                print(f"generated catalog header is stale: {header}", file=sys.stderr)
                return 1
            print(f"generated catalog: PASS ({output})")
            return 0
        output.parent.mkdir(parents=True, exist_ok=True)
        header.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(expected)
        header.write_bytes(expected_header)
        print(f"generated catalog: {header}, {output}")
        return 0
    except GenerationError as error:
        print(f"generated catalog: ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
