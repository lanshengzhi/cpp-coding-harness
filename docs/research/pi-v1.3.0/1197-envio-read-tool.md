# #1197/#1158/#1198/#1199/#1200 — durable EnvIO: frozen read tool and bounded tree listing

## Scope

Advances the durable environment's file IO to the frozen pi-v1.3.0 EnvIO
surfaces under [ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md)
and [#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044), with
[#1197](https://github.com/lanshengzhi/cpp-coding-harness/issues/1197) (read
tool), [#1158](https://github.com/lanshengzhi/cpp-coding-harness/issues/1158)
(listing bounds), [#1198](https://github.com/lanshengzhi/cpp-coding-harness/issues/1198)
(ignore rules), [#1199](https://github.com/lanshengzhi/cpp-coding-harness/issues/1199)
(discovery) and [#1200](https://github.com/lanshengzhi/cpp-coding-harness/issues/1200)
(differential gate) as tickets. Named baseline: `pi-v1.3.0` /
`71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64 with glibc.

This slice follows the #1158 partial slice (`1059713ca`, paged directories and
positioned byte reads, recorded in [1158-envio-bounds](1158-envio-bounds.md)).
It owns no whole capability row: `cch_durable:node-filesystem` and
`cch_durable:builtin-tools` remain with #1180's whole-row durable receipt.

## Delivered behavior

- **Read tool (#1197).** The durable `read` tool honors the frozen one-based
  line `offset`/`limit` (`packages/durable/src/tools/read.ts`), fails with
  `Offset N is beyond end of file (M lines total)` when the offset passes the
  newline-past line, and bounds the default output at 50 KiB / 2000 lines with
  the frozen truncation details, diagnostics and sed fallback, including the
  frozen quirks (`lines total` counts newlines + 1; a zero or negative limit
  selects like `Array.prototype.slice`; fractional indices truncate toward
  zero). A leading byte-order mark is stripped and not counted. The scan is
  streaming: a 256 MiB sparse file raises `VmHWM` by less than 64 MiB. The
  tool runs on the context hook so the frozen `details.truncation` record and
  the rendered diagnostics block reach the stored message.
- **Tree listing (#1158, #1198, #1199).** `Environment::list_files` walks a
  tree depth first, siblings in byte order, with the frozen `maxEntries`
  (default 50,000), `maxBytes` (default 4 MiB, clamped to the 16 MiB − 64 KiB
  payload ceiling), `timeoutMs` (default 10 s) and `truncated` reporting.
  `.git` and `exclude` names are always skipped with their subtree; symlinks
  list as links and are never followed; FIFOs and other kinds are skipped;
  non-UTF-8 names render with U+FFFD. The ignore engine implements the frozen
  `ignore`-crate semantics: the full rule syntax (comments, escapes, trailing
  spaces, negation, anchoring, directory-only, `*`/`?`/byte classes, `**` in
  all positions, brace alternation as a globset extension, case-sensitive
  bytes), the precedence (nearest `.gitignore`, then `.git/info/exclude`, then
  the global file; last rule wins; whitelist override; ignored-subtree
  pruning; repository boundary), and the discovery (`.git` directories,
  `gitdir:`/`commondir` files, parent rules up to the work-tree root,
  `core.excludesFile` through `GIT_CONFIG_GLOBAL`, `$HOME/.gitconfig`, XDG and
  the system config, `~` expansion, XDG default). `GIT_DIR`/`GIT_WORK_TREE`
  are not consulted, exactly like frozen. Rule memory is bounded (4 MiB of
  rule text, 50,000 rules) with a typed Validation error naming the bound;
  cancellation is observed every 64 entries and around each directory read.
- **Differential gate (#1200).** A test asset
  (`tests/durable/EnvIoListingDifferentialTest.cpp`) builds the fixture
  matrix, runs the frozen daemon built from the baseline checkout
  (`cargo build --locked --offline`, binary SHA-256
  `df129674f40ccf1e845eb01aa95f8e598c1f20132504bdf6825102cf83095380`) and the
  C++ listing over the same trees, and compares the payload records, the entry
  count and the `truncated` flag byte for byte, reporting the first
  divergence. It documents a skip when the daemon is unavailable.

### Recorded divergences from frozen (intentional, policy-bound)

- The frozen `openBinaryReader` follows a final-component symlink unless
  asked not to; the C++ durable read stays no-follow per
  [architecture](../../agents/architecture.md) §security (no-follow symlink
  policy). #1197's acceptance does not involve symlinks.
- Config and rule-file reads are bounded (1 MiB per config, 1 MiB per rule
  line); oversized inputs degrade like a missing file. Frozen reads them
  unbounded; the bounds are the issue-mandated C++ safety properties and are
  documented in `GitIgnore.hpp`.

## Frozen evidence

Three candidate families, each captured from the frozen checkout at
`71023aec…` (verified HEAD, clean tracked sources), Linux x86-64 / glibc /
Node v26.13.0 / tsx 4.22.1, and validated with
`scripts/evidence/audit_evidence.py check`:

| Family | Artifact | Scenarios | Bytes | SHA-256 |
|---|---|---:|---:|---|
| `audit-1197` | `read-tool.json` | 24 | 567,439 | see manifest |
| `audit-1198` | `ignore-rules.json` | 26 | see manifest | see manifest |
| `audit-1199` | `discovery.json` | 14 | see manifest | see manifest |

`audit-1158` (paging and positioned reads) stays untouched; the new families
extend its audit identity without overwriting captures. Registration of the
three new families in `fixtures/pi-v1.3.0/baselines.json` remains with the
serial merger, as does the orphan classification of their scenarios and the
#1180 whole-row durable receipt.

## Validation

On testedRevision `e362f453fa21` (implementation commits `a009ff020` +
`e362f453fa`), the default strict Debug preset:

- Focused selection `[issue1197]`: 4 cases, 244 assertions, all pass; the
  24 audit-1197 scenarios replay through the registered tool (content,
  rendered diagnostics and `details.truncation` compared against the
  artifact), plus the 256 MiB sparse-file memory bound and the
  offset-past-end discriminating case.
- Focused selection `[issue1198]`, `[issue1199]`: 4 cases, all pass; every
  audit-1198/1199 scenario rebuilds its recorded tree and compares every
  entry path, kind and the `truncated` flag, with the rule-budget typed
  errors and the cancellation case.
- Focused selection `[issue1158]`: the unbounded-directory memory bound case
  passes (20,000 entries, `maxEntries` 100, `VmHWM` rise < 8 MiB). The
  pre-existing `[issue1158]` paging/positioned-read cases in
  HarnessWorkflowTest pass unchanged.
- Focused selection `[issue1200]`: the differential gate passes every
  scenario (work-tree rules and kinds, hidden off, gitignore off, the bound
  zero/exact/over matrix, the git-file worktree, the nested repository and
  the name exclusion) with the daemon digest recorded above.
- Owning module: `ctest --preset vcpkg -L durable`: 361 selected, 361 passed
  (9 pre-existing frozen-pi continuation prerequisites skipped). The
  architecture selection: 43/43.
- Full Validation: `cmake --build --preset vcpkg` then `ctest --preset
  vcpkg`: 4,262 selected, 4,258 passed, 4 failed — the environment-only
  `real PTY records Herdr markdown and OSC 52 terminal bytes` case requires
  `TERM_PROGRAM=herdr` (passes under it), and the two
  `cch_pi_v130_consolidation_*` cases failed before this record's
  revalidation updated the pinned inputs (they pass after; see below).
- Receipt revalidation: the 1090 record pins `CodingTools.cpp`,
  `Environment.hpp`, `Durable.cmake` and `DurableTests.cmake`; the 1184
  record pins `HarnessWorkflowTest.cpp` plus the two CMake files. Both
  selections were rebuilt and rerun at the new testedRevision into
  `deliveries/1197-revalidation-1090.xml` (36/36) and
  `deliveries/1197-revalidation-1184.xml` (10/10), re-pinned under
  `revalidation1197` in [1058-deliveries.json](1058-deliveries.json). No new
  capability row is claimed and no routing digest changed.
- Gate: `consolidate_audits.py build` and `check` pass;
  `tests/evidence/ConsolidationAuditTest.py` OK (42 cases);
  `tests/evidence/ReceiptBoundaryTest.py` OK (19 cases) and
  `scripts/evidence/receipt_boundary.py` exit 0 (14 receipts, 109 claims,
  208 comparisons); `scripts/evidence/audit_evidence.py verify` covers 57
  registered bundles / 118 artifacts / 661 scenarios;
  `scripts/format-check.sh` and `git diff --check` clean.

## Capability status

No capability row is claimed and no ledger membership changes: the
comparisons advance the EnvIO gap recorded against #1089/#1158, while
`cch_durable:node-filesystem` and `cch_durable:builtin-tools` keep their
status until #1180's whole-row receipt. The three new families stay
unregistered by design; the serial merger's registration and orphan
classification is the follow-up, demonstrated by the gate staying green with
the families present but unregistered.

## Remaining boundary

- The frozen read tool's change-detection retry (read again when the file
  changed mid-read) is not implemented; the C++ reader opens per call, as the
  positioned reads already documented.
- `readHead` chunk-boundary equivalence beyond the captured cases and exotic
  invalid-UTF-8 accounting rely on the decoder shared with the exec path.
- The `hidden:false` whitelisted-hidden interaction, timeout truncation and
  the daemon's exact payload accounting are pinned by the artifacts; the
  timeout path is covered by the audit-1198 zero/60 s cases rather than a
  wall-clock differential.
- Whole-row durable verification, family registration and the
  `cch_durable:node-filesystem` / `cch_durable:builtin-tools` receipts remain
  with #1180 and the serial merger.
