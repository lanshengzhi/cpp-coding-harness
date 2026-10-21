# pi-v1.3.0 application Session persistence, resume and compaction (#1048)

- **Baseline**: `pi-v1.3.0` at `71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64 with glibc.
- **Authority**: [ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md), umbrella [spec #1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044), and ticket [#1048](https://github.com/lanshengzhi/cpp-coding-harness/issues/1048).
- **Scope**: audit only. This ticket adds no production behavior, no package move, no durable-storage change and no `cch_ai` baseline change. It records frozen upstream observations, the current C++ implementation/consumer map, and the gaps that must be closed before ownership moves.
- **Evidence family**: `fixtures/pi-v1.3.0/audit-1048/`, manifest SHA-256 `2cbc07e8d2924ff92775245cd2750545da083fb07fbb1f88abf604134669eecd` (before serial registration).
- **Machine ledger**: [1048-session-persistence.json](1048-session-persistence.json).

## Method and evidence

The capture is a public-call scenario set against a private frozen checkout at the registered revision. It uses
the public package entries `packages/coding-agent/src/index.ts`, `packages/coding-agent/src/core/agent-session.ts`
and the `@earendil-works/pi-ai` compat/faux stream, with `SettingsManager.inMemory`, a temporary XDG-free
session directory, dummy auth and scripted inference. It refuses a wrong revision, a modified tracked source
tree, a non-Linux/x64/glibc host, or a family already owned by another baseline.

The frozen workspace does not contain the ignored generated shard
`packages/ai/src/providers/data/azure.json`, although the checked-in `azure.models.ts` imports it. The private
checkout supplied an empty ignored shard so the public source graph can load. No tracked source file changed.
This is recorded as `checkoutPreparation` in the manifest and is not a behavior observation.

```bash
PI_CHECKOUT=/tmp/cch-1048-pi-frozen PI_BASELINE=pi-v1.3.0 \
  /tmp/cch-1048-pi-frozen/node_modules/.bin/tsx \
  fixtures/pi-v1.3.0/audit-1048/capture-session-persistence.mts
python3 scripts/evidence/audit_evidence.py check \
  --manifest fixtures/pi-v1.3.0/audit-1048/manifest.json
```

The capture recorded the following observations on `node v26.12.0`, `tsx 4.22.1`, glibc 2.44:

| Artifact | Scenario | Observed |
| --- | --- | --- |
| `session-tool-restore.json` | activation persists the added tool declaration | initial active `["read","bash"]`; after explicit activation `["read","bash","write"]`; transcript declarations `["read","bash"]` then `["write"]` |
| `session-tool-restore.json` | resume applies relative modifiers to the recorded loadout | `tools: ["-bash","+edit"]` over transcript `["read","bash","write"]` → active `["read","write","edit"]`; thinking `medium` and model `faux-tools/faux-1` restored |
| `session-tool-restore.json` | resume without modifiers restores the recorded order | active `["read","bash","write"]`; the session file is byte-identical before and after both resumes |
| `session-compaction-boundary.json` | summary serialization omits thinking while storage retains it | both assistant thinking blocks remain in the JSONL; the two summarization requests and the appended summary contain no thinking |
| `session-compaction-boundary.json` | compaction appends without rewriting history | 8 original lines remain a byte-identical prefix; exactly one `compaction` line is appended |
| `session-compaction-boundary.json` | rebuilt context uses the captured boundary | projection roles `["system","compactionSummary","assistant"]`; the system message carries `toolsAdded:["read","bash"]` and all prompt sections |
| `session-tree-and-fork.json` | usage/context_edit entries do not rewrite stored content | stored user text `original user`; projection user text `edited user`; entry list includes `usage` and `context_edit` |
| `session-tree-and-fork.json` | branchWithSummary records the abandoned leaf | active path `model_change, thinking_level_change, message, message, branch_summary`; summary `fromId` is the abandoned assistant leaf |
| `session-tree-and-fork.json` | createBranchedSession forks one path | fork entries `model_change, thinking_level_change, message, message, label`; header has `parentSession`; original file keeps all entries |
| `session-tree-and-fork.json` | in-memory sessions keep the live entry model | no file, `isPersisted:false`, message entry visible in projection |

Artifact and generator digests are pinned by the bundle manifest; the exact bytes are not duplicated here.

### What the capture proves and does not prove

- It proves frozen pi behavior for the two acceptance workflows: tool activation → persistence → resume with
  `-name`/`+name` modifiers and restored ordering/state; and a compaction that serializes a summary without
  thinking while leaving the transcript unchanged.
- It separately observes `usage` and `context_edit` entry kinds and branch/fork behavior needed by the
  consumer trace.
- It does **not** compare those values against a C++ implementation. Rows remain `upstream-captured` or
  `unverified`; no row is `verified` on the strength of this bundle.
- It does not capture cross-model thinking transformation or the additional AI retry matching. Those are
  explicit `cch_ai` exclusions and are kept as separate capability rows below.

## Baseline provenance: the C++ wire is pi-agent-core 0.83, not pi-v1.3.0 coding-agent

The most important finding is a baseline skew, not a missing field. The C++ session wire was built
against the retired pi-agent-core harness format, while this ticket's authority is the v1.3.0
coding-agent `SessionManager`.

- `fixtures/pi-agent-core/README.md:14-39` pins the older evidence to frozen pi commit
  `83114817c68f5413e4d7ba6d7003ddc511cd31d2` and names
  `packages/agent/src/harness/session/session.ts` as the session-wire authority.
- At that commit, `CompactionEntry` carries `retainedTail` and the entry union includes
  `ActiveToolsChangeEntry` and `LeafEntry` (`packages/agent/src/harness/types.ts:398-464`).
  `sessionEntryToContextMessages` projects `[compactionSummary, ...retainedTail]`
  (`packages/agent/src/harness/session/session.ts:120-140`). This is the shape the C++
  `CompactionEntryValue`, leaf marker and `retainedTail` projection implement.
- At pi-v1.3.0, `packages/agent/src` contains no `harness/session` directory. Session persistence
  moved to `packages/coding-agent/src/core/session-manager.ts`; its `CompactionEntry` carries
  `systemMessage` (`:91-104`), its entry union adds `usage` and `context_edit` (`:80-192`), and it
  has no `leaf` or `active_tools_change` entry.

C++ is therefore a hybrid: tool loadout uses the v1.3.0 coding-agent system-message declaration
(`AgentSessionExecution.cpp:55-85`), while compaction and branch position use the old
pi-agent-core wire (`retainedTail`, `leaf`). The committed golden
`fixtures/pi-agent-core/compaction-persistence.jsonl` contains `retainedTail` and no
`systemMessage`, although `CompactionTest.cpp:908` calls it “pi's CompactionEntry wire fields” and
carries the `compat-pi` tag. That test and its README are older-baseline evidence, not v1.3.0
parity proof. Re-basing them is part of the migration; adding `systemMessage` while keeping the
old golden would leave two incompatible “pi” contracts in the tree.

This is why the gap list starts with a wire-baseline decision. The rest of this report describes
the current implementation and the frozen v1.3.0 observation; it does not reinterpret the older
fixtures as current evidence.

## Actual assembly and publication

### New session

1. `SessionFactory::create_async` resolves the model, effective settings, tool policy, resources and session
   publication plan (`src/coding_agent/runtime/SessionFactory.cpp:2204`), then publishes through a reserved
   runtime worker (`:1847-1905`).
2. `SessionLifecycle::publish_session` constructs the JSONL or in-memory `SessionStore`
   (`src/coding_agent/runtime/SessionLifecycle.cpp:206-238`). JSONL creation is deferred: the header and
   pre-conversation entries are buffered until the first user or assistant message
   (`src/agent/harness/session/JsonlSessionStore.cpp:31-73`; test
   `a new session writes no file until the first user or assistant message`).
3. `SessionFactory` appends `model_change` (unless the placeholder model is active) and
   `thinking_level_change` for a new session (`SessionFactory.cpp:1932-1944`), and optional
   `session_info` (`:1947-1953`). On resume it appends `model_change` only when the resume changes the
   model and `thinking_level_change` only when the transcript has no thinking entry (`:1885-1899`). The new-session
   branch mirrors pi `sdk.ts:448-463`; the resume model-change append is a C++ persistence choice.
4. `AgentSessionExecution::build` prepends the initial structured system message and records its
   `tools_added`; it is persisted through the same commitment channel after the run starts
   (`src/coding_agent/AgentSessionExecution.cpp:188-243,316-355`).
5. A resumed session does not append a fresh initial system message; it replays the recorded tool loadout
   and keeps the Agent's live state (`AgentSessionExecution.cpp:227-243`).

### Publication and resume

- `prepare_resume_target` calls `resume_session`, which loads the JSONL tree once and builds the compaction-
  aware context (`src/agent/harness/session/SessionResume.cpp:98-120`;
  `src/coding_agent/runtime/SessionLifecycle.cpp:301-331`).
- `publish_resume_session` then calls `SessionStore::open_existing` and loads the same file **again**
  (`SessionLifecycle.cpp:340-366`; `src/agent/harness/session/SessionStore.cpp:118-136`). That is a
  bounded prefactoring gap: resume parses twice and can observe a different file between the two reads.
  `JsonlSessionStore::open_loaded` already exists as the one-parse seam, but `SessionStore` does not expose it.
- New-session publication is one `SessionStore::create_new` call; `create_new` writes the header through
  `SessionJournal` and defers the file until the first conversation append (`JsonlSessionStore.cpp:89-116`).

### Pi comparison

Pi `createAgentSession` (`packages/coding-agent/src/core/sdk.ts:191`) resolves the same inputs, derives
`initialActiveToolNames` from the explicit selection or defaults (`:282-301`), restores the transcript loadout
only when the caller uses defaults or modifiers (`:469-475,526`), and appends the initial model/thinking
entries only for a new session (`:448-463`). Pi has no separate `SessionFactory` publication object; the
`SessionManager` is the publication point and `SessionManager.create/open/inMemory` choose the backing.

**Current result**: assembly and publication are implemented in C++ and exercised by
`SessionAssemblyContractTest.cpp`, `SessionLifecycleTest.cpp`, `SessionSnapshotTest.cpp` and the capture.
The double-parse resume and the absent relative tool modifiers remain gaps.

## Entry serialization, migration and the live tree

Pi's v3 wire is append-only JSONL. `_persist` writes `JSON.stringify(entry)` and `_appendEntry` updates
`fileEntries`, `byId` and `leafId` in memory (`session-manager.ts:1172-1197`). The current entry union is:
`message`, `thinking_level_change`, `model_change`, `usage`, `compaction`, `branch_summary`, `custom`,
`custom_message`, `context_edit`, `label`, `session_info` (`:80-192`). Pi has **no `leaf` entry kind**;
opening a file sets the active leaf to the last file entry (`:1089-1122`). A branch move without a later
append is therefore not durable in pi.

C++ has a typed DTO for each supported line (`src/agent/harness/session/EntrySerializer.cpp:31-144`), keeps
unknown foreign lines verbatim (`JsonlSessionStoreTest.cpp` case `Glaze JSONL session keeps unknown future
entries`), and mirrors the exact accepted entry into `SessionTree` under one mutex
(`SessionStore.cpp:12-50,184-205`). The current `SessionEntryKind` union
(`src/agent/include/cch/agent/harness/session/SessionEntry.hpp:26-39`) has `Header` and `Leaf` but **not**
`Usage` or `ContextEdit`.

| Contract element | pi v1.3.0 | C++ current | Status |
| --- | --- | --- | --- |
| Entry order / field order | `{id,parentId,timestamp,type,...}` from `_appendEntry` | DTO declaration order matches (`EntrySerializer.cpp:31-144`) | implemented, tested |
| `parentId: null` at root | explicit null | `NullableString` monostate emits null | implemented, tested |
| `message` / `custom_message` | typed | typed | implemented |
| `custom` (not model context by default) | typed | typed | implemented |
| `label` | real tree entry with resolved label map | typed append and tree resolution | implemented |
| `session_info` | last non-empty trimmed name | typed append and `get_session_name` | implemented |
| `compaction` | `summary, firstKeptEntryId, tokensBefore, details, usage, fromHook, systemMessage?` | `summary, firstKeptEntryId, tokensBefore, retainedTail?, details, usage, fromHook` | **partial: `systemMessage` is missing; `retainedTail` is non-pi** |
| `branch_summary` | `fromId, summary, details, usage, fromHook` | typed | implemented |
| `usage` | typed, non-context entry | preserved only as `Unknown` raw line; no append API | **missing** |
| `context_edit` | typed, branch-local projection edit | preserved only as `Unknown` raw line; no projection semantics | **missing** |
| `leaf` | does not exist | typed durable leaf marker | **non-pi extension** |
| v1 → v2 migration (ids, `firstKeptEntryIndex`) | `migrateV1ToV2` (`session-manager.ts:287-315`) | no migration path | **missing** |
| v2 → v3 migration (`hookMessage` → `custom`) | `migrateV2ToV3` (`:316-336`) | no migration path | **missing** |
| Header fields | `type,version?,id,timestamp,cwd,parentSession?` (`:43-50`) | adds `provider` and `model` (`EntrySerializer.cpp:29-39`) | divergence; pi tolerates extras, C++ accepts the pi shape |
| Persistence redaction | none at `_persist` | secret-pattern redaction before mirror and wire (`EntryRedaction.cpp`, `EntrySerializer.cpp`) | intentional Pike security divergence; not removed by this audit |

The C++ compaction DTO (`EntrySerializer.cpp:120-132`) writes `retainedTail` and has no
`systemMessage`, matching the older pi-agent-core 0.83 shape rather than v1.3.0. The C++ preparation
fills `retained_tail` with every currently projected system message plus the retained chat tail
(`src/agent/harness/compaction/Compaction.cpp:1007-1018`), and `commit_compaction` persists it
(`src/coding_agent/AgentSessionCompaction.cpp:155-196`). Pi v1.3.0 instead snapshots the current
system message on the compaction entry (`session-manager.ts:1270,1278-1281`) and projects it before
the summary (`:457-465`). This is the highest-impact wire gap: a pi-v1.3.0-written compaction loses
its system/tool boundary in C++, and a C++-written compaction is not understood by pi's normal
projection.

The projection mismatch is larger than one missing field. A pi-v1.3.0 file carries `systemMessage`
and `firstKeptEntryId` but no `retainedTail`. C++ therefore takes the no-tail branch
(`SessionTree.cpp:519-568`): it replays **every** pre-compaction system message, then the
`compactionSummary`, then the messages from `firstKeptEntryId`. Pi builds exactly
`[systemMessage, compactionSummary]` and, in the kept range, skips system messages
(`session-manager.ts:499-511`). On a resumed pi session with multiple prompt declarations, C++ can
put stale declarations before the summary; on a C++ session read by pi, `systemMessage` is absent and
pi loses the prompt/tool boundary entirely. Adding the field without changing the replay rule would
still not match the captured observation.

The C++ leaf marker (`SessionEntryValue::LeafEntryValue`, `SessionStore::append_leaf`) comes from
the old pi-agent-core wire (`LeafEntry` at commit `83114817`), not v1.3.0 coding-agent. It is a real
interop divergence: pi v1.3.0 treats the marker as the last entry and therefore as the active leaf
when reopening a C++ file. The marker is needed by C++'s durable branch-position behavior, but it
cannot be silently presented as the v1.3.0 wire contract.

## History, tree and context reconstruction

Pi exposes `getBranch()` in root-to-leaf order (`session-manager.ts:1469-1483`), `getEntries()` without the
header (`:1520-1526`), `getTree()` with labels and children (`:1529-1577`), `branch()`/`resetLeaf()`
(`:1579-1598`) and `branchWithSummary()` (`:1600-1627`). `createBranchedSession` filters labels, re-chains
parents, rewrites `firstKeptEntryId` when it points at a removed label, and writes a new header with
`parentSession` (`:1632-1748`).

C++ exposes the equivalent through `SessionStore::tree/tree_snapshot/build_context/branch/reset_leaf/
get_entry/get_branch/entries/get_label/get_session_name` (`SessionStore.hpp:119-160`). The live tree is
updated on every accepted append; no query re-reads the file. `create_from_entries` re-chains a fork and
preserves unknown lines (`SessionStoreTest.cpp` cases `create_from_entries writes a re-chained branch file
the loader reads back` and `create_from_entries preserves a foreign entry line and refuses a nested header`).
`SessionFork` reproduces pi's message-position selection, label re-creation and parent-session header
(`src/coding_agent/runtime/SessionFork.cpp:228-278,437`; `SessionForkTest.cpp`).

Two API differences matter for a later move:

- C++ `get_branch` returns leaf-to-root (its header documents this), while pi `getBranch` returns
  root-to-leaf. Callers in C++ reverse when building context; a consumer-preserving migration must not
  accidentally expose the internal order as the public order.
- C++ `SessionContext` carries only messages, provider/model and thinking level
  (`SessionTree.hpp:40-57`). Pi `SessionProjection` also carries the projected source entries and their
  per-entry message contributions (`session-manager.ts:216-227,525-566`). That provenance is what makes
  `context_edit`, source-entry lookup and estimated-token accounting work; C++ has no equivalent.

The captured tree scenario confirms the pi side: `context_edit` changes only the model projection, a
`branch_summary` records the abandoned `fromId` and becomes the new active leaf, and `createBranchedSession`
writes `model_change, thinking_level_change, message, message, label` with a `parentSession` header while
leaving the original file intact. The C++ gaps are exactly the entry kinds and projection provenance listed
above; the tree/fork operations themselves are implemented.

## Resume and tool restoration

Pi's resume path is captured in `session-tool-restore.json`:

- `AgentSession` constructor restores the transcript loadout when `usesDefaultTools` is true
  (`agent-session.ts:526` → `_restoreToolsFromTranscript`, `:1815-1825`).
- `_restoreToolsFromTranscript` takes the last system message's `toolsAdded`, applies `+name`/`-name` in
  modifier order, and filters through `_isAllowedTool`. It clears `_pendingToolNames` and calls
  `_setActiveTools` (`:1815-1825`). The effective order is the transcript declaration order with added
  names appended.
- `getToolListError` rejects mixed plain names and modifiers or wildcard modifiers
  (`settings-manager.ts:97-104`); `applyToolModifiers` preserves existing positions and appends new names
  (`:110-120`).
- `setActiveToolsByName` only changes the live tool list; the next agent turn emits a system declaration
  delta. The capture shows the first declaration `toolsAdded:["read","bash"]` and the second
  `toolsAdded:["write"]`, not a full replacement.

C++ resumes correctly enough for the existing test, but it does **not** implement the captured modifier or
order contract:

- `AgentSessionCreationRequest::tools` is documented as an allowlist (`AgentSessionCreationRequest.hpp:78-83`);
  `ToolSelection` has only `allowed` and `excluded` (`ToolSelection.hpp:14-23`). There is no `+name`/`-name`
  parser, validation, or modifier precedence.
- `replay_active_tool_names` accumulates declarations and then returns them in `available_names` order
  (`AgentSessionExecution.cpp:55-85`). A recorded `["bash","read"]` loadout becomes the registry's
  `["read","bash"]` order; only `AgentSessionSnapshotTest.cpp` case `resume replays the recorded tool loadout
  and ignores unavailable imported tools` covers a registry-order result.
- C++ keeps `initially_inactive_tool_names` so default-inactive registrations survive resume
  (`AgentSessionExecution.cpp:234-242`), which covers the codemode activation case, but not pi's
  transcript-order or modifier behavior.

**Rows**: `resume:tool-loadout` is `partial`, `resume:relative-tool-modifiers` is `missing`, both
`upstream-captured` only. No C++ test compares against the capture values.

## Compaction, automatic recovery and summary serialization

### Captured summary boundary

`session-compaction-boundary.json` records the exact behavior:

- The transcript keeps `SECRET_THINKING_FIRST` and `SECRET_THINKING_SECOND`.
- `serializeConversation` (`packages/coding-agent/src/core/compaction/utils.ts:120-175`) omits thinking; both
  summarization request prompts contain only visible text/tool-call material (`summarizationRequestsContainThinking:false`).
  This is independently specified by the upstream test `should omit thinking`
  (`packages/coding-agent/test/compaction-serialization.test.ts`).
- The 8 pre-compaction JSONL lines are a byte-identical prefix of the 9 post-compaction lines; exactly one
  `compaction` entry is appended.
- The appended entry has `summary` (history plus split-turn context), `firstKeptEntryId` pointing at the
  second assistant, `tokensBefore`, `details:{readFiles:[],modifiedFiles:[]}`, usage, `fromHook:false`, and a
  `systemMessage` whose `toolsAdded` are `["read","bash"]` and whose timestamp equals the entry timestamp.
- Rebuilt projection roles are `["system","compactionSummary","assistant"]`; the retained assistant still
  carries its thinking block. The summary is a separate message, not a rewrite of the assistant message.

Pi's implementation points: `prepareCompaction` (`compaction.ts:872`), `compact` (`:965`),
`generateSummaryWithUsage` (`:696`), and `SessionManager.appendCompaction`
(`session-manager.ts:1261-1290`). `sessionEntryToContextMessages` inserts `systemMessage` before the
`compactionSummary` and then the retained entries (`:457-465`).

### C++ comparison

C++ implementation is `harness::session::compact` and `AgentSession::Impl::compact/commit_compaction`
(`src/coding_agent/AgentSessionCompaction.cpp:17-196`). It preserves cache retention, fresh summarization
ids, thinking-level routing, split-turn handling, file-operation details and usage. The C++ compaction
tests cover preparation, split turns, cache-retention options, summary text, retry errors and persistence
(`tests/harness/compaction/CompactionTest.cpp`).

One of those tests is a provenance trap: `compaction persistence and rebuild goldens match pi's
CompactionEntry wire fields` (`CompactionTest.cpp:908`) compares against
`fixtures/pi-agent-core/compaction-persistence.jsonl`, which has `retainedTail` and no
`systemMessage`. The test name and `compat-pi` tag therefore assert v1.3.0 parity for the older
pi-agent-core shape. It must be re-based or renamed when the wire decision lands; it cannot be cited
as current parity evidence.

The differences that survive this audit are:

1. **Boundary field**: pi writes `systemMessage`; C++ writes `retainedTail`. C++ `CompactionDto` has no
   `systemMessage` (`EntrySerializer.cpp:120-132`). Pi-written sessions therefore lose the boundary in
   C++ projection; C++-written sessions are not understood by pi's projection. This is not just a field
   name difference: pi's field is a `SystemMessage` with prompt sections and tool declarations; C++'s
   tail is a list of messages and includes all projected system messages plus retained chat.
2. **Compaction in memory**: pi `SessionManager.inMemory` accepts `appendCompaction` and
   `buildSessionProjection`; upstream test `should work with --no-session mode (in-memory only)`
   (`packages/coding-agent/test/agent-session-compaction.test.ts`) covers manual compaction. C++
   `run_auto_compaction` returns false when `store->path()` is absent (`AgentSessionCompaction.cpp:215-221`)
   and manual compaction rejects an in-memory session (`AgentSessionCompactionTest.cpp` case `manual
   compaction rejects sessions too small to compact and in-memory sessions`). This is a recorded
   divergence, not an intentional pi exclusion.
3. **Overflow recovery omission**: pi calls `_omitRecoveryAttempt` before retrying, which appends
   `context_edit(targetId, null)` for the failed assistant and tool results
   (`agent-session.ts:1241-1258,3073`). C++ only pops the trailing assistant from live Agent state
   (`AgentSessionCompaction.cpp:327-338`) and persists no omission. On reopen, the failed assistant
   reappears in the model context. This is exactly why `context_edit` must land before the compaction
   recovery slice.
4. **Retry behavior**: the capture sets `retry.enabled:false` deliberately. Summarization retry and the
   additional AI retry matching are not part of the summary-serialization observation; they are separate
   capability rows and separate AI exclusions. Pi's overflow retry is covered by
   `agent-session-auto-compaction-queue.test.ts` and `agent-session-compaction.test.ts`; C++ covers
   compact-and-retry in `AgentSessionCompactionTest.cpp` but without persistent omission.

The compaction summary serialization itself is aligned: C++ `serialize_conversation` is documented against
pi and tested by `the compaction door serializes pi's verbatim conversation text`
(`CompactionTest.cpp`), and the capture confirms the frozen pi behavior. The row is `upstream-captured`,
not `verified`.

## Abort, close and failure/cancel comparison

Pi `abort()` sets the run abort request, aborts retry, compaction and branch summarization, aborts the
Agent and waits for idle (`agent-session.ts:2464-2475`). `dispose()` aborts retry/compaction/branch summary/
bash, invalidates extension contexts, disconnects listeners, clears the cache warmer and cleans session
resources (`:1397-1419`). It does not await a durable persistence channel because pi writes synchronously.

C++ `AgentSession::abort()` only requests the active stop source (`AgentSession.cpp:537-540`;
`AgentSessionExecution.cpp:797-801`). `close()` transitions to `Closing`, rejects new admission, requests
work-scoped cancellation, waits for admitted compaction and drains the Session Event Commitment channel,
then releases the store and services (`AgentSessionExecution.cpp:803-906`). Idempotence, late callbacks,
slow persistence, retry backoff and a latched persistence failure are all covered by
`SessionCloseTest.cpp` and `SessionCommitmentAsyncTest.cpp`.

The difference is intentional and should be stated, not erased: pi disposal aborts an in-progress
compaction; C++ Close waits for an admitted compaction to reach its terminal outcome before releasing the
store (ADR 0040). A future ownership move must keep the off-loop commitment and Close ordering or explicitly
change the contract with the Owner.

### Failure, cancel and reopen cases

| Case | Frozen pi source/test | C++ source/test | Current result |
| --- | --- | --- | --- |
| Reopen after compaction | `agent-session-compaction.test.ts:should persist compaction to session file`; captured boundary | `SessionLifecycleTest.cpp:resumed session uses compaction tree context`; `SessionTree.cpp:480-568` | **Diverges**: pi projects `systemMessage + summary` and skips kept system messages; C++ has no `systemMessage` and replays every pre-compaction system message before the summary |
| Corrupt or oversized session file | `session-manager/file-operations.test.ts:skips oversized corrupt files and returns a valid session`; `...preserves explicit session file path when recovering from corrupted file` | `JsonlSessionStoreTest.cpp:Glaze JSONL session reports malformed line context`; `SessionResume.cpp:98-102` | **Partial**: both refuse/skip the bad file, but C++ has no session-list recovery path and no corruption test at the discovery layer |
| Abort a manual compaction | `agent-session-compaction.test.ts:cancels in-progress manual compaction when abortCompaction is called`; `...aborts an in-progress manual compaction and waits until the session is idle` | No cancel-compaction API; `AgentSession.cpp:537-540` only stops the prompt; `SessionCloseTest.cpp` waits | **Missing**: C++ `abort()` cannot cancel a compaction; only Close waits for it |
| Close during retry backoff | pi `dispose()` aborts retry and waits for idle (`agent-session.ts:1397-1419`) | `SessionCloseTest.cpp:Session Close during a retry backoff cancels the wait and settles the run` | **Implemented but divergent**: C++ uses its Close quiescence contract; pi disposes synchronously |
| Persistence failure | pi writes synchronously; no queued failure state | `SessionCommitmentAsyncTest.cpp:a persistence failure keeps live state and rejects later prompts with a typed failure`; `SessionCloseTest.cpp:...latched persistence failure...` | **C++-specific**: sticky typed failure and Close drain are stronger than pi's synchronous write path |
| Overflow recovery, repeated | `agent-session-compaction.test.ts:does not retry overflow recovery more than once`; `...notifies extensions when auto-compaction fails` | `AgentSessionCompactionTest.cpp:overflow terminal compacts and retries the turn exactly once`; `...a second overflow...verbatim recovery message` | **Partial**: retry count/error text align, but the failed attempt is not persisted as a `context_edit` omission, so reopen resurrects it |
| Reopen with tool/summary usage | `session-manager/tree-traversal.test.ts:preserves tool and summary usage across a file-backed reload` | `SessionLifecycleTest.cpp` resume cases; `AgentSessionSnapshotTest.cpp` loadout case | **Partial**: usage survives where the C++ DTO has it, but tool order and relative modifiers diverge (see above) |

These are the cases A14 should use to distinguish a green consumer move from a behavior change. The
capture covers only the first row's frozen side; the other rows are existing test-source comparisons and
remain `unverified`.

## SessionStore consumer map

`SessionStore` is the closed concrete facade over JSONL/in-memory, with a mutex around appends and live-tree
queries (`SessionStore.hpp:10-44`). Current consumers:

| Consumer | Entry point / methods | Coupling and behavior |
| --- | --- | --- |
| Session Assembly | `SessionLifecycle.cpp:163,223,363`; `create_new`, `in_memory`, `open_existing` | Chooses the backing and owns the store from publication to Close |
| Resume | `SessionLifecycle.cpp:301-366`; `resume_session`, `open_existing` | Reads the file twice (see gap) and rebuilds context/model/thinking/topology |
| New-session publication | `SessionFactory.cpp:1886-1953`; `append_model_change`, `append_thinking_level_change`, `append_session_info`, `path` | Initial entries are appended on the runtime worker before the session is published |
| Session Event Commitment | `SessionEventCommitment.cpp:26-53`; `SessionPersistence.cpp:117-200` | Completed messages are admitted off-loop and appended in FIFO order; in-memory sessions append directly |
| Initial system message | `AgentSessionExecution.cpp:316-355`; `append` | Persists the structured system message and loadout declaration after the first run starts |
| Interactive messages / model / thinking | `AgentSessionInteraction.cpp:282-337,598,692`; `append`, typed appends | Mid-run direct appends; must drain the commitment before ordering-sensitive typed appends |
| Tree navigation and labels | `AgentSessionInteraction.cpp:904-1024`; `tree_snapshot`, `entries`, `branch`, `reset_leaf`, `append_leaf`, `append_label_change` | Exposes live topology and durable leaf position |
| Session name | `AgentSessionInteraction.cpp:1428-1441`; `append_session_info` | Appends a name entry to the live store |
| Fork | `SessionFork.cpp:228-278,437`; `SessionLifecycle.cpp:381-490`; `load`, `get_entry`, `effective_parent_id`, `get_branch`, `entries`, `create_from_entries` | Foreign-file parse, re-chain, label re-create, in-memory seed, and the persisted fork target written by `prepare_fork_target` |
| Compaction | `AgentSessionCompaction.cpp:112-196`; `get_branch`, `append_compaction`, `build_context` | Drains persistence, appends the checkpoint and rebuilds the Agent context |
| Transcript export | `TranscriptExport.cpp:10-30`; `entries` | Serializes every entry through the private serializer |
| TUI `/export` | `InteractiveEngineHost.cpp:357-360`; `open_existing`, `entries` | Re-opens the file instead of using the live session |
| TUI `/name` | `InteractiveEngineHost.cpp:117-141`; `set_session_name` | Uses the live AgentSession append path (correct) |
| TUI session-selector rename | `SessionFlowController.cpp:563-572`; `open_existing`, `append_session_info` | Re-opens the selected file as a **second writer**; a live store for the same session does not see this append |
| SQLite alternative | `SessionStore.cpp:76-116,158-182`; `create_sqlite`, `open_sqlite`, `commit_batch` | Only tests construct it; no production CLI path. It pulls `SqliteSessionStore`/`SqliteDatabase`/`TaskStore` into `cch_agent_core` |
| Tests | `SessionStoreTest`, `JsonlSessionStoreTest`, `SessionStoreConformanceTest` | Cover wire, redaction, leaf markers, live tree, fork lines and compaction tails |

The TUI findings are narrower than they first appear. Pi `/name` uses the live
`AgentSession.setSessionName` path (`agent-session.ts:3972-3974`, interactive-mode `:6700`), and C++
`/name` now does the same (`InteractiveEngineHost.cpp:128`). The remaining bypass is the
session-selector rename callback (`SessionFlowController.cpp:563-572`), which opens the selected file and
appends `session_info` directly, and `/export`, which re-opens the file
(`InteractiveEngineHost.cpp:357-360`). Pi transcript export uses the live `SessionManager`
(`session-export.ts`). C++ `AgentSession` already exposes `entries`; moving ownership while leaving the
two-second-writer paths in place would create two live trees for a session that is currently open.

## JSONL / legacy SQLite coupling and the durable boundary

The application transcript is the JSONL session file under the Pike XDG state root. `agent_config_dir`
returns `$XDG_CONFIG_HOME/pike/agent` or `~/.config/pike/agent` (`src/coding_agent/AgentConfigDir.cpp:25-35`);
`sessions_root_path()` is its `sessions` child (`:33-35`). Pi uses `~/.pi/agent/sessions`; that path is not
changed by this audit. The retained product boundary is ADR 0075/ADR 0058, not pi's home layout.

`SqliteSessionStore` is described as “mirroring pi-durable's relational backing”
(`src/agent/harness/session/SqliteSessionStore.hpp:17-20`) and lives in `cch_agent_core` with
`SqliteDatabase`, `TaskStore`, `JsonlSessionStore`, `EntrySerializer` and the application compaction code
(`cmake/targets/AgentCore.cmake:31-48`). `cch_agent_core` links `unofficial::sqlite3`. The `SessionStore`
header calls itself a closed facade over “the two supported persistence alternatives — a JSONL session
file and an in-memory no-persistence store,” yet the same public class exposes `create_sqlite` and
`open_sqlite` (`SessionStore.hpp:19-24,54-59`). Production `SessionStore` uses JSONL or in-memory; SQLite is
currently reachable only through test factories. The interface and the link edge therefore carry the legacy
coupling even though no CLI path selects it. This is
the legacy durable coupling the ticket asks to identify: moving application Session to `cch_coding_agent`
must not take the durable SQLite/task surface with it, and durable must not gain a dependency on the
coding-agent package to reuse it.

The independent durable runtime is not switched by this audit. ADR 0075 requires new durable JSONL/SQLite
stores to open and continue with frozen pi-durable v1.3.0, and explicitly forbids old-Pike durable import,
automatic upgrade, dual-format reads or fallback compatibility. Ordinary coding-agent session files remain
a separate contract. The audit does not delete, migrate or reinterpret user session data.

The C++ header and redaction choices are retained behaviors that a wire-parity change must treat
explicitly: C++ writes `provider`/`model` on the session header and redacts secret-like text at the
persistence seam. Pi v1.3.0's header has no provider/model fields and `_persist` does not redact. This audit
records the divergence; it does not authorize removing the security redaction or inventing a migration.

## Capability ledger summary

The machine-readable ledger is the source for A14 consolidation. Counts in this revision:

| Membership | Rows |
| --- | --- |
| supported | 26 |
| excluded | 4 |
| no-decision | 0 |

| Implementation | Rows |
| --- | --- |
| implemented | 11 |
| partial | 9 |
| missing | 10 |

| Verification | Rows |
| --- | --- |
| upstream-captured | 11 |
| unverified | 15 |
| verified | 0 |
| not-applicable | 4 |

The zero `verified` count is deliberate: the lane's own evidence is not C++ verification, and no C++
comparison test was added in an audit-only ticket. `upstream-captured` applies to the tool-restore,
compaction-boundary and tree/fork scenarios; every other supported row is `unverified` with C++ test
sources listed as existing regression coverage, not parity proof.

The two rows that were `no-decision` are now `excluded` by the owner's #1059 ruling: the leaf marker
is a non-pi extension (pi v1.3.0 keeps the active leaf in runtime state only) and the agent's SQLite
session store had no production caller and no pi v1.3.0 equivalent — its code was deleted in commit
`d0844505d`. Both keep an explicit boundary instead of a membership question.

## Remaining gaps (ordered by migration risk)

1. **Wire baseline skew**. The C++ application Session is built on pi-agent-core 0.83
   (`retainedTail`, `leaf`, no `usage`/`context_edit`), while pi-v1.3.0 coding-agent owns the
   SessionManager wire (`systemMessage`, `usage`, `context_edit`, no `leaf`/`active_tools_change`).
   Decide and re-base the application wire before touching individual fields or fixtures.
2. **Compaction boundary field** (`systemMessage` vs `retainedTail`). Blocks pi/C++ session interchange and
   resume-after-compaction prompt/tool fidelity. Fix before any Session ownership move.
3. **Missing `context_edit` and `usage` entry types**. Blocks pi parity for recovery omission, context edits
   and usage accounting; the parser's `Unknown` passthrough preserves bytes but loses projection semantics.
4. **Missing v1→v2→v3 migrations**. A pi v1/v2 file can be read only by field-name accident; no id/parentId
   repair and no `hookMessage` conversion.
5. **Relative `+name`/`-name` tool selection and transcript-order restore**. Explicitly listed in ADR 0075
   application additions; absent from `ToolSelection` and `replay_active_tool_names`.
6. **Overflow recovery omission is not persisted**. Pi writes `context_edit`; C++ pops only live state, so a
   reopen resurrects the failed attempt.
7. **Resume parses twice**. `resume_session` + `open_existing` should become one `open_loaded` handoff.
8. **Compaction is unsupported for in-memory sessions** in C++, while pi's `--no-session` mode compacts.
9. **C++ leaf markers are non-v1.3.0 wire**. Needed for durable branch position, but not interoperable as
   the current coding-agent format.
10. **TUI session-selector rename and `/export` bypass the live session**. `SessionFlowController` opens a
    second store and appends `session_info`; `InteractiveEngineHost` re-opens for export. `/name` already
    uses the live path. Route the remaining two through live APIs before moving.
11. **SQLite/task coupling in `cch_agent_core`**. The application facade carries legacy durable storage and
    an sqlite3 link; split before ownership moves.
12. **Context projection provenance is absent**. Pi `SessionProjection.entries` carries source entries;
    C++ `SessionContext` cannot express `context_edit`/usage/source mapping.
13. **Header and redaction divergences** remain explicit retained choices; they need a wire/compatibility
    decision, not a silent cleanup.
14. **Abort/Close semantics differ**. Pi disposes by aborting compaction; C++ Close waits for admitted
    compaction and drains commitment. Keep ADR 0040 ordering unless the Owner changes it.

## Bounded migration recommendation

The blast radius is the consumer sites above and their tests; the session wire is also consumed by fork,
export, tree navigation and compaction. A direct package move would move SQLite/task dependencies and the
TUI double-open with it. Recommended expand–migrate–contract sequence:

**Expand (green, no behavior change)**

1. Add a `cch_coding_agent`-owned application Session seam that forwards the existing `SessionStore`
   operations and exposes the live `AgentSession` names/entries/labels/export operations. Keep
   `cch_agent_core` as the implementation.
2. Switch the session-selector rename and `/export` to the live AgentSession seam when the target is the
   open session. Remove their `SessionStore::open_existing` calls; keep `/export`'s file writing in the
   existing transcript export path.
3. Add `SessionStore::open_loaded` (or equivalent) and make `publish_resume_session` use the single parse
   already performed by `resume_session`.
4. Land behavior slices behind their own tests: `usage`/`context_edit` entry typing and projection;
   compaction `systemMessage`; tool modifiers/order; v1/v2 migration; in-memory compaction; persisted
   recovery omission. Each slice gets a C++ test that names the capture scenario where one exists.

**Migrate**

5. Move JSONL session, wire serializer, tree and compaction application code to the application owner;
   keep in-memory as the second application alternative.
6. Split `SqliteSessionStore`/`SqliteDatabase`/`TaskStore` out of the application seam into the durable
   owner (or a neutral durable package). Do not let `cch_durable` depend on `cch_coding_agent`; do not make
   application Session depend on durable.
7. Keep `EntrySerializer` and `SessionTree` with the application owner while durable has its own schema.
   If a shared wire type is unavoidable, place only the neutral value types in `cch_support`; do not share
   the durable database/journal implementation.

**Contract**

8. Remove the old `SessionStore` facade from `cch_agent_core`, remove sqlite3 from its link line, and update
   the architecture manifest/validator. Keep the JSONL format and the XDG root unchanged.
9. Re-run the frozen capture scenarios and add the C++ comparison tests; only then may the ledger advance
   from `upstream-captured` to `verified`.

This order keeps existing consumers green at each step, avoids a durable-to-coding-agent dependency, and
separates behavior parity from ownership relocation. It intentionally does not switch `pike --resume` or
the application Session onto durable storage, and it does not require an old-Pike durable migration.

## Validation performed

| Command | Result |
| --- | --- |
| `PI_CHECKOUT=/tmp/cch-1048-pi-frozen PI_BASELINE=pi-v1.3.0 /tmp/cch-1048-pi-frozen/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1048/capture-session-persistence.mts` | 3 artifacts captured at the registered revision |
| `python3 scripts/evidence/audit_evidence.py check --manifest fixtures/pi-v1.3.0/audit-1048/manifest.json` | passed; printed registry record with manifest SHA-256 `2cbc07e8d2924ff92775245cd2750545da083fb07fbb1f88abf604134669eecd` |
| `python3 -m json.tool fixtures/pi-v1.3.0/audit-1048/manifest.json` and each artifact | passed (JSON shape and duplicate-key check by parser) |
| `git diff --check` | passed |
| C++ test suite | not run; this ticket changes no C++ source and the relevant tests are cited as existing regression coverage only |

The bundle is `upstream-captured`, not registered or `verified`. The serial merger must apply the JSON
patch request in the ticket handoff and run `verify` after registration.

## Open decisions and unsupported observations

- No frozen capture of abort/close timing, persistence failure, retry/backoff or fork-at-message error
  strings is added here. Those rows cite existing pi/C++ test sources and remain `unverified`.
- The cross-model thinking transformation and additional AI retry matching are excluded from this audit by
  ADR 0075; they are not used to explain away application summary behavior. The compaction capture keeps
  them separate by disabling retry and recording only the summary request/storage boundary.
- The empty `azure.json` preparation is an environment workaround, not a model-catalog observation. The
  frozen checkout's HEAD and tracked tree were verified clean before capture.
- No ownership, package, durable-storage or CLI migration is claimed by this ticket.
