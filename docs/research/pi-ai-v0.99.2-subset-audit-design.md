# cch_ai ↔ pi-ai v0.99.2 子集差异审计

**Date:** 2026-10-03
**Left side (target):** pi `packages/ai/src` @ `005af57d88ee23b33778f343a9595b32e67ff788` (v0.99.2, tagged 2026-09-30)
**Prior baseline:** `f07218c4` (v0.87.1), the repo's T1 fixture provenance pin. `f07218c4` is an ancestor of `005af57d8` (verified via `git log`), so the diff range is clean.
**Right side (subject):** this repository's `cch_ai` Owner (`include/cch/ai/`, `src/ai/`) plus its evidence bundle `fixtures/pi-ai/README.md` (38 Supported rows + Deferred list).

## Definition of done (falsifiable)

A Markdown report at `docs/research/pi-ai-v0.99.2-subset-audit.md` containing:

1. **Coverage table.** Every file under pi `packages/ai/src` @ v0.99.2 (193 files, enumerated) assigned to exactly one partition: A (scoped adapter / cross-cutting / evidence), B (new subsystem requiring a scope call), or C (deferred / out of scope).
2. **Per-in-scope-file row.** For each of the four scoped adapters plus the cross-cutting layer, one verdict per *behavioral surface* (not per line): ALIGNED (green), INTENTIONAL-DIVERGENCE (yellow, with recorded rationale), GAP (red), or UPSTREAM-DRIFT (blue: new upstream behavior since `f07218c4` not present in the frozen baseline).
3. **Every verdict carries a citation** to pi source (`file:line` at v0.99.2), the C++ surface (`file` in this repo), and the existing evidence (test name → fixture path) where the claim is ALIGNED. A verdict with no citation is INCONCLUSIVE, not ALIGNED.
4. **Disposition suggestion per red/blue row:** port-and-golden, record-divergence, confirm-deferred, or escalate.

## Method

Diff-bounded. The drift window `f07218c4..005af57d8` touches 108 files (+3296/−2219) across 21 commits. Reading v0.99.2 source cold against the C++ surface is intractable in one pass; the 21-commit range is small enough to read hunk-by-hunk and gives a near-complete map of *changed* in-scope behavior. Unchanged in-scope files were already gate-audited against v0.87.1 semantics; the report confirms that by row rather than re-reading line-by-line. This is the one documented shortcut; it relies on `f07218c4` being a true ancestor, which was verified.

## Partitions

| Partition | Owner | pi scope | Why separate |
| --- | --- | --- | --- |
| P1 | Worker 1 | `openai-codex-responses.ts` + `openai-codex-responses.lazy.ts` + Codex OAuth (`auth/oauth/openai-codex.ts`, `pkce.ts`) + Codex provider shard | WS-first adapter, heaviest scoped surface |
| P2 | Worker 2 | `openai-responses.ts`, `openai-responses-shared.ts`, `openai-responses.lazy.ts` + DeepSeek provider shard | Stateless replay adapter + shared Responses processor |
| P3 | Worker 3 | `openai-completions.ts`, `openai-completions.lazy.ts` + `anthropic-messages.ts`, `anthropic-messages.lazy.ts` + Kimi vendor shard | Two adapters, shared simple-options path |
| P4 | Worker 4 | Cross-cutting: `types.ts`, `models.ts`, `simple-options.ts`, `transform-messages.ts`, `utils/retry.ts`, `utils/provider-retry.ts`, `utils/calculate-cost.ts`, `auth/resolve.ts`, `auth/types.ts`, `credential-store.ts`, `core/*` (model-config, model-runtime, auth-storage), `api/simple-options.ts`, `utils/models-error.ts` | Behavioral seams owned by Models/SimpleOptions/Auth, not by one adapter |
| P5 | Worker 5 | Full-tree classification pass over all 193 files: partition A/B/C assignment + commit-range triage of all 21 drift commits (done in main thread above; worker verifies completeness) | Coverage completeness; catches files no other worker touched |
| P6 | Main thread | `index.ts`, `model-catalog.ts`, `compat.ts`, `legacy-api-aliases.ts`, `env-api-keys.ts`, `bun-oauth.ts`, `cli.ts`, `images*.ts`, `providers/all.ts` + provider shards for the six bundled providers | Small; index surface defines the public API boundary |

## Worker output contract

Each worker writes one section:

```
## <partition name>
### Per-file rows
| Surface | pi v0.99.2 source | C++ surface | Verdict | Evidence / rationale |
### Drift-commit triage
| Commit | In scope? | Behavioral? | Verdict |
```

Rules:

- Verdicts are ALIGNED / INTENTIONAL-DIVERGENCE / GAP / UPSTREAM-DRIFT / OUT-OF-SCOPE.
- ALIGNED requires a real citation: pi `file:line`, C++ `file`, test→fixture. String or existence checks are reported as checks, never as acceptance (validation.md §Acceptance discipline).
- `INTENTIONAL-DIVERGENCE` must name a recorded rationale source (fixture README residual note, ADR, or commit message). Unrecorded divergence is GAP, not yellow.
- Every one of the 21 commits in range gets a triage row: behavioral-in-scope / refactor-or-out-of-scope, with one-line justification.

## Rigor

High. This audit is the input to pike's pi-compat graduation decision; an overclaimed ALIGNED is worse than an honest INCONCLUSIVE. Each worker's output is re-verified by a judge pass that spot-checks citations (opens the cited file:line, confirms the claim). Guard-the-context-window applies: workers read pi source in the `../pi` checkout; only their row tables return to the main thread.

## Decision trail

This design document + the final report + the worker row tables are the trail. No code changes in this run.
