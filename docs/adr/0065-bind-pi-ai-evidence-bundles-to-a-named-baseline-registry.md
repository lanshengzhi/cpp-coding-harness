---
status: accepted
---

# Bind pi-ai evidence bundles to a named baseline registry

## Context

`fixtures/pi-ai/` carries the committed evidence bundle for the pi-ai completion gate
([#347](https://github.com/lanshengzhi/cpp-coding-harness/issues/347)). Its provenance is pinned, but that pin is expressed as a **revision constant repeated
in five places** rather than as a single authority the artifacts are bound to:

| # | Site | Shape |
|---|---|---|
| 1 | `scripts/ai/record_pi_ai_provenance.py:16-17` | `BASELINE_PREFIX = "f07218c4"` / `BASELINE_REVISION = "f07218c4d4bbc12bef056a7058c3dd49dfe41abe"` |
| 2 | `scripts/ai/t0_cost_probe.mjs:9` | `const EXPECTED_PI_REVISION = "f07218c4d4bbc12bef056a7058c3dd49dfe41abe"` |
| 3 | `fixtures/pi-ai/capture/capture-completions-ts-events.mts:18` | `const frozenCommit = "f07218c4d4bbc12bef056a7058c3dd49dfe41abe"` |
| 4 | `fixtures/pi-ai/models/provenance.json:6` | `"revision"` |
| 5 | `fixtures/pi-ai/probes/t0-cost-probe.json:335` | `"pi_revision"` |

Sites 1–3 are **executable guards**: each compares its constant against `git -C <pi checkout>
rev-parse HEAD` and refuses to proceed on a mismatch. Sites 4–5 are the recorded provenance of
the committed artifacts. Nothing currently forces the two groups to agree, so checkout, guard,
and artifact can name three different revisions and each check still passes on its own terms.

Three separate pressures have made this visible.

**The pin and the work have drifted apart.** The fixture bundle is pinned to
`f07218c4d4bbc12bef056a7058c3dd49dfe41abe` (v0.87.1, generated 2026-09-23), while the completed
`cch_ai` subset audit is baselined at `a13d35a742c6ef8462812a28fbe1d8c8b7431c32` (v1.0.0). The
audit's own filename still reads `pi-ai-v0.99.2-subset-audit.md`, which invites reading
`v0.99.2` as the current audit baseline when the document is titled
`cch_ai ↔ pi-ai v1.0.0 子集差异审计报告` and names `a13d35a74` as its baseline. Two revisions are
in active use and the filenames do not disambiguate them.

**"The v0.87.1 baseline" is not one object.** [ADR 0024](0024-record-and-explicitly-advance-the-pi-parity-baseline.md) anchored Semantic Parity to
`83114817c68f5413e4d7ba6d7003ddc511cd31d2`; the fixture provenance pins
`f07218c4d4bbc12bef056a7058c3dd49dfe41abe`. They are different commits. Documents that say
"v0.87.1 baseline" without a revision therefore do not name a checkable target.

**The bundle cannot be reproduced from its revision alone.** The catalog generator reads **live**
services — models.dev, NVIDIA NIM, OpenRouter, Vercel AI Gateway, Radius — and the committed
artifacts bind one particular live snapshot by SHA-256. A future checkout at the same revision
will not reproduce those bytes. Re-recording a baseline is therefore a **new observation**, not a
recomputation, and must be reviewable as evidence.

## Considered options

- **Search-and-replace the five constants to the new revision**: rejected. It converts every
  guard to the new pin simultaneously and silently retires the verification path for artifacts
  that are already committed, leaving the v0.87.1 bundle in the tree with no working verifier.
  The resulting state is indistinguishable from a deliberate decision to freeze the old path,
  which is exactly the decision this ADR must not make on the owner's behalf.
- **Accept an arbitrary revision as a command-line or environment parameter**: rejected because it
  permits the mixed state directly — an operator can verify bundle A against checkout B, and every
  individual check still succeeds. It moves the hazard from three disagreeing constants to one
  unconstrained input.
- **Pin both revisions and select among them by name**: adopted, in the form below. Selection is
  by a *named* baseline whose revision and bundle path travel together, so a selection cannot name
  a bundle the revision does not correspond to.
- **Overwrite the existing bundle in place when a new baseline is recorded**: rejected per the
  non-destructive requirement below; the v0.87.1 bundle is the only surviving record of a live
  catalog snapshot that can no longer be re-observed.

## Decision

Evidence bundles are addressed by a **named baseline**. A baseline name resolves, in one place,
to a full 40-character revision, a bundle path, and recorded provenance. There is no free-form
revision input anywhere in the capture, record, or verify paths.

Each baseline bundles these fields together:

| Field | Meaning |
|---|---|
| `name` | stable identifier used by scripts, CI, and ADR prose |
| `revision` | full 40-character pi commit SHA — never a tag or short prefix |
| `bundle_path` | the artifact directory that revision's evidence occupies |
| `captured_at` | generation timestamp for the recorded snapshot |
| `source_endpoints` | the live catalog services the snapshot was read from |
| `digests` | the per-artifact SHA-256 values bound by that snapshot |

Three rules follow from bundling the fields rather than storing them separately:

1. **Verification binds to bundle metadata, not to a repeated constant.** A verifier reads the
   baseline that owns the bundle it is checking and compares `git rev-parse HEAD` against *that*
   baseline's `revision`. The three executable guards stop carrying independent copies of the
   pin and become reads of the registry. The two recorded provenance files are deliberately
   **not** registry-backed — see the scope decision below.
2. **Generators never write across baselines.** A capture or record run targeting one baseline
   must refuse to modify another baseline's bundle path. Writing v1.0.0 evidence over the v0.87.1
   directory is a hard failure, not a merge.
3. **Recording a new baseline is additive.** The prior bundle keeps its directory, its digests,
   and a working verifier. New evidence lands in a new baseline-suffixed bundle path.
   @lansy ruled **preserve**: the v0.87.1 bundle stays in place **and keeps a working verifier**.

### Divergence rationale

These are deliberate changes to Pike's own current implementation, with the reasoning recorded
here so it is not reconstructed from a diff. They are **not** an `Intentional Divergence` in the
`GLOSSARY.md` sense: that term is a departure from Semantic Parity, i.e. from pi's behavior. The
changes below concern Pike's internal evidence plumbing and do not depart from pi at all, so the
glossary term is deliberately not used for them.

- **Binding verification to bundle metadata** rather than a constant trades a cheap, readable
  literal for a single authority. The literal was copyable into five sites and stayed correct in
  none of them independently; the registry is correct in exactly one place.
- **Refusing cross-baseline writes** makes an operation that would previously have succeeded now
  fail loudly. That failure is the point: overwriting the only surviving record of an unobservable
  snapshot is unrecoverable, and a warning would not stop it.
- **Naming baselines instead of accepting a revision parameter** removes flexibility nobody needs.
  Every legitimate consumer selects a baseline that already exists, so the unconstrained input
  buys nothing and costs the mixed-state hazard.

### Full SHAs, and why tags are not accepted

Baselines are recorded by full 40-character SHA. In the current pi checkout `v1.0.0` resolves
**ambiguously** (`git rev-parse v1.0.0` warns `refname 'v1.0.0' is ambiguous`), so a tag-pinned
ADR could be satisfied by more than one object. Known revisions:

| Baseline | Revision |
|---|---|
| T1 catalog fixture provenance (v0.87.1) | `f07218c4d4bbc12bef056a7058c3dd49dfe41abe` |
| ADR 0024 frozen parity commit | `83114817c68f5413e4d7ba6d7003ddc511cd31d2` |
| `cch_ai` subset audit target (v1.0.0) | `a13d35a742c6ef8462812a28fbe1d8c8b7431c32` |
| audit detailed reading position (v0.99.2) | `005af57d88ee23b33778f343a9595b32e67ff788` |

### Scope decision: the baseline this cycle records

**Resolved 2026-10-04 by @lansy: this cycle covers capabilities not yet audited, against pi
v1.0.0.** The new baseline is:

| Field | Value |
|---|---|
| `name` | `pi-v1.0.0` |
| `revision` | `a13d35a742c6ef8462812a28fbe1d8c8b7431c32` (pi `v1.0.0`; the `v1.0.0` refname is ambiguous, so the full SHA is authoritative) |

This is already the `cch_ai` subset audit's target, so the recorded evidence moves toward the
direction the repository was already working. The v0.87.1 bundle must stay verifiable while a
second bundle is captured beside it.

**Scope narrowing — ratified by @lansy, 2026-10-04.** Of the five recorded hard-coded revisions,
only the **three executable guards** are migrated to the registry.
`fixtures/pi-ai/models/provenance.json` and `fixtures/pi-ai/probes/t0-cost-probe.json` are left
verbatim, because they record *how the v0.87.1 bundle was originally captured* and preserve means
that record is not rewritten — rewriting it would falsify the capture history.

The narrowing was put to @lansy as an open question rather than assumed, because it was the
implementer's reading and not part of his preserve ruling. He ratified it, so the two recorded
provenance files stay as they are and the three guards remain the registry-backed set. Had it
been rejected, the fallback would have been to migrate sites 4–5 and carry the capture history in
a **new** file rather than by editing the recorded one.

The alternative branch — extending [ADR 0060](0060-advance-the-cch-coding-agent-baseline-to-pi-v0-87-1-and-adopt-transcript-system-messages.md)'s
existing scope and retaining v0.87.1 `f07218c4d4bbc12bef056a7058c3dd49dfe41abe` — was not
chosen. It is recorded here because it is the branch that required **no executable change at
all**, and because its absence is what makes the guard-site edits in scope.

The two options were named by what they *do*, not by numbers: an earlier draft of this section
used S1/S2 labels, and those labels were already read two different ways across two documents in
this cycle. A numbered label that has demonstrably drifted is the ambiguity, not the fix.

**Decided by @lansy: preserve.** The v0.87.1 bundle is preserved in place and keeps a working
verifier. The v1.0.0 **baseline is registered** — it resolves to a revision and a bundle path,
and the tools select it by name — but **its evidence bundle is not captured**, so it is not yet
independently verifiable. "Registered" and "captured" are different states, and this ADR
distinguishes them. Rules 1 and 2 hold independently of the preserve ruling.

### Relationship to other ADRs

This ADR adds no capability alignment and does not alter any alignment authority.
[ADR 0053](0053-replace-pi-parity-authority-with-the-product-architecture-contract.md) keeps pi
as evidence rather than design authority, and per-capability alignment decisions remain
per-capability: if the "cover capabilities not yet audited" option is chosen, the revisions named in
[ADR 0059](0059-align-the-cch-ai-capability-subset-with-pi-ai-and-diverge-on-kimi-code.md) and
[ADR 0060](0060-advance-the-cch-coding-agent-baseline-to-pi-v0-87-1-and-adopt-transcript-system-messages.md)
stand as the existing decision record for the capabilities they govern. **This ADR does not
rewrite them, and a migration of any baseline they reference requires its own ADR** rather than a
silent edit to those files.

Note also that [ADR 0024](0024-record-and-explicitly-advance-the-pi-parity-baseline.md) still carries `status: accepted` in its frontmatter even though
ADR 0053 supersedes it. That inconsistency predates this ADR and is not corrected here.

## Consequences

- The **three executable guards** stop being independent copies of the pin. They keep their
  refusal behavior but source the expected revision from the baseline that owns the bundle under
  test, so checkout, guard, and artifact can no longer disagree. The two recorded provenance
  files are deliberately not migrated — see the scope narrowing above.
- The v0.87.1 bundle stays in place with a working verifier, and stays the only record of a live
  catalog snapshot that cannot be re-observed.
- Re-recording a baseline is explicitly *new evidence* rather than a refresh: it carries its own
  timestamp, endpoint set, and digests, and it is reviewed as evidence. Nothing in this ADR makes
  a recorded snapshot reproducible, because upstream services are not.
- "v0.87.1 baseline" becomes ambiguous no longer in practice: every baseline is named and
  revision-pinned, so prose can cite either and a reader can resolve it.
- Of the five recorded hard-coded revisions, the **three executable guards now read the registry**
  and the **two recorded provenance files are unchanged**. That split is a scope narrowing
  ratified by @lansy on 2026-10-04, recorded in the scope decision below.
- The v0.87.1 bundle keeps a working verifier: `check_pi_ai_provenance.py` still passes against
  the untouched bundle, and `tests/ai/PiAiProvenanceTest.py` passes with its stale-snapshot
  cases. Capture, record, and probe scripts select a baseline by name and refuse to write into a
  bundle owned by another.
- A v1.0.0 bundle has **not** been captured. Recording one requires re-observing live catalog
  services and is reviewed as new evidence in its own step. Until then the tools select
  `pi-v1.0.0` by name and fail loudly on its missing bundle directory rather than falling back
  to the v0.87.1 bundle: a silent fallback would produce a v1.0.0-labelled report built from
  v0.87.1 evidence.
- Capture and probe resolve both their write target and their input data through the selected
  baseline's `bundle_path`, and refuse to read or write a bundle owned by another baseline.
- This ADR reached `accepted` together with the migration it authorizes, because @lansy has
  answered all three of its fields. The scope narrowing above was ratified by him separately
  once it was put to him as a question rather than assumed.
- `GLOSSARY.md` gains `Named Baseline` and `Evidence Bundle`, landing in the same change as this
  ADR so the vocabulary and the decision enter together. `Parity Baseline` and `parity map` stay
  historical per ADR 0053 and are named in both `_Avoid_` lists so neither term is revived.
- Both follow-ups this ADR opened are now closed. The `GLOSSARY.md` entries landed here. The
  supersession record for [ADR 0024](0024-record-and-explicitly-advance-the-pi-parity-baseline.md)
  — which read `status: accepted` while ADR 0053 supersedes it — was added separately (#3) using
  the body-note house convention taken from ADR 0001, leaving that ADR's frontmatter untouched.
  Neither item remains open drift.

## References

- [ADR 0053](0053-replace-pi-parity-authority-with-the-product-architecture-contract.md) — parity authority replaced by the Product Architecture Contract
- [ADR 0059](0059-align-the-cch-ai-capability-subset-with-pi-ai-and-diverge-on-kimi-code.md) — `cch_ai` capability subset alignment
- [ADR 0060](0060-advance-the-cch-coding-agent-baseline-to-pi-v0-87-1-and-adopt-transcript-system-messages.md) — `cch_coding_agent` baseline
- `fixtures/pi-ai/README.md` — committed evidence bundle and T1 provenance
- `docs/research/pi-ai-v0.99.2-subset-audit.md` — completed `cch_ai` subset audit (baselined at v1.0.0; filename predates that baseline)
