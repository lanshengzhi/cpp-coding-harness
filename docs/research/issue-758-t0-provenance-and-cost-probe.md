# Issue #758 T0: pi-ai provenance and minimum cost probe

Status: complete. This note records the verified catalog snapshot and the
minimum local wire probe that must precede production catalog or adapter work.
T0 intentionally makes no production adapter or generated-directory change.

## Provenance gate

The source checkout is `https://github.com/earendil-works/pi` at
`1a584a7a56eb5e7b4ff8ccbd46430f1533282eed` (`1a584a7a5`). From that checkout,
the generator was run at `2026-09-22T05:24:23Z` with:

```text
node packages/ai/scripts/generate-models.ts --strict
```

The command completed successfully and reported 1,460 tool-capable models
across the full generated catalog. It reads live `models.dev`, NVIDIA NIM,
OpenRouter, Vercel AI Gateway, and Radius catalogs. Therefore the commit pins
the generator and adapter source, while the six artifact hashes pin this
particular live-data snapshot. The generated files were copied unchanged from
`packages/ai/src/providers/data/` into
[`fixtures/pi-ai/models/providers/`](../../fixtures/pi-ai/models/providers/).

The authoritative machine-readable record is
[`fixtures/pi-ai/models/provenance.json`](../../fixtures/pi-ai/models/provenance.json).
It contains the complete sorted `model_ids` list and API grouping for every
artifact. The resulting sets are:

| Provider | Count | API families |
| --- | ---: | --- |
| `deepseek` | 2 | `openai-completions` |
| `kimi-coding` | 4 | `anthropic-messages` |
| `openai` | 39 | `openai-responses` |
| `openai-codex` | 6 | `openai-codex-responses` |
| `openrouter` | 378 | `openai-completions` |
| `opencode-go` | 30 | `openai-completions`, `openai-responses`, `anthropic-messages` |

The Codex discrepancy is resolved by the generator, not by the old fixture:
the verified six are `gpt-5.3-codex-spark`, `gpt-5.5`, `gpt-5.6-luna`,
`gpt-5.6-sol`, `gpt-5.6-terra`, and `gpt-6-astra`. The previous seven-model
snapshot's `gpt-5.4` and `gpt-5.4-mini` are not in this verified set.

## Probe method

[`scripts/ai/t0_cost_probe.mjs`](../../scripts/ai/t0_cost_probe.mjs) imports
the actual baseline TypeScript adapters, supplies one normalized system
prompt, one user text, and one ordinary JSON-schema tool, and replaces `fetch`
with an in-process response. No provider network or credential is used. Each
case runs `streamSimple`, captures the request payload in both the adapter
callback and the fake transport, and consumes the returned stream. The frozen
result is
[`fixtures/pi-ai/probes/t0-cost-probe.json`](../../fixtures/pi-ai/probes/t0-cost-probe.json).

The measured results are:

| Case | Actual baseline observation | Raw request SHA-256 |
| --- | --- | --- |
| `openai-completions` using `deepseek-flash` | One `POST` to `/chat/completions`; 454-byte body; `reasoning_effort:"high"`, `thinking:{type:"enabled"}`, and ordinary tool `strict:false` | `53317fc16570d8760e78ff354625f62a6c8e2ce3e67379599ab99dae345c1020` |
| Anthropic budget thinking using `opencode-go/minimax-m3` | One `POST` to `/messages?beta=true`; `max_tokens:6144`, `thinking:{type:"enabled",budget_tokens:2048,display:"summarized"}`; the generated model has no adaptive-thinking override, so this branch is reachable | `becf6c3779861c2d88f73c0405cb4cebdfe5cb4d9bb06ebd6fc603e8b9750c9` |
| Responses ordinary tool using `openai/gpt-4` | One `POST` to `/v1/responses`; the tool contains `strict:false` | `a44b15c152f40bfbf5031c47668170d33b737e73c6956533eb8279aa7e108848` |

The same probe records the upstream source surface as an observation:
`openai-completions.ts` is 62,871 bytes / 1,726 lines,
`anthropic-messages.ts` is 50,697 bytes / 1,520 lines,
`openai-responses.ts` is 14,282 bytes / 397 lines, and
`openai-responses-shared.ts` is 30,087 bytes / 793 lines. These are measured
scope indicators, not an estimate of engineering time.

The C++ probe test
`"T0 probe records the missing OpenAI Completions dispatch"` measures the
current composition seam: an `openai-completions` model produces the explicit
no-implementation stream error and makes zero transport calls.
`"T0 probe records the reachable Anthropic budget-thinking limitation"` measures
the current model-validation error for the same synthetic shape as
`opencode-go/minimax-m3`. `"T0 probe records the ordinary Responses strict-false
limitation"` measures that the current generic Responses payload omits the
`strict` member. These outcomes are intentionally recorded as the pre-T0
baseline and must be changed by the dependent implementation tickets.

## Decision and dependent scope

Go on the pinned upstream direction: the generator confirms that the six
provider set requires the completions family, and the real baseline adapter
probe shows the exact request behavior to mirror. No-go on landing the
generated catalog in production yet. The dependent implementation work must
add the private `openai-completions` dispatch/adapter and typed compatibility
surface, then close both reachable gaps before promoting the snapshot:

1. `openai-completions` must reproduce the captured DeepSeek request shape and
   the provider-specific reasoning/tool-history behavior.
2. The Anthropic budget branch must produce the captured enabled thinking
   payload for generated Anthropic-family reasoning models, or those models
   must be explicitly removed from the supported catalog with a recorded
   scope decision. A Deferred label alone is not sufficient.
3. Ordinary Responses tools must preserve the baseline `strict:false` member
   where the generated model advertises strict-mode support.
4. Catalog parity tests must consume the six artifacts independently of the
   generator output and use the six verified counts/sets, not the stale
   observations in #757.

This adjusts dependent ticket scope from the old observations: OpenAI is 39
(not 38), OpenRouter is 378 (not 274), OpenCode Go is 30 (not 18), DeepSeek's
verified IDs are `deepseek-flash` and `deepseek-v4-pro`, and Codex is the
verified six above. Kimi's generated artifact remains the upstream
`anthropic-messages` source artifact; the documented vendor API-key and
completions-family divergence remains a later implementation decision, not a
silent mutation of this upstream evidence.

## Limits and rerun commands

The probe does not prove provider acceptance, billing, live authentication, or
service-side model availability; those require separately authorized manual
validation. It also does not claim to implement any adapter. The generated
snapshot is reproducible only while the live catalog responses remain
available and unchanged; the committed hashes are the reproducibility anchor.

Offline checks:

```text
python3 scripts/ai/check_pi_ai_provenance.py \
  --pi-root /path/to/pi
node scripts/ai/t0_cost_probe.mjs \
  --pi-root /path/to/pi \
  --fixture-root fixtures/pi-ai \
  --output fixtures/pi-ai/probes/t0-cost-probe.json
```

To re-derive the six artifacts, run the pinned generator from the pi checkout
at the pinned revision, then run
`scripts/ai/record_pi_ai_provenance.py` with the completed UTC timestamp and
the exact generator command. If the network or generator is unavailable,
retain the existing hash-pinned artifacts and report the failing endpoint or
command output; the smallest manual follow-up is to rerun that generator and
refresh only the provenance record after verifying all six hashes.
