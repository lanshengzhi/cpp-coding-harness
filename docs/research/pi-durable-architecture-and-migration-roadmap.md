# pi-durable Architecture and Pike Migration Roadmap

**Research baseline:** upstream `@earendil-works/pi-durable` v1.0.4 at the Pike-recorded pi SHA `7c10bd4337495ee613f2224843ecdf349b80d1df`; Pike current worktree at research time. This is an engineering investigation, not an owner membership ruling. ADR 0053/0066 governs whether any capability enters Pike's supported subset.

## Executive summary

`pi-durable` is a transactional runtime for conversations, append-only entries, durable documents, submissions, and resumable tasks. Its 15,483 LOC package combines three layers: a generic `Storage` contract, a serialized `Session` transaction kernel, and a `Harness` scheduler that executes versioned task definitions. SQLite and JSONL are two implementations of the same broad storage contract, not interchangeable transcript formats.

Pike already owns a strong but narrower session-history subsystem: private-permission JSONL journal, strict parsing/serialization, a live `SessionTree`, branch/leaf markers, compaction, and an awaited Agent-to-Session commitment path. Pike does not currently expose durable tasks, task ownership graphs, queued submissions/inbox, scheduler recovery, or multi-record transaction rollback. Its JSONL append behavior and pi's multi-record atomic commits therefore make different promises.

Recommended sequence, conditional on a future explicit scope ruling:

1. Record a capability-specific decision in ADR 0066 and an issue: whether Pike needs a SQLite-backed **Agent Session history** store, or the much broader pi-durable object model.
2. If SQLite is wanted, build a narrow Pike-owned session store prototype behind the existing concrete `SessionStore` facade. Preserve JSONL compatibility/import and never silently reinterpret a file. Measure operational, durability, and migration costs before selecting it.
3. Add transactional event journaling only if a concrete user-visible multi-record operation requires atomicity. Specify the durability boundary and failure recovery explicitly; do not infer rollback from appending event records.
4. Add durable task/submission scheduling as a separate capability slice. It requires the task state model, registry/migration contract, ownership/cancellation rules, and scheduler semantics as one coherent vertical tranche; a graph viewer alone is not a scheduler.

## Architecture map

```text
pi-durable
┌───────────────────────────────────────────────────────────────┐
│ Harness API: conversations, submissions, tasks, views, hooks  │
│  ├─ TaskScheduler ─ registry snapshots ─ version migration    │
│  ├─ Submissions + Inbox ─ generation / tool / compaction tasks │
│  ├─ TaskGraphView + conversation views                         │
│  └─ Extension definitions, tools, sections, wrappers, hooks   │
├───────────────────────────────────────────────────────────────┤
│ Session + Transaction: serialized commit callback, staged     │
│ table/document writes, validation, atomic storage commit,      │
│ cache adoption/publication only after successful commit        │
├───────────────────────────────────────────────────────────────┤
│ Storage contract: conversations / entries / tasks /           │
│ submissions / revisioned documents                             │
│   ├─ SQLite: normalized indexes + JSON records + SQL txn       │
│   ├─ JSONL: main commit markers + task/document sidecars        │
│   └─ Memory: conformance/reference implementation               │
└───────────────────────────────────────────────────────────────┘
```

```text
Pike today
Agent ── awaited lifecycle commitment ──> AgentSession serialized domain
                                              │
                                              ├─ SessionStore facade + mutex
                                              │    ├─ JsonlSessionStore
                                              │    │    └─ SessionJournal (append + fsync, no-follow, 0600)
                                              │    └─ in-memory SessionTree
                                              ├─ SessionTree (branch/leaf/context projection)
                                              └─ compaction (summarize off-domain, append result)

RuntimeRoot: Boost.Asio event loop + bounded worker/mailbox lanes
```

The key distinction is transaction shape. pi-durable's `Session.commitWith(callback)` stages a batch, validates it, asks Storage to atomically commit the batch, then adopts cached document changes and publishes committed state. Pike's `SessionStore` serializes individual entry appends and mirrors accepted entries into a live tree. The Session Event Commitment contract deliberately says that persistence failure does not roll back newer live state and that Pike does not add a WAL or rollback transaction (ADR 0040).

## Upstream subsystem findings

### Storage

| Concern | SQLite (`src/storage/sqlite/`) | JSONL (`src/storage/jsonl/`) | Relevance to Pike |
|---|---|---|---|
| Data model | `conversations`, `entries`, `tasks`, `submissions`, `documents`, `document_revisions`, global ID table and metadata. JSON records retain flexible payloads; indexed columns serve filters and ordering. | `main.jsonl` stores ordered commit markers and small records; `task-ID.jsonl` / `doc-ID.jsonl` sidecars store mutable/live task and document content. A MemoryStorage replay builds query state. | Neither maps directly to Pike's one-file conversation transcript. SQLite schema covers the durable task runtime, not merely session entries. |
| Atomicity | `SqliteStorage.commit` wraps all writes in one database transaction. IDs, entry sequence, task/submission state, document revisions and retirements advance together. Migrations are contiguous and transactional. | Sidecars append first; optional fsync of each; main commit marker publishes them. Recovery confirms referenced sidecar records, truncates uncommitted tails, rejects malformed complete lines, tolerates torn final lines, and retries reclamation. | Pike currently fsyncs each transcript append. pi JSONL's default `fsync=false` is weaker unless configured. A SQLite backend must explicitly choose FULL/NORMAL synchronous semantics and define what a successful `Expected` means. |
| Reads/indexes | SQL indexes support owner/status/kind/background/request/address lookups, pagination, history points, and document history. Prepared statement caching and an operation queue serialize one connection. | Replay through MemoryStorage offers the same abstract query contract; this trades simple inspectable files for startup/replay work and sidecar complexity. | SQLite adds substantial concepts Pike does not need for transcript browsing unless task/query requirements justify them. |
| Documents | Chord-backed documents use base + delta revisions, history points, scopes (session/conversation/task), fork copy semantics, retirement, optional checkpointing. | Main marker references document sidecar records by sequence/ordinal; recovery reconstructs commits and sidecar reclamation compacts current-only documents. | Closest reusable concept is not Pike's JSONL entry. Pike's session tree is an append-only history; it has no general mutable document store today. |
| Adapter seam | `SqliteDatabase` / `SqliteExecutor` abstracts SQL operations and transactions; Node adapter uses built-in `node:sqlite`, WAL, `synchronous=NORMAL`, busy timeout 5s, and `BEGIN IMMEDIATE`. | Abstract filesystem adapter enables portable storage and deterministic fault injection. | C++ should own a narrow SQLite connection/transaction adapter; no ORM is required by the inspected schema. |

`SqliteStorage`'s atomic commit is meaningful because all write records participate in one storage protocol. It is not a generic WAL layer that can be placed under current JSONL and automatically make a sequence of existing append APIs rollback-safe.

### Session transaction and crash recovery

`src/session/transaction.ts` owns transaction-local staged writes, document drafts, task candidates, submissions, and pending asynchronous operations. It seals the transaction when the callback settles. On callback failure it aborts draft changes and drains outstanding operations. On callback success it prepares changes, validates ownership/version/fork constraints, builds a complete write batch, and submits it once. Storage failure discards prepared changes. Successful storage commit is followed by in-memory adoption/publication. Table reads are rejected after the first table write (`ReadAfterWrite`), making callback order part of the contract.

Crash recovery is split across persistence and scheduler startup:

- SQLite transaction rollback gives all-or-nothing durable writes if a process/database failure interrupts a transaction, subject to SQLite and selected sync mode guarantees.
- JSONL commit markers distinguish complete commits from stray sidecar tails. Complete malformed records are corruption; incomplete final lines are truncated. Missing committed sidecars are corruption unless later valid reclamation proves their contents obsolete.
- `TaskScheduler.open()` reloads live statuses and converts surviving `running` tasks to `pending`, then reconciles ownership cancellation, fail-fast waits, terminal outcomes, and queued inputs. It dispatches no code until `resume()`.

This is not automatic exactly-once execution. A task phase can perform an external side effect and crash before committing its checkpoint. Definitions must use durable checkpoints/idempotency where effects can repeat. Scheduler persistence gives recoverable intent and state, not transactional rollback of arbitrary tools, shell commands, or provider calls.

### Harness, scheduling, submissions, graph

`TaskScheduler` serializes every task transition on the Session line. Reservation is committed before handler execution. It uses immutable registry snapshots per scheduling pass/phase, task-definition version migrations, pending/running/waiting/completing/terminal states, durable abort marks, cancellation propagation through ownership, fail-fast joins, background task boundaries, idle waits, and orphan/fault terminal outcomes. Handler code runs off the line; its runtime methods re-enter through serialized commits. On open, running work is reset to pending; pending work is dispatched only after resume.

`Submissions` persists queued/placed/done/unanswered requests with request-ID deduplication and waiter handles. `InboxDoc` keeps steer/follow-up/write items; admission and boundary placement occur in commits with session entries and submission state. This is a durable admission model, not just a FIFO in-memory queue. `TaskGraphView` mounts a read-only live projection from durable task records and advances it from committed publications; it is intentionally a view, not the scheduler itself.

### Extensions, hooks and lifecycle

`define.ts` is mostly typed declaration helpers (`defineExtension`, `defineTool`, `section`, `hook`, `wrapTool`, `wrapSection`). `registry.ts` publishes immutable snapshots; built-in tasks cannot be removed/replaced, extension task names are collision-checked, and install/uninstall synchronously notifies listeners. A task record can continue under its old definition if a replacement cannot take it over; successful version migration occurs before reservation.

Extension hooks are selected by the current conversation Agent's extension set and task name, then run serially in extension order. Generation hooks cover before request, after response, yield and after tools; tool hooks cover before/after tool; compaction has beforeCompact. Hook errors are reported and generally isolated so later hooks/task execution proceed; invocation cancellation still propagates. Sections and tools can be wrapped by extension selection. This registry is more than a plugin loader: it participates in task version evolution and live scheduling.

## Pike comparison by file/seam

| Upstream pi-durable | Pike counterpart | Existing capability | Gap / migration observation |
|---|---|---|---|
| `storage/sqlite/database.ts`, `migrations.ts`, `storage.ts` | `session/SessionStore.*`, `JsonlSessionStore.*`, `SessionJournal.*` | Session transcript persistence, tree records and branch/fork history. | No SQLite store, schema migration framework, task/submission/document tables, or multi-record commit. SQLite is a new backend decision, not a drop-in optimization. |
| `storage/jsonl/storage.ts` | `SessionJournal.cpp`, `JsonlSessionStore.cpp` | Append-only session JSONL with owner-only permissions, symlink rejection, fsync, deferred first flush, and redaction/serialization seam. | Pike's format is pi session transcript format; upstream JSONL is a commit protocol for a generic database. Formats are not compatible. |
| `session/transaction.ts` | `SessionStore.cpp` mutex; `SessionEventCommitment` / `SessionPersistence` | Append serialization, ordering, awaited strong commitment, live tree mirror, failure latching/close obligations. | No callback transaction, atomic batch rollback, document drafts, read-after-write enforcement, or atomic state+history publication. Existing contract explicitly permits live state ahead of persistence after failure. |
| `harness/scheduler.ts` | `AgentSessionExecution.cpp`, `RuntimeRoot`, Agent lifecycle | One current Agent run with cancellation, worker pool, bounded queues, quiescent close, session commitment. | No persistent task definitions/checkpoints, joins, background tasks, scheduler restart recovery, task ownership tree, or scheduler view. Do not call the existing Agent loop a durable scheduler. |
| `harness/submissions.ts`, `inbox.ts` | AgentSession prompt/run admission and TUI steering/follow-up | Current run input and steering semantics exist in memory and are coordinated with run lifecycle. | No durable submission IDs/status, request deduplication, queue persistence across process restart, or atomic boundary placement. |
| `harness/task-graph.ts` | No direct equivalent; `SessionTree` tracks transcript topology | Session branch topology can be queried. | Transcript tree and task ownership graph have different nodes, edges, lifecycle and consumers. |
| `harness/define.ts`, `registry.ts`, `agent.ts` | `ToolRegistry`, Agent Tool seam, Extension Tool Source foundation (per ADR 0066) | Tool registration/assembly and extension-provided tool foundation. | Task registry, immutable task snapshots, task migration and task lifecycle hooks remain undecided/new. Tool source parity does not imply durable extension registry parity. |
| `harness/compaction.ts` | `harness/compaction/Compaction.cpp`, `AgentSessionCompaction.cpp` | Compaction selection/summarization and awaited append through the existing session path. | Compaction is a capability Pike already has; adopting durable tasks is not required to preserve compaction behavior. Wrapping compaction as a durable task would change lifecycle/recovery semantics. |
| `harness/context.ts`, `view.ts`, `live.ts` | `SessionTree`, `AgentSessionSnapshot`, AgentSession event/snapshot APIs | Session context projection and live interaction state. | No Chord replicated-state/document publication protocol; avoid adopting Chord merely to expose a graph. |

## C++23, no-exceptions and Boost.Asio integration

Pike's strict `-fno-exceptions` rule is incompatible with directly transliterating TypeScript's throw/reject control flow. Expected validation failures must use `support::Expected<T>` / `AsyncResult<T,E>`, and asynchronous callbacks must return errors through typed outcomes. Storage APIs should own errors at their seam, preserving the existing single-consumption, move-only AsyncResult rules (ADR 0040).

The event loop is not the place for synchronous SQLite calls. SQLite's C API is blocking even when a wrapper returns an awaitable. A C++ adapter should execute blocking open/query/transaction/checkpoint operations on the bounded worker pool and post completion to the owning serialized domain. Admission must be bounded and persistence/control work must use the reserved lane. Do not expose Boost.Asio types from a new Owner Interface; keep executor details in implementation code as ADR 0040 requires.

A single SQLite connection with `BEGIN IMMEDIATE` is a plausible initial shape because Pike serializes session state already; however it may serialize unrelated sessions and can block workers on file locks. Per-session connection/database partitioning reduces contention but complicates cross-session task ownership and atomicity. WAL does not itself guarantee power-loss durability: upstream chooses `synchronous=NORMAL`; Pike's present append uses fsync. A product decision must name the promised failure class (process crash, OS crash, power loss), sync mode, busy timeout, checkpoint/close behavior, and how a failed commit affects live state.

No `sqlite_orm` dependency is indicated by upstream's design: its SQL is explicit. The pinned vcpkg manifest currently has no SQLite dependency. If selected, first probe the pinned baseline for `sqlite3` and its CMake target/headers, then add that manifest dependency; do not create an overlay port unless the pinned port is missing or unsuitable. `sqlite_orm` would add a mapping layer and compile/API surface without being required for this schema. Keep SQL/Glaze serialization private and keep SQLite implementation beneath the existing concrete `SessionStore` facade unless an independently useful Owner Interface is proven.

## Proposed roadmap and tranche sizing

These are conditional issue drafts, not authorized product decisions. Each tranche should be independently reviewable and should only start after ADR 0066's owner ruling defines the desired capability.

### Tranche 0 — Scope and contract decision (documentation/spec)

**Goal:** resolve the two separate ADR 0066 rows: SQLite session store and durable execution layer. Ask whether Pike wants SQLite for current transcript history, and separately whether it wants persistent tasks/submissions/scheduling. Specify pi baseline, supported user-visible workflows, format/import policy, failure model, and explicit exclusions.

**Acceptance:** ADR 0066 rows updated with attributed ruling; issue names semantic parity surface and non-goals; migration/import behavior and durability promise stated. No implementation dependency.

### Tranche 1 — SQLite session-history backend probe (bounded prototype, then issue)

**Goal:** determine whether a narrow SQLite representation of existing Pike session records can preserve `SessionStore` behavior and improve a measured user concern. This tranche does not implement pi-durable's general task/document model.

**Candidate design:** new private `SqliteSessionStore` behind the current concrete facade; store the Pike JSON record/wire payload and indexed entry/parent/leaf metadata; transactionally append the message and leaf marker where current Pike semantics require it. Retain JSONL read/import as a one-time migration path only if the ruling wants it. Do not dual-read indefinitely.

**Probe questions:** SQLite port availability on pinned vcpkg; staged install/runtime licensing; fsync-vs-WAL behavior; startup/resume, branch queries, append latency and database size; power-loss durability; database file permissions and symlink/no-follow safety; backup and corruption recovery.

**Acceptance:** recorded probe data and selected schema; storage contract fixture covers tree/fork/foreign-entry preservation and injected transaction failure; old JSONL import is atomic/idempotent or explicitly excluded; explicit durability policy. No `sqlite_orm` unless probe demonstrates material benefit.

### Tranche 2 — Transactional session event journal (only if required)

**Goal:** make a specific multi-record Pike operation atomic, if Tranche 0 establishes that requirement. Decide whether the SQLite transaction in Tranche 1 is sufficient or a separate event journal is truly needed. A generic append-only journal without consumer/replay semantics is not a deliverable.

**Acceptance:** an operation-level transaction boundary, replay/recovery algorithm, idempotency rules for external effects, and crash matrix are specified; runtime state is published only at the intended commit point or the documented ADR 0040 failure semantics are preserved; cancellation/close waits for admitted persistence.

### Tranche 3 — Durable submissions and task records (vertical slice)

**Goal:** add one concrete restart-resumable task kind plus a durable submission handle, using a store transaction and serialized scheduler seam. Start with one task (e.g. a product-approved background operation) rather than importing all built-ins.

**Prerequisites:** chosen task ID/state/checkpoint schema; definition version migration contract; retry/idempotency for non-transactional effects; cancellation and ownership semantics; bounded scheduling admission and close behavior; owner package and architecture manifest ruling.

**Acceptance:** crash at every phase boundary reopens deterministically; running becomes pending and repeated effects are controlled/documented; task state transition races, abort, wait, close, and storage failure are covered; architecture gate and no-exception constraints pass.

### Tranche 4 — Task ownership graph, inbox boundaries, hooks and registry evolution

Add only when demanded by concrete workflows. Treat ownership cascades/background boundaries, `failFast`, persistent inbox placement, task replacement/migration and task hooks as separate issue slices with a shared spec. The task graph projection follows persisted task records and is added after task semantics, not used as a proxy for them.

## Risk assessment

| Risk | Severity | Why / mitigation |
|---|---|---|
| Scope conflation | High | SQLite backend, transactional journal, and durable scheduler are separate capabilities in ADR 0066. Split rulings and issues. |
| False rollback promise | High | SQLite can atomically commit Pike-owned records but cannot undo provider/tool/process side effects or current live-state changes. Specify idempotency and publication timing. |
| Durability regression | High | pi Node adapter uses WAL + `synchronous=NORMAL`; Pike fsyncs JSONL appends. Set the intended crash class and sync mode explicitly. |
| Scheduler duplicate effects | High | Recovery re-dispatches nonterminal work; arbitrary phase effects may run twice. Require idempotent operations/checkpoint discipline. |
| Serialized loop blocking | High | SQLite API is blocking. Run on bounded workers, return via mailbox, preserve reserved persistence admission and close quiescence. |
| Data migration/compatibility | High | pi JSONL storage is not Pike session JSONL. One-time explicit importer only; version, validation, permissions and rollback need design. |
| File security/privacy | High | Session files are owner-only and no-follow. SQLite DB, WAL and SHM files need equivalent permissions, path handling, symlink checks and cleanup. |
| Overbuilding generic framework | Medium-high | Full upstream contract includes documents, tasks, submissions, Chord and hooks. Port only approved vertical slices; retain concrete facade. |
| Dependency/build burden | Medium | SQLite vcpkg dependency and system libraries affect all presets/architectural evidence. Probe pinned port and build impact first. |
| Failure diagnostics/corruption | Medium | SQLite corrupt DB handling and WAL recovery differ from parseable JSONL corruption/torn-line behavior; define user repair/export. |
| Multiple sessions/contention | Medium | One DB/connection can create lock contention; benchmark representative concurrency before database partition choice. |

## Draft issue specifications

### Draft A — Decide whether Pike adopts SQLite for Agent Session history

**Type:** spec / owner ruling. **Depends on:** none. **ADR:** update ADR 0066 SQLite row.

- Compare current Pike JSONL requirements against the actual user problem SQLite is intended to solve.
- Decide file format and compatibility: JSONL remains authoritative, one-time import, or deliberate replacement; no implicit mixed-format probing.
- Define crash durability class, `synchronous` policy, busy/lock behavior, backup/export, path permissions and symlink policy.
- Decide whether single DB, per-session DB, or per-workspace DB is in scope.
- Record operational/dependency/build cost and clear non-goals (general tasks/documents unless separately decided).

### Draft B — Prototype Pike SessionStore SQLite backend

**Type:** implementation after Draft A approves. **Owner:** `cch_agent_core` private session implementation; preserve `SessionStore` facade.

- Probe pinned vcpkg baseline for `sqlite3`; add manifest dependency only after the probe.
- Implement a private SQLite adapter with typed expected errors, prepared statements, transaction RAII/status, bounded worker execution, and explicit close/checkpoint.
- Preserve SessionStore append/tree/fork/import observable behavior and existing private file protections.
- Add fault injection at begin, statement, commit, checkpoint, and close; prove no partial logical entry transaction is accepted.
- Compare latency, startup, disk size, and lock contention against JSONL on representative histories; record sync mode and limits.

### Draft C — Specify durable task execution for Pike

**Type:** capability spec / owner ruling. **Depends on:** Draft A only if task persistence uses SQLite; otherwise define storage choice independently. **ADR:** update ADR 0066 durable execution row and relevant architecture/runtime ADRs.

- Select a single user workflow and one task kind for the first slice.
- Define task record, checkpoint, version migration, ownership, foreground/background, join, abort, and terminal outcomes.
- Define crash recovery at reserve/handler/checkpoint/finalize boundaries and duplicate-effect rules.
- Define submission IDs/deduplication and whether queued input survives restart.
- Define scheduler admission/worker limits, fairness, diagnostics, and quiescent close integration with RuntimeRoot.
- Define the task graph as a derived view only after task semantics exist.

### Draft D — Implement first durable task vertical slice

**Type:** implementation after Draft C. **Acceptance:** no exceptions; all state changes serialized; heavy code off loop; errors typed; cancellation cooperative; restart test matrix covers every transition; required architecture and full validation run. Keep task runtime behind the approved `cch_agent_core`/`cch_coding_agent` ownership seam and avoid exposing Boost.Asio or SQLite types through Owner Interfaces.

## Decision record guidance

Update ADR 0066 with independent rows for (a) SQLite persistence of Pike session history, (b) transactional multi-record session commits, and (c) durable tasks/submissions/scheduler. Record who decided, scope, user-visible parity claims, and explicit non-goals. If selecting an intentional implementation divergence, show why it improves the C++ caller contract and cannot be hidden behind a private adapter, as ADR 0053 requires. A private SQLite implementation alone does not require an Owner Interface or architecture manifest expansion; durable task capabilities likely require an ownership and lifecycle ruling before they require a new package.

## Verification and source pointers

This report was assembled by reading the upstream files listed below and Pike's corresponding implementations and architecture contracts. No code or product behavior was changed, and no builds/tests were run for this research-only deliverable.

| Area | Source files |
|---|---|
| Upstream storage | `pi/packages/durable/src/storage/sqlite/{database,migrations,storage,node}.ts`; `src/storage/jsonl/storage.ts`; `src/storage/memory.ts` |
| Upstream transactions | `pi/packages/durable/src/session/{transaction,session}.ts`; `src/documents.ts`; `src/types.ts` |
| Upstream runtime | `pi/packages/durable/src/harness/{scheduler,submissions,task-graph,inbox,harness}.ts`; `src/harness/{define,registry,agent,types}.ts` |
| Pike persistence | `src/agent/harness/session/{SessionJournal.hpp,SessionJournal.cpp,JsonlSessionStore.hpp,JsonlSessionStore.cpp,SessionStore.cpp}` |
| Pike compaction/runtime | `src/agent/harness/compaction/Compaction.cpp`; `src/coding_agent/AgentSessionExecution.cpp`; `src/coding_agent/runtime/{SessionPersistence,SessionFactory}.cpp` |
| Pike contracts | `docs/adr/0040-own-asynchronous-operations-and-the-serialized-runtime-lifecycle.md`; `docs/adr/0066-pi-capability-scope-decisions-and-open-questions.md`; `docs/agents/{architecture,validation,pi-parity}.md`; `vcpkg.json` |
