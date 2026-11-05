# Build readiness for #1202 re-anchoring, 2026-11-05

State of the build environment for re-anchoring the 13 withdrawn #1202 rows and the later
receipt work. Read-only assessment recorded by the implementation sub-agent; **no build was
started**. Facts checked 2026-11-05 against the working tree at `caf830ce4` (current branch
HEAD, not pushed). Conventions: [conventions](conventions.md).

## 1. Which build preset the receipt tests use

Every active receipt in [`1058-deliveries.json`](../../research/pi-v1.3.0/1058-deliveries.json)
validates with `ctest --preset vcpkg` (label or name-regex selections, JUnit via
`--output-junit docs/research/pi-v1.3.0/deliveries/*.xml`), and
[validation](../../agents/validation.md) names the `vcpkg` Debug preset as the default for
Focused and Full Validation:

- configure: `cmake --preset vcpkg` (only if `build/` is not configured),
- build: `cmake --build --preset vcpkg [--target <shard>]`,
- test: `ctest --preset vcpkg -L <label>` / `-R <regex>`.

The receipt labels used by the withdrawn rows' re-anchoring are `issue1090` (2 rows),
`issue1118` (3), `issue1179` (4) and `issue1181` (4); the fully withdrawn #1104 record's
re-anchoring would use `issue1104`/`issue1101`-class selections over
`tests/coding_agent/McpSessionManagerTest.cpp` and `McpManagerFlowTest.cpp`. `CMakePresets.json`
maps the `vcpkg` preset to `binaryDir = ${sourceDir}/build` (Debug), with `vcpkg-release`,
`clang-conformance`, `vcpkg-asan-ubsan`, `vcpkg-tsan` and the `dev-fast` family as siblings;
`CMakeUserPresets.json` adds `vcpkg-ccache` variants.

## 2. `build/` state and age

`build/` exists at the repo root and is a configured `vcpkg`-preset tree:

- `build/CMakeCache.txt` — 2026-11-02 08:20:46 +0900 (configure time).
- `build/build.ninja` — 2026-11-02 23:12:20 +0900 (last generation/build).
- vcpkg dependencies installed under `build/vcpkg_installed/x64-linux` (asio, boost, catch2,
  fmt, glaze, …); root `CTestTestfile.cmake` present.

Age versus `HEAD` (`caf830ce4`, 2026-11-05 02:00 +0900): the tree is **~2 days stale** and,
more importantly, **not current for receipt work**:

- Commits after 2026-11-02 23:12 touched C++ inputs — `tests/agent/AgentTest.cpp`,
  `tests/agent/AuditArtifact.hpp`, `tests/codemode/CodemodeOwnerTest.cpp`,
  `tests/codemode/CodemodeRemoteTest.cpp`, `tests/coding_agent/CodemodeSandboxTest.cpp`,
  `src/durable/CodingTools.cpp`, `src/durable/Environment.cpp`,
  `src/durable/include/cch/durable/Environment.hpp`, `tests/durable/EditToolTest.cpp`,
  `tests/durable/HarnessCodemodeTest.cpp`, `tests/durable/HarnessWorkflowTest.cpp`
  (commits `12be64a25`, `218c6ca18`, `1f6a0b753`, `b9b7c1fb2`, `2ba9bc9d5`, …).
- The tree's registered Catch2/CTest labels (`grep LABELS build/**/CTestTestfile.cmake`) are
  `issue1044 issue1046 issue1058 issue1106 issue441 …` — the receipt labels
  (`issue1090/1101/1104/1118/1179/1181`) are **not registered in the stale tree**, so the
  pinned receipt commands would select nothing (the `vcpkg` test preset fails closed on an
  empty selection). `catch_discover_tests` only re-registers when the targets rebuild.
- No CMake-input changes (`CMakeLists.txt`, `cmake/`, presets, `vcpkg.json`) since the
  configure, so an **incremental** `cmake --build --preset vcpkg` is the expected path; the
  build-phase `PARITY-4011` include-scan gate will re-derive include evidence automatically,
  and a changed include set fails there until a reconfigure — chain build and test with `&&`.

## 3. cmake / vcpkg toolchain availability

All present on the host: `cmake 4.4.5`, `ninja 1.13.2`, `gcc/g++ 16.2.1 20260810`,
`clang` (conformance lane), `ccache 4.14.1` (cache 7.4/12 GB, 82% hit rate over 1.03M
cacheable calls), 8 cores, 40 GB free on `/home`.

`VCPKG_ROOT` is **unset**; the preset's fallback `.deps/vcpkg` exists in this worktree and sits
at exactly the pinned `vcpkg.json` `builtin-baseline` `2f1d605400c8727cc00c15797aba796c88ccd523`
(verified `git rev-parse HEAD`). Dependency install is warm in `build/vcpkg_installed`.

## 4. PI_CHECKOUT sibling with working esbuild

`PI_CHECKOUT` is unset in the environment. The frozen checkouts recorded in the v1.3.0
evidence (`/tmp/cch-*-pi-frozen`) no longer exist. The supported default per
[`fixtures/pi-ai/README.md`](../../../fixtures/pi-ai/README.md) ("`PI_CHECKOUT` overrides the
default sibling `../pi`") is present and usable:

| path | HEAD | working tree | node | esbuild |
| --- | --- | --- | --- | --- |
| `../pi` (`/home/lansy/Work/github/coding-agent/pi`) | `71023aec1` "Release v1.3.0" | clean | v26.13.0 | 0.28.2 (runs) |
| `../pi-frozen-1049` | `71023aec1` "Release v1.3.0" | clean | v26.13.0 | 0.28.2 (runs) |

`71023aec1546d6d677a65669c5665c0927ce0dd0` is the frozen pi-v1.3.0 baseline named by the
registry and every receipt, so capture/replay tooling can run against the default sibling with
no setup.

## 5. What is missing to make re-anchoring cheap

1. **A current build tree.** Run one incremental `cmake --build --preset vcpkg` at `HEAD`
   (~2 days of C++ deltas; the stale tree cannot even register the receipt labels). With the
   warm vcpkg install and 82%-hit ccache this is the "warm build" the proposal's environment
   improvements ask for, but it has not been done since 2026-11-02.
2. **The re-anchored comparisons are new C++ test code, not reruns.** Each withdrawn claim
   fails because the cited test never reads its registered frozen artifact: the #1090 image
   rows need `audit-1056/environment.json` actually opened (today only
   `audit-1090/images.json` is read via `tests/durable/ImageExpectation.hpp`); the #1118
   catalog rows need the discriminating cases to compare `audit-1053/application-catalog.json`
   instead of literals; the #1179 rows need `CodemodeOwnerTest.cpp` reading
   `audit-1053/worker-lifetime.json` and the timeout case reading
   `audit-1053/script-execution.json`; the #1181 rows need `McpSessionIntegrationTest.cpp`
   reading `audit-1052/application-initialization.json` and `McpOAuthSessionTest.cpp` reading
   `audit-1052/oauth-lifecycle.json` (per the `1202-withdrawn-*-claims.json` records and
   [1202-1104-evidence-integrity](../../research/pi-v1.3.0/1202-1104-evidence-integrity.md)).
   Each row therefore needs: test edit → owning-shard build → exact selection run → committed
   JUnit → delivery record at the new `testedRevision` with inputs re-pinned per the
   [delivery contract](../../research/pi-v1.3.0/1058-delivery-reconciliation.md).
3. **The automated #1202 boundary check does not exist yet.** Acceptance check 7 of the
   reconciliation proposal requires an evidence-suite check that a receipt's test actually
   reads its registered artifact before any re-anchored receipt is trusted; `tests/evidence/`
   today holds only `ConsolidationAuditTest.py`, `NamedEvidenceTest.py` and
   `TuiReconciliationTest.py`. Building this check is a precondition for trusting the new
   comparisons, and it is pure Python — no C++ build needed.
4. **Receipt pinning hygiene.** The re-anchored records must carry `testedRevision` of the
   commit that contains the new tests (receipts currently pin older revisions such as
   `1059713caf…`/`320df81d2…`), and only the touched records' pins change — never bulk-rehash.

Bottom line: toolchain, dependency install, frozen pi checkout and esbuild are all in place;
the cheap path is one incremental build plus the pure-Python boundary check first, then the
four test-file re-anchoring slices in receipt order (#1090, #1118, #1179, #1181).

Related: [reconciliation proposal](reconciliation-1183-proposal-2026-11-04.md), [owner
decisions](decisions-2026-11-04.md), [index](index.md).

## 2026-11-05 — Build receipt (implementation sub-agent run)

**HEAD:** `caf830ce4` — feat(evidence): apply the owner ambiguity rulings to the ticketMappings plan per #1183
**Build:** `cmake --build --preset vcpkg` — SUCCESS, exit 0. Started 02:07:06+09:00, finished 02:13:19+09:00 (≈6 min 13 s, 116 ninja edges; final edge the Parity Architecture Gate). A follow-up no-op invocation confirmed `ninja: no work to do` / gate PASS (`build/parity-build-gate.json`). Warnings only (e.g. one `-Wunused-function` in `tests/tui/SystemThemeTest.cpp`); no errors. No source files changed.

**Test inventory (ctest --preset vcpkg -N, listing only — no tests executed):** Total Tests: 4253.

| Receipt label | Tests found (-L) | Status |
|---|---|---|
| issue1090 | 10 | present |
| issue1118 | 19 | present |
| issue1179 | 5 | present |
| issue1181 | **0** | **MISSING** |
| issue1189 | 8 | present |
| issue1104 | 12 | present |
| issue1101 | 14 | present |

**issue1181 finding:** no test carries an `issue1181` Catch2 tag, so the CTest label is absent
(labels are propagated via `ADD_TAGS_AS_LABELS`). Test **#1181** in the listing exists only by
slot numbering — `tests/tui/ImageTest.cpp:347` "Unsupported and malformed images render bounded
themed fallback" is tagged `[tui][image][issue53][issue994][spec]`, unrelated to receipt #1181.
Reported as-is; no code or test changes made (per task scope).

**Frozen `../pi` checkout (read-only):** `node --version` → **v26.13.0**;
`node_modules/.bin/esbuild --version` → **0.28.2** (matches `node_modules/esbuild/bin/esbuild`).

**Failures:** none. Build green; six of seven receipt labels present; `issue1181` label gap
flagged above for the owning shard to resolve.

## 2026-11-05 — #1202 boundary check execution note

**Built:** `scripts/evidence/receipt_boundary.py` (read-only; `SourceMatcher` seam with a static
name+path matcher over comment-stripped source, quoted-include closure under `tests/`) plus
`tests/evidence/ReceiptBoundaryTest.py` (19 tests: matcher true/false positives, sub-directory
path forms, fail-closed unknown claims, CLI exit codes). Recorded in
[consolidation README](../../fixtures/pi-v1.3.0/consolidation-1058/README.md). Nothing pushed;
ledger/deliveries untouched.

**First full run, all 14 active receipts, `--baseline pi-v1.3.0`:** exit **1** (fail-closed, as
designed — 12 of 14 receipts fail). PASS: `1101-structured-content`, `1176-agent-comparisons`.
The check rechecks every row #1202 named: #1118's 3 withdrawn rows, #1179's 4 (timeout-option +
3 worker rows), #1181's 4, #1090's 2 `audit-1056/environment.json` comparisons — all FAIL as
withdrawn, and row 72 of #1118 passes (its discriminating case lives in the anchored
`CodemodeCatalogTest.cpp`). #1189/#1104 are whole-record withdrawals, not active receipts.

**Beyond the withdrawn set, the same drift class is widespread** (cited tests compare literals;
the artifact appears only in comments/tags): all chord receipt claims (`1178`, `1185`, `1186`,
`1187`, `1188`, plus the `ChordContextTest` row of `1184`), the codemode inline and remote rows
of `1179`, `1181`'s active `mcp-http-transport`/`mcp-oauth-issuer` rows, two `1182` TUI rows
whose discriminating case cites `ITuiTest.cpp`, and three `1177` rows citing
`CodemodeCatalogSessionTest.cpp`. In total **67 of 109 claims** have at least one unanchored
reference; the 2026-11-04 manual heuristic counted comment mentions, so the 96 "still-verified"
rows were never independently anchored. Full output archived by the requesting session; per-receipt
unmatched references are listed by the tool itself (`--receipt <id>` isolates one).
