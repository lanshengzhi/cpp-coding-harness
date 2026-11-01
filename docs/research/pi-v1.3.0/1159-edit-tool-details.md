# #1159 — durable edit: BOM/CRLF repair, file mutation queue and diff details

## Scope

Delivers the frozen `edit` tool's remaining durable behaviour through the public
Harness tool surface, under [ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md)
and the [#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044)
delivery, with [1159](https://github.com/lanshengzhi/cpp-coding-harness/issues/1159)
as its ticket. Named baseline: `pi-v1.3.0` /
`71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64 with glibc.

[#1089](1089-local-environment.md) delivered the injected environment and the
read/write/edit/bash registrations. Its edit path matched `edits[].oldText`
against the raw file text with unique/overlapping semantics and returned a plain
success line. It performed no BOM or CRLF repair, had no per-file mutation queue,
returned no diff detail and used runtime-invented rejection text. Those four
gaps are this slice. Whole-row verification, capability promotion and the
consolidated ledger rebuild stay with
[#1180](https://github.com/lanshengzhi/cpp-coding-harness/issues/1180).

## Frozen contract

Source of truth at the pinned revision:

- `packages/durable/src/tools/edit.ts` — `createEditTool`: strip the BOM, detect
  the dominant line ending, normalize to LF, match every edit against that
  normalized original, restore the ending, write, and return
  `{diff, patch, firstChangedLine?}` as `details`.
- `packages/durable/src/tools/edit-diff.ts` — `stripBom`, `detectLineEnding`,
  `normalizeToLF`, `restoreLineEndings`, `applyEditsToNormalizedContent`, the
  single-edit and indexed rejection texts, `generateDiffString` (jsdiff
  `diffLines` plus the numbered display format) and `generateUnifiedPatch`
  (jsdiff `createTwoFilesPatch`, four-line context, file headers only).
- `packages/durable/src/tools/file-mutation-queue.ts` — `withFileMutationQueue`,
  the process-wide per-file chain that serializes `edit` and `write` of one file
  by file system identity and canonical path.

The independent capture in `fixtures/pi-v1.3.0/audit-1159/` records ten
scenarios from frozen pi at the named revision through the public Harness over a
real `NodeExecutionEnv`: a BOM+CRLF file edited with LF old text, a single
replacement's diff/patch/first-changed-line, a two-digit line that pins the
gutter width, two concurrent edits of one file, the duplicate and absent-text
rejections (single and indexed), the no-change rejection, the empty-old-text
rejection and the overlap rejection. The capture resolves the baseline from
`baselines.json`, refuses a checkout whose `HEAD` is not the registered revision
or whose tracked tree is dirty, re-executes under that checkout's `tsx`, and
writes the artifact and manifest itself. Expected text, details and file bytes
come from that capture, not from C++. Its two normalizations are that no value
carries a temporary path and that a round's results are listed in call-id order,
because concurrent tool tasks settle in scheduler order.

A measurement recorded during the capture: frozen's two-call round is
order-independent only when the calls cannot observe each other. Replacing one
file's `alpha`→`beta` and then `beta`→`gamma` concurrently makes the frozen
outcome depend on which tool task the scheduler starts first; the recorded
concurrency scenario therefore uses two targets sixteen lines apart, which keeps
every captured value (including each call's diff context) independent of the
order the queue runs them in, while the final file still proves both landed.

## Delivered behavior

`src/durable/EditDiff.{hpp,cpp}` mirrors frozen `edit-diff.ts`:

- `strip_bom`, `detect_line_ending`, `normalize_to_lf`, `restore_line_endings`,
  and `apply_edits_to_normalized_content`, which normalizes each `oldText` and
  `newText` to LF, rejects an empty target, matches every edit against the
  original LF content (not incrementally), rejects a text found more than once
  with frozen's non-overlapping occurrence count, rejects overlapping edits, and
  rejects a replacement that changes nothing. Every rejection carries frozen
  pi's exact wording, including the `edits[i]` indexed forms used when a call
  carries more than one edit.
- `generate_diff_string` reproduces the jsdiff line diff and the frozen numbered
  display form: the four-line context window, the `...` elision, the gutter width
  computed from `content.split("\n")` (so a trailing newline widens it), and no
  trailing newline. `generate_unified_patch` reproduces jsdiff
  `createTwoFilesPatch` with `FILE_HEADERS_ONLY` and four context lines,
  including zero-length hunks and the `\ No newline at end of file` marker.
- The line diff is a faithful port of jsdiff's greedy forward Myers walk
  (path-selection rule, component list, diagonal pruning), not an LCS
  approximation, because a display diff over repeated lines has more than one
  minimal form and only the frozen tie-breaking reproduces the recorded values.
  The walk is bounded (2,048 edit lengths, 2Mi components): a pathological
  rewrite that exceeds it falls back to the common prefix and suffix around one
  whole-region replacement instead of spending unbounded time or memory.

`src/durable/FileMutationQueue.{hpp,cpp}` is the frozen queue: one FIFO slot per
file for the whole process, keyed by the environment's absolute path canonicalized
where the host resolves it (`stat` gives the resolved path; an unresolvable path
falls back to its given spelling). `edit` takes the slot for its whole
read-modify-write and `write` takes it too (frozen `write.ts` shares the queue,
so a write and an edit of one file cannot interleave either), so a concurrent
pair applies in arrival order instead of losing an update; other files never
wait. The slot is
retired when the last holder leaves, so a long-lived process holds one entry per
file being written rather than one per file ever written.

`src/durable/CodingTools.cpp` now registers `edit` through the context-aware
`execute_durable` hook, which is the only hook that can return `details`. The
edit path repairs BOM and line endings, applies the edits, writes with the
original BOM and dominant ending restored, and returns
`{"diff", "patch", "firstChangedLine"}`. A rejection or environment failure
becomes a tool result with `isError=true` and the frozen
`<harness>\n[error] …\n</harness>` content and no details, so the run continues
as frozen pi's does.

## Tests

`tests/durable/EditToolTest.cpp` adds three cases:

- `Durable edit tool reproduces the frozen BOM CRLF diff and rejection results`
  (ten sections, one per captured scenario) drives the public Harness with the
  coding-tools extension over a real local environment, and compares every
  recorded `pi.tool-result` — `isError`, model-visible text, `diff`, `patch`,
  `firstChangedLine` — plus each observed file's bytes against the artifact.
- `Durable edit and write calls of one file apply one after another` starts one
  edit, waits until it is inside its read, starts a second edit that names the
  same file with a different spelling (`./queued.txt`), and releases the first.
  The second call replaces text only the first call wrote, the final file is
  `gamma\n`, and the instrumented environment reports one read in flight at a
  time. The discriminating case is the same test with the slot key reduced to the
  raw spelling: the follower then runs concurrently, reads the original text and
  fails with frozen's not-found message, so the case separates a canonical key
  from a string key rather than merely passing on one spelling.
- `Durable edit rejects text that appears twice or is absent` pins the two
  acceptance rejections as tool results: the duplicate and absent messages, no
  details, and the file unchanged.

## Validation

Measured with the default strict Debug preset on the delivered tree: the merge of
`origin/main` at `706ac9a3f` (the #1175 abandon-on-restart slice and its receipt
repair, evidence-only over `7e2ccc288`) into `e8d813a0c`. Every selection below
was rerun on that merged tree after the review fixes and after the final merge.

- Focused Validation: `ctest --preset vcpkg -L issue1159` selected **3** cases
  and passed **3/3**.
- Owning module: `ctest --preset vcpkg -L durable -j4` selected **330** cases and
  passed **330/330** on the merged tree (one rerun during another worktree's full
  validation, at load average 15, hit two of the runner-sensitive cases below;
  both passed on repeat).
- JS-diff parity probe: `src/durable/EditDiff.cpp` was compiled standalone and
  compared against frozen `edit-diff.ts` over **2,510** generated line pairs
  (duplicate lines, blank lines, missing trailing newlines, empty inputs,
  insertions and deletions): **0** differing outputs for `diff`, `patch` and
  `firstChangedLine`, re-checked after the review fixes. A 5,000-line file with
  one changed line matched exactly in 5 ms, and a 3,000-line rewrite took the
  bounded fallback in 0.08 s.
- Architecture: `ctest --preset vcpkg -L architecture` selected **43** and passed
  **43/43**.
- Full Validation: `TERM_PROGRAM=herdr PI_CHECKOUT=<frozen> ctest --preset vcpkg
  -j1` ran twice on the delivered tree: **4,175** selected, **4,169** passed
  (first run) and **4,171** passed (second run, 1,086 seconds). The 10 live
  frozen-pi continuation cases ran and passed, and the two documented
  capture-environment cases skipped. The failures move between runs and are this
  host's documented runner-sensitive cases: the #1170 nested-call case that
  asserts a recorded completion order of two gated calls (it failed only as
  `completionOrder ["B","A"]` against the frozen `["A","B"]`, and passed 8/10
  isolated reruns), its cancelled-sibling case (`Subprocess aborted`, passes in
  the same reruns), the two #1019 `Process Terminal` PTY races, the documented
  #632 `[quarantine]` projection cost bound, and one or two Native TUI or E2E
  cases (`mcp reconnect with no name`, the focused-editor turn, cursor protocol)
  that pass in isolation. Both runs overlapped another worktree's full validation
  on this host (load average 9-13), which is exactly the condition those cases
  are recorded as sensitive to. No case in `src/durable`, `tests/durable` or any
  file this slice touches failed in either run.
- Delivery revalidation, because the new bundle and the added durable sources
  changed inputs pinned by existing receipts (all rerun on the merged tree): the
  exact
  `^(issue1090|issue1101|issue1104)$` selection reran **36** selected, **36**
  passed (`deliveries/1159-revalidation-1090-1104.xml`, shared by #1090, #1104
  and #1101); `-L issue1118` reran **19** and passed **19**
  (`deliveries/1159-revalidation-1118.xml`); the #1181 `-R` selection reran
  **16** and passed **16** (`deliveries/1159-revalidation-1181.xml`). Each
  record keeps its retained artifact and records the reviewed changed inputs in
  `revalidation1159`. The final `706ac9a3f` merge is evidence-only over
  `7e2ccc288`, so those rerun artifacts still describe the delivered binaries.
- Consolidated evidence: `audit_evidence.py check` accepts the candidate
  manifest (`c5b503ce…`), `register` pins `audit-1159` in `baselines.json`, and
  `verify` reports **50** bundles, **106** artifacts and **597** scenarios
  (previously 48 / 104 / 582 before the #1175 merge). `consolidate_audits.py build` and `check
  --pi-root <frozen>` pass (1,464 rows), and
  `tests/evidence/ConsolidationAuditTest.py` ran **42** tests and passed.
- Inherited consolidation debt, repaired because this slice rebuilds the ledger:
  `consolidate_audits.py check` was red at this branch's base (the #1172 family
  had no plan routing entry, so its eleven captured scenarios read as orphans,
  and the #1090 receipt pinned the pre-registration `baselines.json` digest).
  The #1175 merge classified the #1172 scenarios but arrived with the same shape
  for its own five, and left the ledger unbuilt. This slice records the #1175
  observations as explicit orphans naming `cch_durable:abandon-on-restart` and
  its neighbours, refreshes the #1090 digest, and rebuilds the ledger. No
  capability row, membership or routing changed.
- `scripts/format-check.sh origin/main` and `git diff --check` are clean.

## Capability status

No capability status or membership changes. The durable edit tool's row remains
whole-row verification work for #1180; this slice supplies the implementation and
its focused frozen comparison only.

## Remaining boundary

- Frozen fuzzy matching (`normalizeForFuzzyMatch`: NFKC, per-line trailing
  whitespace, smart quotes, Unicode dashes and spaces) is not reproduced: matching
  here is exact on the LF-normalized content. A call whose old text differs from
  the file only by those normalizations is rejected as not found instead of being
  repaired; durable does not take the `utf8proc` dependency the application edit
  tool uses for NFKC.
- The access path still reports the environment's own failure. Frozen checks
  `fileInfo` first and reports `Could not edit file: <path>. Error code: <code>.`
  or `Path is not a file.`; this slice keeps the read/write error message and adds
  no file-kind check.
- Frozen `prepareArguments` repair is not reproduced: `edits` as a JSON string, a
  single edit object, or a top-level `oldText`/`newText` pair are not accepted;
  the call must carry the schema shape.
- The queue serializes this runtime's own `edit` and `write` calls only. It is not
  a lock against `bash`, another process, or another machine, and a symbolic-link
  spelling of one file is not reachable through this environment because the
  durable adapter refuses to write through a symlinked path (the #1160 boundary).
- The environment and image seams of #1089/#1090 are unchanged; nothing here
  changes `Environment`, `Codemode`, `Task` or `WorkflowRuntime`.
