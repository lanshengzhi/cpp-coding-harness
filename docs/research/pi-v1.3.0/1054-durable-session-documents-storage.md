# pi-v1.3.0 durable Session, documents and interoperable storage (#1054)

- **Baseline**: `pi-v1.3.0` at `71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64 with glibc 2.44.
- **Authority**: [ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md), umbrella [spec #1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044), and ticket [#1054](https://github.com/lanshengzhi/cpp-coding-harness/issues/1054).
- **Scope**: audit only. This ticket adds no production behavior, no package move, no ownership relocation, no durable-storage change and no C++ implementation. It records frozen upstream observations, the current C++ implementation/consumer map, the exact formatting and recovery obligations, and the gaps that must be closed before `cch_durable` can be claimed.
- **Evidence family**: `fixtures/pi-v1.3.0/audit-1054/`, manifest SHA-256 `e9039b53cfccdf6caf62e09702377ca45e53e91cedbea4dee6d1b92d18e38a44` (before serial registration).
- **Machine ledger**: [1054-durable-session-documents-storage.json](1054-durable-session-documents-storage.json).

The ledger has 111 rows: 109 supported and 2 excluded. No row is `verified` against C++; 83 are
`upstream-captured`, 26 are `unverified`, and 2 are `not-applicable`. Implementation status is
95 `missing`, 16 `partial` and 0 `implemented`. A `partial` row names the analogous existing
application behavior and the part that cannot be reused; it is not a claim that the durable
contract exists.

| Ledger group | Rows | Coverage |
| --- | --- | --- |
| `Session` | 13 | Transaction line, settlement visibility, rollback/failure, close, publications, snapshot/as-of, state/watch, retention |
| `Tx` | 19 | Table reads/scans, conversation/task/submission operations, document access/retirement, write ordering, task attribution |
| `Entry` | 7 | Typed definitions, built-in kinds, record fields, head markers, edits, fork ancestry |
| `Document` | 13 | Definitions/families, scopes, versions, base/delta/checkpoint, as-of, fork copy, retire, address identity, task scope |
| `Identity`/`Query` | 4 | Global numeric IDs, commit sequence, cursors, filters |
| `Storage` | 45 | Generic contract plus memory, JSONL and SQLite backends and reclaim/recovery |
| `Chord` | 5 | Context/abort, drafts, deltas, attached state, strict JSON |
| `Interchange` | 5 | pi JSONL/SQLite creation, both C++ directions, alternating writers, historical-format exclusion |

## Method and evidence

The capture runs public `createSession`, `defineDoc`, `defineDocFamily`, `defineEntry`,
`defineTask`, `openNodeJsonlStorage` and `openNodeSqliteStorage` against a private frozen
checkout at the registered revision. A guard refuses a different revision, a tracked-dirty
source tree, a non-Linux/x64/glibc host, or a different baseline. The checkout's ignored
`node_modules` was supplied from the frozen audit workspace; no tracked source file changed.
The capture re-executes under the checkout's `tsx` with `--conditions=source` so package
exports resolve to source.

```bash
PI_CHECKOUT=/tmp/cch-1054-pi-frozen PI_BASELINE=pi-v1.3.0 \
  /tmp/cch-1054-pi-frozen/node_modules/.bin/tsx \
  fixtures/pi-v1.3.0/audit-1054/capture-1054.mts
python3 scripts/evidence/audit_evidence.py check \
  --manifest fixtures/pi-v1.3.0/audit-1054/manifest.json
```

The capture produced five artifacts with 23 scenarios on `node v26.12.0`, `tsx 4.22.1`:

| Artifact | Scenarios | What it pins |
| --- | --- | --- |
| `session-transactions.json` | 10 | Commit publication, prepared-draft invisibility, callback rollback, storage failure, close, watches/states, as-of history, retirement, forks, cursors, retention |
| `backend-contract.json` | 1 | Memory/JSONL/SQLite produce the same observations for one public operation sequence, including reopen |
| `jsonl-store.json` | 6 | Main marker/sidecar bytes, append and fsync ordering, checkpoint/reclaim, retirement, torn-tail recovery, corruption refusal, poison/reopen |
| `sqlite-store.json` | 4 | Exact schema/indexes/metadata rows, prefix isolation, batch rollback, newer-version refusal |
| `interchange.json` | 2 | pi-created JSONL and SQLite dummies reopen, expose unfinished state and continue in pi |

The exact artifact bytes and generator digests are pinned by the manifest; this report does not
duplicate them. The SQLite dummies are retained as gzip+base64 because the raw database is
118,784 bytes (4,808 bytes encoded). The JSONL dummy is retained as its exact file texts.

### What the capture proves and does not prove

- It proves the frozen pi behavior named in each scenario, including the transaction success,
  callback-failure and storage-failure paths.
- It proves that the retained JSONL and SQLite fixtures can be closed, reopened and continued
  by pi, and that the reopens observe the same unfinished task, document revisions and checkpoint.
- It does **not** run a C++ durable writer. The C++-create, C++-continue and C++-reopen legs are
  specified below but are unverified because `cch_durable` does not exist. No row is promoted to
  `verified` from a pi-only capture.
- It does not exercise filtered task/submission scans, task invocation (`byTaskId`), context
  edits, document schema migration, or Cloudflare-specific hosting. Those rows are recorded as
  unverified or excluded rather than inferred.

## Acceptance evidence map

| Ticket criterion | Evidence | Status |
| --- | --- | --- |
| Complete durable operation/consumer/status inventory | Ledger rows `cch_durable:*`, grouped by Session kernel, transactions, entries, documents, queries/identities, storage, memory/JSONL/SQLite, Chord, interchange | Complete |
| Public transaction with a failure/rollback distinguishing case | `session-transactions.json`: commit publication, prepared-draft invisibility, callback rollback, storage failure | Captured |
| JSONL dummy generated, closed, reopened, continued | `interchange.json`: JSONL scenario, 939-byte `main.jsonl`, `doc-4.jsonl`, `doc-5.jsonl`, `task-6.jsonl` | Captured; C++ leg absent |
| SQLite dummy generated, closed, reopened, continued | `interchange.json`: SQLite scenario, 118,784-byte `storage.sqlite` | Captured; C++ leg absent |
| JSONL ordering/commit/reclaim/recovery documented | `jsonl-store.json` and the JSONL obligations section | Captured |
| SQLite schema/version/prefix/transaction obligations documented | `sqlite-store.json` and the SQLite obligations section | Captured |
| Exact C++ differences | Current-code diff section, with source paths | Recorded |
| Bidirectional opening and alternating writers | Interchange acceptance section | Unverified: no C++ durable implementation |

## Session transaction semantics

The Session is a single serialized transaction line over a `Storage` backend. A commit callback
receives a `Tx`; drafts and table writes are admitted during the callback, and the whole batch is
handed to `Storage.commit`. Only after the backend settles does the Session adopt the new
revision and synchronously publish one `CommitPublication` with the commit sequence and ordered
changes.

| Scenario | Observed |
| --- | --- |
| One commit changes an entry and four documents | Conversation 2 published at seq 1; commit seq 2 published entry 3, document 4 (`audit1054.latest`), 5 (`audit1054.counter`), 6 (`audit1054.member`) and 7 (`audit1054.rewind`) with their committed values |
| Prepared draft before settlement | While `Storage.commit` was held, snapshot stayed `{text:"committed",revision:1}` and publication count stayed 2; release adopted `{text:"pending",revision:2}` and added exactly one publication at seq 3 |
| Callback throws after drafting | Callback rejected with `callback failed`; snapshot stayed `baseline`/v1, persisted entries stayed empty, publication count stayed 2; a later commit succeeded and published seq 3 |
| Storage commit rejects | Commit rejected with `disk vanished`; no publication; later `snapshot` and `commit` rejected `SessionFailed` with `Session failed after a storage error; close and reopen it`; `Session.closed` reported `{reason:"failed",errorMessage:"disk vanished"}` |
| Close | Close listener ran exactly once; a later commit rejected `Session is closed`; `Session.closed` reported `{reason:"closed"}` |
| Watch and state | Watch delivered one frame with the committed value and exact ops; `stop()` returned `{reason:"stopped"}` and Session close returned `{reason:"session_closed"}`; attached state observed the committed value until disposal |

The distinguishing property is therefore:

- **Callback failure is a rollback.** The Session remains usable, nothing is published, and a
  later transaction can commit.
- **Storage failure is a Session failure.** The Session is poisoned/closed for admission, later
  operations reject with `SessionFailed`, and nothing is published. Reopening the storage is the
  recovery path; the failed batch is not retried by Session.

An eager implementation that published on draft mutation would expose `pending` while the
backend was held, and would leave `lost` visible after the callback threw. Both observations are
rejected by the captured snapshots and publication counts. The failure is injected by wrapping
the public `Storage` interface, matching the frozen `ControlledStorage.failNextCommit` precedent;
no private scheduler or SQL state is touched.

`session-documents.test.ts` ("fails the Session after a Storage failure and publishes nothing")
and `session-support.ts` `ControlledStorage.failNextCommit` are the frozen narrow-storage-boundary
precedent for this distinction. The capture uses the public `Storage` proxy boundary, not a
private scheduler.

## JSONL obligations

A durable JSONL store is a directory, not a single application transcript:

```text
<store>/
  main.jsonl          committed transaction markers only
  doc-<id>.jsonl      one record per document incarnation change
  task-<id>.jsonl     live task sidecar, removed at terminal publication
```

### Main marker

`main.jsonl` is newline-delimited JSON. Each line is a complete commit:

```json
{"format":1,"type":"commit","seq":2,"writes":[{"type":"entry","value":{"kind":"audit1054.note","data":{"n":1},"id":3,"conversationId":2}},{"type":"document.create","record":{"id":4,"kind":"audit1054.latest","scope":{"kind":"conversation","conversationId":2},"history":"latest","fork":"current"},"ordinal":0}]}
```

Observed write variants are `conversation`, `entry`, `task.sidecar`, `document.create`,
`document.change` and `document.retire`; document writes carry an `ordinal` identifying the
sidecar record within the commit. `format` is 1. The marker is the commit point: a sidecar
record is confirmed only by a later marker that names it.

### Sidecar record

```json
{"format":1,"type":"record","seq":2,"ordinal":0,"payload":{"type":"document","id":4,"content":{"version":1,"kind":"base","value":{"text":"ordered","revision":1}}}}
```

- `seq` is the commit sequence that confirmed the record; `ordinal` matches the marker write.
- A document payload is `{version,kind:"base",value}` on first access or checkpoint, and
  `{version,kind:"delta",ops:[[...]]}` for ordinary updates. Ops are Chord wire operations, e.g.
  `["s",["text"],"two"]`.
- A task payload is `{type:"task",value:{...}}` and holds the latest complete task record.

### Ordering

For each commit, every affected sidecar record is appended first, then `main.jsonl`'s marker is
appended. The observed public `FileSystem` append order was `["doc-4.jsonl","main.jsonl"]`. With
`fsync: true`, the sidecar was flushed before the marker append. A backend may not publish a
commit before its marker is durably appended. The marker must be the last write; a crash before
it leaves the sidecar records unconfirmed and recoverable by truncation.

### Commit, reclaim and recovery

- A base record is created on first document access. Ordinary updates append deltas.
- `checkpointWhen` replaces the accumulated base+delta history with a new base line; the old
  bytes are reclaimed. After a checkpoint, the capture observed one base at seq 4 rather than
  base+delta history.
- Retiring a document appends a `document.retire` marker and removes the document sidecar. The
  main markers remain; a live task sidecar is removed when its terminal record is published.
- On open, a torn final line in `main.jsonl` is dropped, and a complete but unconfirmed sidecar
  tail is truncated. The capture appended a torn marker and an unconfirmed sidecar record after
  a clean close; reopen returned the last published value and restored both files byte-identically
  to their pre-crash contents.
- A marker whose confirmed sidecar record is missing is corruption, not a partial commit:
  `JsonlCorruptionError: Missing confirmed sidecar record doc-3.jsonl at sequence 2`.
- Main sequences must strictly increase. Source and tests define
  `Commit sequence does not strictly increase in main.jsonl`; this bundle does not capture that
  branch.
- A commit I/O failure poisons the JSONL instance: the commit and later reads reject
  `JsonlStoragePoisonedError: JSONL storage is poisoned and must be reopened`. Reopening recovers
  the published prefix and discards the unconfirmed batch. In the captured main-marker failure,
  reopen retained conversation 2 and returned no entries.
- JSONL has no cross-process writer promise beyond the upstream storage contract. Alternating
  writers require close/reopen; simultaneous independent writers are not a supported claim.

## SQLite obligations

### Exact schema

All durable tables are `STRICT`; JSON-bearing columns use `CHECK (json_valid(column))`. The
observed schema version is 1. The tables and their summary columns are:

| Table | Columns (summary) |
| --- | --- |
| `durable_schema` | `singleton INTEGER PRIMARY KEY CHECK(singleton=1)`, `version INTEGER NOT NULL CHECK(version>=0)` |
| `durable_metadata` | `singleton` (1), `next_id TEXT NOT NULL`, `next_seq INTEGER NOT NULL` |
| `record_ids` | `id INTEGER PRIMARY KEY`, `record_type TEXT CHECK IN ('conversation','entry','task','submission','document')` |
| `conversations` | `id INTEGER PRIMARY KEY`, `owner_conversation_id`, `owner_task_id`, `record TEXT` |
| `entries` | `id INTEGER PRIMARY KEY`, `conversation_id`, `head`, `commit_seq`, `record TEXT` |
| `tasks` | `id INTEGER PRIMARY KEY`, `conversation_id`, `kind`, `status`, `abort_requested`, `background`, `record TEXT` |
| `submissions` | `id INTEGER PRIMARY KEY`, `conversation_id`, `request_id`, `status`, `record TEXT` |
| `documents` | `id INTEGER PRIMARY KEY`, `kind`, `family`, `key_value`, `scope_kind`, `owner_id`, `created_at`, `retired_at`, `record TEXT` |
| `document_revisions` | `document_id`, `seq`, `kind` (`base`/`delta`), `version`, `content TEXT`, `PRIMARY KEY(document_id,seq)` |

Checks include task status `IN ('pending','running','waiting','completing','terminal')`,
submission status `IN ('queued','placed','done','unanswered')`, scope
`IN ('session','conversation','task')`, document family `IN (0,1)`, and revision kind
`IN ('base','delta')`.

Named indexes observed: `conversations_by_owner_conversation`,
`conversations_by_owner_task`, `entries_by_conversation`, `entry_heads_by_conversation`
(partial `WHERE head IS NOT NULL`), `tasks_by_status`, `tasks_by_conversation`, `tasks_by_kind`,
`tasks_by_abort_requested`, `tasks_by_background`, `submissions_by_request`,
`submissions_by_conversation`, `submissions_by_status`, `documents_by_address`,
`documents_by_scope`, `documents_by_scope_kind`, and `document_revisions_by_kind`.
`document_revisions` also has the implicit `sqlite_autoindex_document_revisions_1` from its
composite primary key.

### Version, prefix and transaction obligations

- Schema application creates `durable_schema` with version 0, then applies contiguous migrations
  from 1 atomically. A stored version newer than the supported version refuses open:
  `Durable SQLite schema version 2 is newer than supported version 1`. A version gap in the
  migration list is an error. The capture constructs the newer-version input by changing the
  singleton row outside the runtime after close; that is fixture construction, not a private
  transaction seam. All transaction and rollback observations use public `Storage.commit`.
- A prefix is literal concatenation of identifiers; the caller supplies any separator. The
  prefix must match `/^[A-Za-z_][A-Za-z0-9_]*$/` and must not start with `sqlite_` (case
  insensitive). Prefix `t1054_` produced `t1054_conversations`, `t1054_documents`, all named
  indexes and the other durable tables. Opening the same file without the prefix saw no
  conversation; reopening with the prefix saw it. Prefixes isolate schemas in one database.
- Every commit runs in one immediate SQLite transaction (`BEGIN IMMEDIATE`), including ID/seq
  allocation and all row writes. Any failure rolls back every row and leaves `next_id`/`next_seq`
  unchanged. The capture inserted conversation 3, then a task row violating the status CHECK;
  conversation 3 and its `record_ids` row disappeared, and the next successful commit returned
  seq 2. The exact failure was
  `CHECK constraint failed: status IN ('pending', 'running', 'waiting', 'completing', 'terminal')`.
- `durable_metadata.next_id` is TEXT because `node:sqlite` rejects INTEGER results outside the
  JavaScript safe-integer range; `next_seq` is INTEGER. `record_ids` is the global ownership
  ledger: every record and document ID is claimed there, so IDs cannot collide across record
  kinds.
- Documents record the creation commit in `created_at` and retirement in `retired_at`; the
  current row points at the latest revision. Revisions preserve bases and deltas by commit
  sequence, so rewindable/as-of materialization can replay from the last base at or before the
  requested entry commit.
- The adapter opens with `PRAGMA journal_mode = WAL`, uses `BEGIN IMMEDIATE`, and checkpoints the
  WAL with `PRAGMA wal_checkpoint(TRUNCATE)` on close. The captured closed fixture reopened as
  `journal_mode=wal`. `busyTimeoutMs` and WAL auto-checkpoint page options exist; non-default
  values are not captured here.
- SQLite files are a logical-row contract, not a byte-identical serialization contract. C++ must
  produce the same schema identifiers, constraints, metadata and observable rows/revisions; it
  need not reproduce SQLite's page layout byte-for-byte.

## Exact differences from the current C++ implementation

There is no `cch_durable` target and no C++ durable Session, document, Chord, or generic
`Storage` implementation. The current application-owned stores are separate contracts that ADR
0075 explicitly says must not be relabeled as durable storage. Current durable-specific stores,
journal and task machinery are to move to `cch_durable`; this ticket does not perform that move.

| Area | pi-v1.3.0 durable contract | Current C++ | Consequence |
| --- | --- | --- | --- |
| Owner and seam | `cch_durable`: public `Storage`, `Session`, `Tx`, document and Chord seams | `SessionStore` is a closed concrete facade in `src/agent/include/cch/agent/harness/session/SessionStore.hpp`; no generic storage interface | A durable implementation cannot be injected or conformance-tested through the application facade |
| Session transaction | One commit callback; atomic write batch; adopt after backend settlement; publish commit sequence | `SessionStore::append`/`commit_batch`; SQLite `append_batch` wraps one batch internally | No callback transaction, no prepared/adopted split, no commit publication |
| Failure lifecycle | Callback failure rolls back and keeps Session usable; storage failure fails/poisons Session | Append failures return per-call errors; the facade has no Session-failed latch or close admission seal | Failure semantics are not equivalent; retry behavior must not be inferred |
| Identity | Session-global numeric IDs minted through `Storage.mintId`; one global `record_ids` space | 8-character random hex entry IDs; TEXT session/task/submission IDs; SQLite does not allocate global numeric IDs | Existing IDs cannot be translated by relabeling; a durable store needs upstream-compatible integer allocation |
| Entry kinds | `defineEntry` typed kinds plus built-in `pi.user`, `pi.assistant`, `pi.system`, `pi.tool-result`, `pi.reset`, `pi.compaction`; `head`, `edits`, `byTaskId` | Closed application `SessionEntry` variants (message, model change, thinking level, label, compaction, branch summary, session info, leaf); TEXT parent chain, no `head`/`edits`/`byTaskId` | Durable entry records and typed guards are missing; application transcript kinds are not the durable wire |
| Documents | Singleton/family definitions, scopes, latest/rewindable history, fork modes, version/migrate, checkpoints, retirement, watches/states, retention | No document layer at all | The largest implementation gap; documents are not derivable from transcript entries |
| Chord | `Op`/delta, draft tracker, attached replicated state, Context/abort | No Chord package or equivalent | Durable documents and watches depend on the Chord contract; it is tracked separately |
| JSONL layout | Directory with `main.jsonl` commit markers plus `doc-N.jsonl`/`task-N.jsonl` sidecars | One application transcript file: first line `{"type":"session","version":3,...}`, then one entry line per append | The current file is explicitly a separate coding-agent contract; it is not a durable store |
| JSONL append | Sidecar bytes first, optional fsync, then main marker; marker confirms the batch | `SessionJournal::append_line` appends and fsyncs one line; header/entries are the only records | No sidecars, commit sequence, marker confirmation, reclaim, poison or transactional reopen |
| JSONL recovery | Drop torn main tail and unconfirmed sidecar tail; fail closed on missing confirmed sidecar/non-increasing seq | `SessionJournal::read_lines` returns every line; `EntrySerializer::parse_lines` rejects malformed JSON; no truncation or sidecar confirmation | Crash recovery and corruption semantics differ; a C++ durable reader cannot use this parser |
| SQLite schema | Nine strict tables with `durable_schema`, `durable_metadata`, `record_ids`, documents/revisions, prefixing and version refusal | `conversations(session_id TEXT PK, created_at, updated_at)`, `entries(id TEXT PK, session_id, parent_id, entry_type, payload, created_at)`, `inbox`, plus `TaskStore` tables; no schema version, prefix, global IDs, documents or revisions | Table overlap is not interoperability; current SQLite is not openable as a durable store |
| SQLite tasks | Numeric ID, conversation, kind, five-state status, abort/background, JSON record | TEXT ID, kind, six-state state (`completed`/`aborted` included), checkpoint/owner/definition-version/timestamps/parent task columns | Different identity, status vocabulary and columns; see #1055 for task ownership |
| SQLite submissions | Numeric ID, conversation, optional request ID, four-state status, JSON record | TEXT ID, task ID foreign key, payload/kind/state, no conversation or request key | Different scope and deduplication contract; see #1055 |
| Queries | Cursor pages, scan order, owner/task/submission filters, global entry lookup, head-marker query | SessionTree queries, transcript reads, task/submission store reads; no durable cursor/filter contract | Consumers must not treat tree traversal as a durable scan |
| Storage conformance | `registerStorageConformance` exercises memory, JSONL and SQLite through one public contract | Application-specific store tests; no shared storage conformance target | A single backend cannot be claimed conformant from its own tests |
| Interchange | Bidirectional JSONL/SQLite open and continued operation with pi v1.3.0 | No durable reader/writer; application session files remain separate | The C++ legs of the acceptance workflow cannot run yet |

Current `SqliteDatabase` already provides WAL and `BEGIN IMMEDIATE`, which are useful building
blocks, but they do not supply the durable schema, versioning, prefixing, ID/sequence ledger,
document revisions or commit publication contract.

### Current C++ consumer map

| Current seam | Current caller/use | Durable target |
| --- | --- | --- |
| `SessionStore` facade | `SessionLifecycle`, `SessionPersistence`, `SessionFork`, TUI `SessionFlowController`/`InteractiveEngineHost`, compaction and transcript export | `cch_durable` Session/transaction line; ordinary application Sessions must remain separate |
| `SessionJournal` + `EntrySerializer` | Only through `SessionStore`; JSONL header/entry lines and fsync per append | Replaced by the transactional main/sidecar format; the application transcript remains its own contract |
| `SqliteSessionStore` | Reached through `SessionStore::create_sqlite`/`open_sqlite`; no application lifecycle caller found (tests only) | Durable SQLite schema v1 and migration/prefix contract |
| `TaskStore` + `TaskScheduler` | `TaskScheduler` is constructed only by its tests; no production application caller found | Durable task records/submissions; scheduling audited in #1055 |
| Document observation | None: no document layer, watch, state or retention exists | `cch_durable` documents over `cch_chord` (#1057) |

The SQLite session factories being present but not on the application lifecycle path is a
current-state fact, not a reason to treat that schema as durable interoperability.

## Mapping to `cch_durable` and `cch_chord`

The target owner is `cch_durable`; the current code remains application-owned until a later
move. The ledger uses `cch_durable:*` identifiers to state the target capability, not to claim
the move happened.

| Target capability group | Required upstream seams | Ledger evidence |
| --- | --- | --- |
| Session kernel | `createSession`, `Session.commit/close/snapshot/snapshotAsOf/documentState/watchDoc`, commit/close subscriptions, retention | `session-transactions.json` |
| Transaction tables | conversation/entry/task/submission reads and scans, `latestHeadMarker`, `createConversation`, `forkConversation`, `appendEntry`, `createTask`, draft/settle submissions | `session-transactions.json`, `interchange.json`; task/submission lifecycle partly deferred to #1055 |
| Documents | `defineDoc`, `defineDocFamily`, `Tx.doc`, `Tx.retireDoc`, base/delta/checkpoint, as-of, fork copy, version/migrate | `session-transactions.json`, `jsonl-store.json`, `sqlite-store.json` |
| Storage contract | `commit`, `mintId`, record lookups/scans, document lookup/materialization/scans, `close` | all five artifacts |
| Memory backend | Full `Storage` contract and prepared-commit isolation | `backend-contract.json` |
| JSONL backend | Directory layout, main markers, sidecars, ordering/fsync, reclaim, recovery, corruption, poison | `jsonl-store.json`, `interchange.json` |
| SQLite backend | Strict schema, migrations/version, prefix, metadata/ID ledger, transactions, WAL/close | `sqlite-store.json`, `interchange.json` |
| Chord contracts | Context/abort, draft/tracker, delta ops, attached state, strict JSON | `session-transactions.json`; ownership tracked in #1057 |
| Interchange | Bidirectional JSONL/SQLite continuation and alternating writers | `interchange.json` (pi legs only) |

## Interchange acceptance workflow

The retained fixtures are the starting point for the bidirectional acceptance test:

- JSONL: materialize `main.jsonl`, `doc-4.jsonl`, `doc-5.jsonl`, `task-6.jsonl` from
  `interchange.json` (`expected.fixture.files[].text`). The fixture has conversation 2, entries
  3 and 7, task 6 pending with checkpoint `{phase:"start",n:7}`, document 4 latest at
  `{text:"checkpoint",revision:3}`, and document 5 rewindable at `r2` with as-of `r1`.
- SQLite: base64-decode and gunzip `expected.fixture.sqlite.gzipBase64` to `storage.sqlite`
  (SHA-256 `4cdcc3fb5f7accdab53053a047af148f37b9a773eb15e244717ad13063ee0495`). It carries
  the same logical state.

The required acceptance sequence, both directions, is:

1. **C++ create**. Use only public `cch_durable` APIs to create a JSONL store and a SQLite store
   through the same operation sequence as the captured pi fixtures: conversation 2, entries 3
   and 7, task 6 pending with checkpoint `{phase:"start",n:7}`, document 4 latest
   (`base`, `delta`, checkpoint base) and document 5 rewindable (`r1`, `r2`). Close both stores.
2. **C++ reopen**. Reopen its own stores, assert the exact IDs, entry order, task checkpoint,
   latest `{text:"checkpoint",revision:3}`, rewindable `r2`, as-of first `r1`, and `mintId` 8.
   Append entry 9 and update both documents; close.
3. **pi reopen**. Open the C++ stores with the frozen pi code. Assert the same observations,
   including the pending task and both document revisions. Continue in pi only after those
   assertions. This is the leg the current bundle cannot run.
4. **pi create, C++ continue**. Use the retained pi fixtures or regenerate them with the capture.
   Open them in C++, assert the same observations, append the next entry and update both
   documents, close, and reopen in pi to verify entry 9, latest revision 4 (`after-cpp-leg`) and
   rewindable `r3`.
5. **Recovery and alternating writers**. Repeat with a crash before the main marker, after the
   sidecar append, and after a checkpoint/retire; require the same recovered prefix and no
   resurrection of unconfirmed writes. Alternate pi and C++ after each clean close/reopen. Do not
   claim simultaneous independent writer support.

For JSONL, acceptance includes the exact main-marker/sidecar ordering, sequence monotonicity,
reclaim of retired/checkpoint sidecars, and truncation of torn/unconfirmed tails. For SQLite,
acceptance includes the schema version/`STRICT`/`json_valid` constraints, prefix isolation,
global `record_ids` allocation, and all-or-nothing batch rollback. Byte-identical SQLite files
and byte-identical JSON formatting are not required; decoded records, ordering and observable
recovery are.

## Missing implementation and bounded next steps

No production fix is made here. The following gaps are explicit and must be scheduled under the
durable owner, not hidden as deferrals:

1. Introduce the `cch_durable` target and its public `Storage`/`Session`/`Tx` seams, then a
   memory backend that passes the upstream storage-conformance scenarios.
2. Implement the Session commit line, committed-only adoption/publication, callback rollback,
   storage-failure latch and close lifecycle.
3. Add the Chord delta/state/context contract needed by documents (owner `cch_chord`, #1057).
4. Implement document definitions, scopes, families, base/delta/checkpoint, as-of history, fork
   copy, retirement, watches/states, retention and version migration.
5. Implement the JSONL directory format: main markers, document/task sidecars, sidecar-first
   ordering/fsync, reclaim, torn-tail recovery, corruption checks and poison/reopen.
6. Implement the SQLite schema v1, migrations/version refusal, validated prefixing, metadata and
   `record_ids`, strict constraints, immediate transactions and WAL close behavior.
7. Move the durable-specific stores/journal/task machinery to the durable owner and add
   bidirectional JSONL/SQLite acceptance tests against the retained fixtures.
8. Close the task/submission integration gaps under #1055 and the durable Harness invocation
   gaps under #1056; this ticket records only the Session persistence side.

## Validation

- Capture completed: 5 artifacts, 23 scenarios, manifest SHA-256
  `e9039b53cfccdf6caf62e09702377ca45e53e91cedbea4dee6d1b92d18e38a44`.
- `python3 scripts/evidence/audit_evidence.py check --manifest
  fixtures/pi-v1.3.0/audit-1054/manifest.json` passed and reported the family manifest and hash.
- The capture guards the frozen revision, clean tracked tree, Linux/x64/glibc environment and
  baseline before writing.
- Documentation check: headings, relative links, tracker references and machine JSON were
  validated; no C++ build is required for this audit-only, docs/evidence change.

## Residual limitations

- No C++ durable implementation exists, so the bidirectional/interchange rows remain
  `unverified`; pi-only captures are not C++ acceptance.
- Task/submission lifecycle and Harness attribution are audited elsewhere (#1055/#1056); the
  ledger marks those rows rather than guessing their behavior.
- Chord ownership and semantics are tracked under #1057; only durable-facing observations are
  recorded here.
- Cloudflare SQLite hosting is excluded by ADR 0075; the generic SQLite obligations remain in
  scope.
- Non-default SQLite busy-timeout/WAL-threshold options, document migrations, context edits and
  filtered task/submission scans are not captured by this bundle.
