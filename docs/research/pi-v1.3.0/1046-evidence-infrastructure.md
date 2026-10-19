# pi-v1.3.0 named audit evidence infrastructure (#1046)

- **Baseline**: `pi-v1.3.0` at `71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64 with glibc.
- **Authority**: [ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md),
  recorded by `196f4231e4ebbc8bdb06d3b2152d72aa8850e956`; umbrella [spec #1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044).
- **Scope**: shared evidence infrastructure only. This ticket adds no production behavior, no
  package migration and no `cch_ai` baseline change. It captures one genuine public-call family to
  prove the path; every other capability family remains the owning audit ticket's work.

## What landed

| Piece | Path | Purpose |
| --- | --- | --- |
| Named registry | `fixtures/pi-v1.3.0/baselines.json` | One baseline name → full revision; per-ticket family → pinned manifest SHA-256 |
| Shared verifier | `scripts/evidence/audit_evidence.py` | `check` / `register` / `verify` / `read`, fail closed |
| Worked example family | `fixtures/pi-v1.3.0/audit-1046/` | Executable capture, real observation, manifest |
| Conventions | `fixtures/pi-v1.3.0/README.md` | Registry/manifest/artifact/scenario shapes and the lane extension procedure |
| Boundary test | `tests/evidence/NamedEvidenceTest.py` | Committed bundle, fail-closed negative cases, preservation, real capture path |
| CTest entry point | `cmake/tests/EvidenceTests.cmake` | `cch_pi_v130_named_evidence` |

The verifier generalizes the named-bundle discipline of [ADR 0065](../../adr/0065-bind-pi-ai-evidence-bundles-to-a-named-baseline-registry.md)
and `scripts/tui/evidence.py` rather than adding a competing proof format: a named baseline owns a
full revision, a manifest pins the capture and the exact artifact bytes, an artifact envelope binds
its source, environment and scenario observations, and a read emits only bytes verified through
that chain. Historical `fixtures/pi-ai/` and `fixtures/pi-tui/` registries, bundles and tooling are
untouched, so no historical evidence identity is rewritten or relabelled.

## Genuine capture

`fixtures/pi-v1.3.0/audit-1046/capture-extension-example.mts` imports the frozen
`@earendil-works/pi-tui` package entry (`packages/tui/src/index.ts`) from a checkout that must be
at the registered full revision and have a clean working tree. It refuses a non-Linux/x64/glibc
host and a family already owned by another baseline, and writes only after every guard and
observation succeeded.

Observed on the frozen checkout (`node v26.12.0`, `tsx 4.22.1`, glibc 2.44):

| Scenario (upstream test provenance) | Inputs | Observed result |
| --- | --- | --- |
| `empty query matches everything with score zero` (`packages/tui/test/fuzzy.test.ts`) | query `""`, text `"anything"` | `{matches: true, score: 0}` |
| `word boundary scores better than interior match` (`packages/tui/test/fuzzy.test.ts`) | query `"fb"`, texts `"foo-bar"` / `"afbx"` | `{matches: true, score: -18.6}` / `{matches: true, score: -4.7}` |
| `filter sorts results by match quality` (`packages/tui/test/fuzzy.test.ts`) | query `"app"`, items `["a_p_p", "app", "application"]` | `["app", "application", "a_p_p"]` |

Provenance recorded in the bundle:

- `sourceEndpoints`: `packages/tui/src/fuzzy.ts:fuzzyMatch,fuzzyFilter`,
  `packages/tui/test/fuzzy.test.ts:fuzzyMatch,fuzzyFilter`.
- `capturedAt`: `2026-10-19T15:30:56.095Z`.
- Generator SHA-256: `ede82a725ba7ca5c72a89a4131bc4ab554d7f3c6359d5607da37805448bc199c`.
- Artifact SHA-256: `5a31493abee0d47bd1d23a07822abe4abc87c22c1413c7974e10ed848cd15ff2`
  (1970 bytes).
- Manifest SHA-256: `db14038aff4315eb84f12a46af3fcee63b80214b456d5870ef1d956dc285daea`.

The values are the running frozen surface, not a Pike-generated expectation. The example
deliberately does not claim toolkit capability coverage; those rows remain with the toolkit and
Native TUI audit tickets.

## Verification boundary

`verify` reads only the selected baseline's registered families under `fixtures/pi-v1.3.0` and
fails closed on each of these cases, which the boundary test exercises with synthetic fixtures
(so the negative cases do not depend on the committed observation):

| Case | Check that fails |
| --- | --- |
| Missing bundle or manifest | `missing evidence bundle` |
| Unknown baseline / unrecorded family or artifact | `unknown baseline` / `artifact not recorded` |
| Short or differing revision | `40-character revision` / `source revision mismatch` |
| Manifest altered after pinning | `evidence manifest digest mismatch` |
| Artifact altered after pinning | `artifact SHA-256 mismatch` |
| Artifact from another source with valid re-pinned digests | `artifact source revision/baseline mismatch` |
| Absolute or `..` artifact/manifest path | `invalid evidence path` |
| Duplicate scenario name / missing source endpoints | `duplicate evidence scenario name` / `missing source endpoints` |
| Historical observation parked in the fixture root | still `missing evidence bundle`; no fallback read |
| Family reused under another baseline | `already belongs to baseline` |
| Unverified candidate manifest registered | `artifact SHA-256 mismatch`; registry unchanged |
| Recapture of a registered family without `--replace` | `pass --replace` refusal |

`read --family <family> --artifact <path>` runs the full verification first and emits only the
verified bytes; failures print to stderr with empty stdout. `verify --pi-root <checkout>` also
compares the checkout's full `HEAD` with the registered revision. Selection is by baseline name
only: a free-form revision argument does not exist, so an operator cannot verify one family
against another baseline's checkout.

## Normalization

The example records `normalizations: []` for every scenario: the observations are pure public
function results and needed no environment normalization. The lane convention, carried from the
run-scoped audit protocol, is that a normalization may remove only declared noncontractual
environment variation (wall-clock capture timestamps, machine paths, permitted numeric
tolerance). It never removes timing-field presence, a missing event, an ordering change, an
error, a warning, a state transition, malformed content or a cancellation outcome, and an
undeclared normalization is a capture defect.

## Preserved boundaries

- `fixtures/pi-ai/baselines.json` keeps default `pi-v0.87.1`, its existing baseline identities
  and its recorded capture times; `pi-v1.3.0` is not registered there.
- `fixtures/pi-tui/baselines.json` keeps `pi-v1.0.4` at
  `7c10bd4337495ee613f2224843ecdf349b80d1df`.
- No live credentials, provider catalogs or `~/.pi` state enter this bundle; the capture uses the
  frozen checkout's public package entry and deterministic function inputs.

## Validation

```bash
python3 tests/evidence/NamedEvidenceTest.py
PI_CHECKOUT=<frozen checkout> python3 tests/evidence/NamedEvidenceTest.py
python3 scripts/evidence/audit_evidence.py verify
ctest --preset vcpkg -R cch_pi_v130_named_evidence
```

Offline the committed bundle and every fail-closed negative case run; the two capture-path cases
report a skip when no frozen checkout with `tsx` is available. With the frozen checkout present,
the end-to-end case captures into a sandbox, registers, verifies and reads, and the
wrong-revision guard case proves the capture refuses before writing.

Focused results on the ticket commit (the exact hash is in the ticket handoff):

| Command | Result |
| --- | --- |
| `python3 tests/evidence/NamedEvidenceTest.py` | 18 tests, 16 passed + 2 capture-path skips |
| `PI_CHECKOUT=<frozen checkout> python3 tests/evidence/NamedEvidenceTest.py` | 18 tests, all passed (capture, register, verify, read; wrong-revision refusal) |
| `ctest --preset vcpkg -R cch_pi_v130_named_evidence` | 1/1 passed |
| `PI_CHECKOUT=<frozen checkout> ctest --preset vcpkg -R cch_pi_v130_named_evidence` | 1/1 passed |
| `python3 scripts/evidence/audit_evidence.py verify --pi-root <frozen checkout>` | 1 bundle, 1 artifact, 3 scenarios verified; baseline `pi-v1.3.0` at `71023aec...` |
| `scripts/format-check.sh 376b25df8` | clean (no C++ lines changed) |
| Configure (`cmake --preset vcpkg`) | Parity Architecture Gate PASS; test `cch_pi_v130_named_evidence` registered |

No production C++ build is required for this evidence/tooling change; the offline test and the
registered CTest entry point are the focused meaningful selection.

## Remaining gaps

- Only the `audit-1046` example family is captured. The agent, application Session, nonterminal
  application, toolkit, Native TUI, MCP, codemode, durable storage/tasks/Harness and Chord
  families are the downstream audit tickets' obligations; this ticket does not claim complete
  v1.3.0 evidence.
- The evidence is `upstream-captured`, not C++-verified. No capability row advances to `verified`
  on the strength of this capture.
- No production code, package move or architecture graph change is included.
