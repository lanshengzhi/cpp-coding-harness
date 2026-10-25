# #1071 legacy durable machinery: delivery reconciliation

## Scope

Reconcile #1071's explicit move-or-retire scope with the retirement already
merged into main in `d0844505d529b1cd37c869a0c0e69bfc892f34b8`, following
`97a5aaf73`'s removal of transcript export's scheduler coupling. The accepted
boundary is ADR 0075 / #1044 and the owner's #1059 ruling. This is an ownership
and retirement result, with no new durable capability claim.

## Delivered behavior

The unused agent-side `SqliteSessionStore`, `SqliteDatabase`, `TaskStore` and
`TaskScheduler` implementations and their private/test-only entrances were
retired. `SessionStore` retains its JSONL and memory paths. The independent
`TranscriptExportHandler` remains callable with a passive session snapshot and
requires neither a scheduler nor a task store. The public regression cases
exercise export/reload, branch order, repeat overwrite, owned snapshot lifetime
and cancellation before a file is written.

Following `97a5aaf73`'s removal of transcript export's scheduler coupling, the
retired SQLite and task paths had no production callers: the application used
JSONL/memory SessionStore, and export no longer required the scheduler. Current
export behavior also includes the subsequent `d22a96adf` format correction and
`45d3bac15` snapshot ownership change; the retirement commit's historical
headerless-export observation is not a description of the current tree.

The actual configured `cch_agent_core` dependency closure is inspected through
the CMake File API, rather than inferred from a removed include or source name.
The preserved source-level retirement search is an additional check only.
`cch_durable` owns the new memory/JSONL/SQLite backends and task execution;
retirement of the old scheduler makes no claim about completing that runtime.

## Validation

Current-tree focused runs on 2026-10-25, after building the full Debug preset:

- `ctest --preset vcpkg -L '^store$' -LE '^codemode$'`: 24 selected, 24 passed.
  The codemode exclusion removes one unrelated store test; the selection covers
  the retained JSONL/memory SessionStore behavior.
- `ctest --preset vcpkg -R '^transcript export '`: 9 selected, 9 passed, including
  reloadable output, active-branch ordering, owned snapshot lifetime and the
  stop-requested case that writes no destination.
- CMake File API `target-cch_agent_core-*.json`: the transitive project dependency
  closure is exactly `cch_ai`, `cch_support`. It contains no SQLite, application
  or frontend target.
- File API `target-cch_tests_agent-*.json`: actual library link fragments are
  `libcch_agent_core.a`, Catch2, `libcch_ai.a`, `libcch_support.a`, SSL, crypto,
  `dl` and utf8proc. No SQLite or application library is linked. This test target
  also has build-order prerequisites through the parity gate; those prerequisites
  are not link dependencies.
- A source search for `SqliteSessionStore`, `SqliteDatabase`, `TaskStore` and
  `TaskScheduler` under `src/agent`, `tests/harness`, `AgentCore.cmake` and
  `AgentTests.cmake` returns no matches. This supplements the behavioral and
  configured-link checks.

Full-suite and architecture results are recorded in the reconciliation report.
The original retirement commit's historical differential failure is not used as
current acceptance evidence.

## Capability status

No membership or verification status is changed by this internal retirement.
The already excluded legacy SQLite SessionStore and leaf-marker rows retain
#1059's visible boundaries. P7 in `1058-implementation-frontier.json` records
this retirement and points here; it does not require reimplementing an excluded
legacy store in the new Owner.

## Remaining boundary

Application Session/history/compaction, concrete environments and application
tools still belong to their pending migration slices #1068–#1073. The new
Durable Runtime's full task, submission, workflow and interchange obligations
remain open, including #1085/#1086. No old-Pike import/migration/fallback or user
data deletion is introduced. #1044 remains open.
