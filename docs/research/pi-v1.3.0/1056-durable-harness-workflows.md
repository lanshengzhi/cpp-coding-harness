# pi-v1.3.0 durable Harness workflows and Linux integrations

Ticket: #1056 (A12), parent #1044.
Authority: [ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md),
[frozen evidence protocol](../../../fixtures/pi-v1.3.0/README.md) and the #1044 integration workflow.

## Result in one paragraph

The frozen pi v1.3.0 public durable Harness was exercised against a private checkout at
`71023aec1546d6d677a65669c5665c0927ce0dd0` on Linux x86-64/glibc. Seven evidence artifacts record
32 public scenarios: Harness lifecycle and admission, conversation creation/fork/reset/abort/reopen,
generation retry and settlement, manual/threshold/declined/no-cut compaction, registry and hook
transitions, inspection/task graph/view/event/context observation, real local environment and coding
tools, image processing and read outcomes, code mode with a real nested tool, tool search, JSONL
reopen and SQLite reopen. The current C++ tree has no `cch_durable` target or public Harness. It has
useful parts under the configured `cch_agent_core` and `cch_coding_agent` owners (session store/tree,
compaction door, task store/scheduler, local filesystem/shell, built-in tools, application
codemode/tool search, image input), but none is the independent durable Harness contract. This is an
audit and executable-evidence ticket: no production code, ownership graph, shared registry, shared
verifier, shared test or README was changed.

## Baseline and capture method

- Baseline name: `pi-v1.3.0`.
- Exact revision: `71023aec1546d6d677a65669c5665c0927ce0dd0`.
- Private checkout: `/tmp/cch-1056-pi-frozen`; `git rev-parse HEAD` matched the revision and
  `git status --porcelain` was empty before capture. The checkout's `node_modules` contains the
  frozen dependencies plus the two runtime packages needed by durable imports
  (`@silvia-odwyer/photon@0.3.3`, `quickjs-wasi@3.6.2`); those additions are inside the private,
  gitignored checkout only and do not mutate a shared checkout.
- Platform: Linux `x64`, glibc `2.44`, Node `v26.12.0`, tsx `4.22.1`.
- Capture command:
  `NODE_OPTIONS=--conditions=source PI_CHECKOUT=/tmp/cch-1056-pi-frozen PI_BASELINE=pi-v1.3.0 /tmp/cch-1056-pi-frozen/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1056/capture-1056.mts`
- The capture refuses to write unless the full revision matches, the checkout is clean, the platform
  is Linux/x64/glibc, and `packages/durable/src/index.ts` exists.
- All model output is scripted with the frozen faux provider; all filesystem/shell operations use
  temporary local directories. No live provider, credentials, `~/.pi` state or shared upstream
  checkout was used.
- Normalizations are recorded per scenario in the artifacts and manifest. They cover generated
  identity relations, wall-clock durations/wall time, temporary roots, event batching and the
  scripted summary text. Presence/absence, ordering, errors, state transitions and settlement
  outcomes were not normalized away.

## Evidence bundle

Manifest: `fixtures/pi-v1.3.0/audit-1056/manifest.json`.
Manifest SHA-256: `97b9f7e760fa4183d4c8a2038476b328699911f21cce89e5286199adff7457a2`.

| Artifact | Scenarios | Content |
| --- | ---: | --- |
| `workflow.json` | 4 | Public admission/tool/settlement; live registry transition; generation `onYield`; tool hooks |
| `conversations.json` | 6 | Root creation and five documents; fork as-of; reset/handoff; task ownership; abort; SQLite close/reopen |
| `compaction.json` | 4 | Manual; blocking threshold; `beforeCompact` decline; no eligible cut |
| `observability.json` | 6 | Inspect task states; inspect submission; task graph; view delta frames; event stream; context at entry |
| `environment.json` | 6 | Real Node filesystem/shell; coding tools; Photon resize; read resized image; absent processor; inline pass-through |
| `codemode-toolsearch.json` | 4 | Nested-tool script and store; failure result; deferred tool load; no-match |
| `generation-storage.json` | 2 | Retryable provider error; JSONL write/reopen |

Every artifact repeats the baseline, revision, capture timestamp, environment, source endpoints and
scenario records. The manifest pins the artifact bytes, sizes and SHA-256, the generator and every
helper source digest. The shared verifier accepted the unregistered candidate:

```text
python3 scripts/evidence/audit_evidence.py check \
  --manifest fixtures/pi-v1.3.0/audit-1056/manifest.json
{
  "audit-1056": {
    "manifest": "audit-1056/manifest.json",
    "manifestSha256": "97b9f7e760fa4183d4c8a2038476b328699911f21cce89e5286199adff7457a2"
  }
}
```

## What the public workflow actually observed

The first workflow starts from `Harness.open(MemoryStorage, {models, registry})`, obtains the root and
admission through `root.submit`. The settled receipt keeps the submission ID, identifies the root
conversation, and has no request ID. The committed transcript is user, system, assistant tool call,
tool result and final assistant. The injected local `echo` tool executed exactly once with
`{text:"hello"}`, returned `echo:hello` for call `call-1`, had no error and carried a duration. The
system entry contained the `audit-note` section and the offered tool list was `["echo"]`.

The registry transition installed a second extension after root creation. The next request offered
`["first","second"]`, added the `late-section` prompt section, and recorded `toolsAdded` as
`["first","second"]`; this is the concrete omitted-integration transition requested by the ticket.
The generation hook scenario observed two yields (`first answer`, `second answer`), two faux model
calls, a continued user entry `go on`, and one `done` settlement. The tool hook scenario observed the
original arguments, rewrote them to `{text:"rewritten"}`, observed `echo:rewritten` after execution,
and committed that rewritten result without error.

Conversation scenarios observed the root in one commit with `pi.agent`, `pi.inbox`, `pi.live`,
`pi.provider` and `pi.usage`; forks at two entry positions with independent copies and as-of thinking
levels (`low` vs `high`); reset with `head` pointing at itself and handoff text as the first context
user message; a task-owned child conversation with the task as owner and the root as owner
conversation; abort settling the input `unanswered`/`aborted`; and SQLite close/reopen preserving the
root, child and fork IDs, thinking level and provider session IDs.

Compaction captured the exact manual placement (`completed`, placement `done`, marker at the cut,
raw history preserved), the threshold marker preceding the protected request, a `beforeCompact`
decline producing no marker and no summary request, and a no-cut manual request completing without a
summary or marker. Observation captured inspect's `paused`/`ready`/`running`/`waiting`/`blocked`
states, a placed submission with its generation task, a parent/child graph with an owned conversation,
three view delta frames converging on fresh hydration, the ordered event type sequence including
`tool_execution_start`/`update`/`end`, and context-at-entry equaling the fork's starting entries.

The Linux environment scenario observed real file append/read/list/stat, a missing-file
`not_found`, shell stdout/stderr split with exit code 3, an argv command, and a `timeout` error after
0.1 seconds. The coding-tool scenario executed `write`, `read`, `edit`, `read`, `bash` in sequence and
observed the final file `omega\n`. Photon passed an upright 40x20 PNG through byte-identically and
resized the same image to 10x5 under a 10x10 limit; invalid bytes returned `undefined`. The read tool
returned a 216-byte-base64 image plus the exact coordinate-mapping diagnostic. A BMP without a
processor returned the explicit unsupported-image result, while an inline 2x2 PNG passed through
unchanged.

Code mode ran a QuickJS script through the frozen public `CodemodeSandbox`; the script called a real
nested `double` tool, returned `42`, and the journaled details recorded the nested call as `ok`. A
second turn read `store("answer")` and returned `42` again. A failing script returned error text with
the error and the already-made nested call. Tool search offered only `alpha` and `tool_search` before
the query, loaded `github_get_issue`, then offered and executed it; a no-match query returned
`No matching tools found.` and left `modelTools` unset. JSONL wrote `main.jsonl` plus five document
sidecars and reopened the same transcript.

## Capability ledger

Machine ledger: `docs/research/pi-v1.3.0/1056-durable-harness-workflows.json`.
It follows the `pike/pi-v1.3.0-audit-ledger/1` lane shape used by #1054/#1055 and keeps membership,
implementation and verification as separate values.

- Rows: **70** — 66 supported, 4 excluded.
- Implementation: **51 partial**, **15 missing**, **4 not-applicable**.
- Verification: **47 upstream-captured**, **19 unverified**, **4 not-applicable**, **0 verified**.
- Scenario references: **64**; every referenced name exists in the artifact manifest.
- No supported row is marked `verified`: this lane captured frozen observations only. C++ comparison
  against the same scenario is still required.

Coverage families:

| Family | Representative rows | Captured evidence |
| --- | --- | --- |
| Harness lifecycle/configuration | `harness-open-close`, `harness-settings-defaults`, `harness-documents`, `commit-subscription`, `memory-storage`, `harness-run-control` | open/root/close, five documents, commit publication, in-memory storage, run control |
| Conversations/ownership | `conversation-ownership`, `conversation-fork`, `reset-handoff`, `conversation-abort`, `conversation-reopen`, `conversation-entries-config` | all six conversation scenarios |
| Generation/context/retry/prompt | `generation-admission`, `retry-policy`, `prompt-context`, `steering-followup` | public workflow, retry, prompt sections |
| Compaction | `manual-compaction`, `threshold-compaction`, `background-compaction`, `compaction-decline`, `overflow-compaction-retry`, `compaction-no-cut` | four compaction scenarios; background/overflow remain gaps |
| Registries/extensions/hooks | `registry-transition`, `registry-task-definitions`, `extension-sections`, `generation-onYield`, `tool-hooks`, `native-run-triggering`, `task-hooks`, `task-versioning` | live transition, task graph, onYield, tool hooks |
| Views/events/task graph/inspection | `inspect-task-states`, `inspect-submissions`, `task-graph`, `conversation-view-delta`, `event-stream`, `context-at-entry` | all six observation scenarios |
| Environment/tools/images | `node-filesystem`, `node-exec`, `builtin-tools`, `builtin-tool-limits`, `node-env-watch`, `image-processor`, `image-orientation-conversion`, `read-image`, `image-capability-absent`, `tool-execution-timing`, `tool-task-cancellation` | all six environment scenarios |
| Codemode/tool search | `codemode-script`, `codemode-store`, `codemode-failure`, `codemode-replay`, `codemode-remote`, `codemode-models`, `codemode-limits`, `codemode-output`, `toolsearch-load`, `toolsearch-no-match`, `toolsearch-persistence` | four code mode/tool-search scenarios |
| Durable dependencies/storage | `chord-documents`, `chord-delta`, `sqlite-storage`, `jsonl-storage`, `task-recovery`, `task-outcomes` | SQLite/JSONL reopen, task graph |
| Explicit boundaries | `cloudflare-excluded`, `ssh-daemon-excluded`, `ai-image-provider-excluded`, `thirdparty-plugins-excluded` | Not applicable |

## C++ mapping and ownership conclusions

The configured graph has no `cch_durable`; the target is absent by construction. The useful current
pieces are:

- `cch_agent_core`: `src/agent/include/cch/agent/harness/session/SessionStore.hpp`,
  `SessionTree.hpp`, `SessionResume.hpp`, `Compaction.hpp`; `src/agent/harness/session/*`;
  `src/agent/harness/TaskScheduler.*`, `TaskStore.*`; `harness/FileSystem.hpp`, `Shell.hpp`,
  `LocalFileSystem.hpp`, `LocalShell.hpp`; `tools/ToolFactories.hpp` and
  `src/agent/tools/AsyncToolFactories.cpp`. These are application husks and old durable pieces, not
  the v1.3.0 public Harness.
- `cch_coding_agent`: `src/coding_agent/AgentSession.*`, `AgentSessionCompaction.cpp`,
  `AgentSessionExecution.cpp`, `runtime/SessionFactory.*`, `runtime/SessionFork.*`; application
  codemode and tool search under `extensions/`; `ImageInput.*`. These implement the ordinary
  application Session and must not become the durable Harness by relabeling.

Target mapping and edge rules:

1. Move the durable-specific session/document/journal/task machinery to `cch_durable`. Do not make
   `cch_durable` depend on `cch_agent_core`: upstream pi-durable does not depend on pi-agent-core, and
   a durable-to-agent-core edge would repeat the old placement mistake. Agent-core keeps the Agent
   loop/tool execution only.
2. `cch_chord` is a direct durable-required dependency for Context/document/delta values. Keep it
   narrow; the upstream Chord plugin/RPC product is not a target.
3. `cch_codemode` is a direct dependency of `cch_durable`'s codemode integration. The current
   application codemode sandbox/routing (`src/coding_agent/extensions/codemode/`) is not reusable as
   the durable task-scoped integration and must not be reached through `cch_coding_agent`.
4. `cch_ai` contributes only passive values and the retained Magpie inference contract. Classifier
   and image-generation provider execution are excluded; durable images and codemode `models.*` must
   expose capability absence rather than invent a provider path.
5. The ordinary `pike --resume` Session remains on its current application storage. Implementing
   `cch_durable` must not switch the CLI Session onto durable storage. Durable needs a real
   independent caller/example, not a test-only component.
6. New durable JSONL/SQLite must be the pi-durable main/sidecar/document/task format. The current
   `JsonlSessionStore` is the coding-agent transcript format and the current `SqliteSessionStore` is
   the application session/task schema; neither is bidirectional durable-format evidence.

## Unverified selected capabilities (honest gaps)

These rows are supported by membership but have no public scenario in this bundle. They are not
Deferred and not exclusions:

- `harness-settings-defaults` — the complete default/override matrix and retention split.
- `steering-followup` — durable queue admission/drain/capacity behavior.
- `background-compaction` — non-blocking background placement, idle waits and Esc.
- `overflow-compaction-retry` — durable overflow compact-and-retry-once/reopen.
- `native-run-triggering` — native registration/tool wrapping/run triggering/busy-state hooks.
- `submission-queues` — durable submission/inbox/queue modes (A10 evidence covers the domain but is
  not reused as this lane's public capture).
- `codemode-replay` — deterministic start/journal replay after restart, divergence/fingerprint checks.
- `codemode-remote` — generic remote exchange and working Linux host.
- `codemode-models` — capability-gated `models.*` under the retained Magpie contract.
- `toolsearch-persistence` — loaded tools across forks/reset and durable `modelTools` persistence.
- `task-versioning`, `task-outcomes`, `task-hooks` — typed migration/versioning, structured terminal
  outcomes/completing holds, and typed task hooks.
- `tool-task-cancellation` — durable ToolTask cancellation; top-level duration is only
  upstream-captured by the public workflow, not C++-verified.
- `builtin-tool-limits` — durable truncation/spill/diagnostic result contract.
- `node-env-watch` — ExecutionEnv file watching.
- `image-orientation-conversion` — durable EXIF orientation and format conversion.
- `codemode-limits`, `codemode-output` — durable timeout/interrupt/output-limit and output-file/image
  sidecars.

Further implementation gaps visible in captured rows but with upstream evidence only: the public
Harness/document/commit/registry/view/task-graph/inspect APIs are wholly absent; manual/threshold
compaction is application-level; image and codemode/tool-search are application integrations. These
are `partial` or `missing`, not `verified`.

## Bounded vertical recommendations

1. **Foundation slice (cch_durable + cch_chord):** public Harness open/root/close, Storage interface,
   document descriptor/scope/history/fork, built-in documents and commit publication. Add an
   independently callable example and compare the root-creation and commit scenarios.
2. **Admission/generation slice:** submissions/inbox, generation task with retry/overflow checkpoints,
   tool task nested execution, abort and caller-visible settlement. Compare the public workflow,
   retry, abort and inspect-submission scenarios.
3. **Context/compaction slice:** durable context entries, manual/threshold/background compaction,
   hooks, reset/handoff, forks and context-at-entry. Compare all compaction/conversation scenarios.
4. **Observation slice:** registry snapshots and transitions, inspect, task graph, view deltas and
   event stream. Compare the six observation scenarios and the live registry transition.
5. **Linux integrations slice:** local ExecutionEnv, built-in tools, ImageProcessor/read image,
   codemode through cch_codemode (including replay/remote) and tool search/modelTools. Compare the
   environment, codemode/tool-search and storage scenarios.
6. **Storage continuation:** consume the A10 JSONL/SQLite contract and prove bidirectional frozen-pi
   opening/continued operation, alternating writers and reopen/recovery. Do not ship application
   store aliases as durable storage.

## Validation and boundaries

Validation run for this ticket:

```text
python3 scripts/evidence/audit_evidence.py check --manifest fixtures/pi-v1.3.0/audit-1056/manifest.json
# PASS: manifest 97b9f7e760fa4183d4c8a2038476b328699911f21cce89e5286199adff7457a2, 7 artifacts, 32 scenarios

git diff --check
# PASS

git status --short
# only ticket-owned evidence/report/ledger files; shared registry/verifier/tests/README untouched
```

No C++ production build or parity execution was run: this ticket is evidence and audit only, and the
required C++ comparison does not yet exist. The shared verifier `check` accepted the candidate but
the family is not registered; the exact registry record and manifest digest are in the handoff for
the serial merger. No `upstream-captured` row claims C++ parity, and no historical v1.0.4 evidence is
used as a v1.3.0 observation.

## Explicit boundaries

- Cloudflare-specific adapters, the standalone pi-env SSH daemon, classifier/image-generation
  provider execution and direct third-party TS/JS plugin execution remain excluded by ADR 0075.
- The Magpie discovery/catalog/authentication/inference contract and XDG Pike state root are
  preserved; this audit does not restore provider registries, `models.json` execution or AI baseline
  changes.
- Ordinary application Sessions remain separate from durable storage; no installed SDK/ABI is
  promised.
- Historical evidence and other lanes' bundles are untouched. The shared registry, verifier, shared
  tests and README were not edited.
