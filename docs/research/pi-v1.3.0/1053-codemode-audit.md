# #1053 Codemode declarations, execution, replay and Remote audit

**Status:** audit complete. The frozen behavior is captured as `upstream-captured`; no row is
`verified`, because this ticket did not run a C++ parity suite and did not change production code.
The C++ side is an audit of the current seam, not a claim that it implements the captured behavior.
This ticket did not extract `cch_codemode`, did not add `start()`/Inline/Remote execution, and did not
close any capability gap.

- **Baseline:** `pi-v1.3.0` at `71023aec1546d6d677a65669c5665c0927ce0dd0`
- **Capture host:** Linux x86-64, glibc 2.44, Node v26.12.0, tsx 4.22.1
- **Private frozen checkout:** `/tmp/cch-1053-pi-frozen`; tracked tree clean and `HEAD` verified as
  the full baseline SHA before the capture wrote anything
- **Integration base:** `e4ff4763a` (`integration/1044-pi-v1.3.0`, including the landed
  #1045–#1055 audit bundles; the ticket work was originally cut from `e8cf93dd1`)
- **Ticket branch:** `ticket/1053-audit`
- **Machine ledger:** [1053-codemode-audit-ledger.json](1053-codemode-audit-ledger.json) — 68
  capability rows, two exclusions, implementation/verification states, consumer chains and exact
  C++/upstream seams
- **Evidence family:** `fixtures/pi-v1.3.0/audit-1053/` — eight artifacts, 82 scenarios
- **Capture source:** `fixtures/pi-v1.3.0/audit-1053/capture-1053.mts`,
  `scenarios.mts`, `pi.mts`

The JSON ledger is the authoritative operation/consumer/status inventory. This report explains the
observed contracts, the C++ gap classes and the slices that follow.

## Evidence bundle

| Artifact | Scenarios | SHA-256 |
|---|---:|---|
| `source-options.json` | 7 | `664109294d1f852cee1b142acf8de400d9066861dc10b81e7e210f24e42c8f56` |
| `declarations.json` | 13 | `a29f7a798b15cb4dacce608f4018a5c917162ce16f4ee5a4e37f86f6f34939c5` |
| `script-execution.json` | 18 | `267ba6f2774da433809a783e0656b7d3703f4457906ab8bb54e5c9b1c209f0c9` |
| `worker-lifetime.json` | 8 | `1ca3ecef15ea78d9e4fbec315a8640ba09a50f2f114cd796ce59aee49a426dcf` |
| `deterministic-replay.json` | 14 | `24105ec690559628886d9431c98c11c5598f70f25e738f318b2bb714e031e329` |
| `inline-interrupt.json` | 6 | `b01e9ff3b21d6d47bf7b2009b1cc5a79e503116e8ad5ca5b18d55e827cdb401d` |
| `remote-exchange.json` | 6 | `0292c4f696715a46f87442203a949c65006e60b991cd864351f9d1cf0f57b2ab` |
| `application-catalog.json` | 10 | `68a7e9716755ef5ebba97948b3ad9343b1621c1bdf72517248554d0927f52fb6` |

`manifest.json` SHA-256: `f4c48cc5db9bf4a0e40db6651e5c43e17023d9635a1dd9f85f2b9a472cb793bf`.

The exact capture command (run from the repository root; the capture re-executes itself under the
frozen checkout's `tsx`):

```bash
NODE_OPTIONS=--conditions=source \
PI_CHECKOUT=/tmp/cch-1053-pi-frozen PI_BASELINE=pi-v1.3.0 \
  /tmp/cch-1053-pi-frozen/node_modules/.bin/tsx \
  fixtures/pi-v1.3.0/audit-1053/capture-1053.mts
```

`python3 scripts/evidence/audit_evidence.py check --manifest fixtures/pi-v1.3.0/audit-1053/manifest.json`
accepted the bundle and printed this registry record:

```json
{
  "audit-1053": {
    "manifest": "audit-1053/manifest.json",
    "manifestSha256": "f4c48cc5db9bf4a0e40db6651e5c43e17023d9635a1dd9f85f2b9a472cb793bf"
  }
}
```

Serial registration in `fixtures/pi-v1.3.0/baselines.json` is left to the parent/serial merger. The
lane did not edit the registry, the shared evidence checker, the README or the boundary test. Until
registration lands, no row is `verified`; evidence is `upstream-captured`, source-only
`unverified`, or `not-applicable` for the two exclusions.

### Frozen-checkout resolution topology

The checkout is a private copy at the exact revision, not the shared `../pi` working tree. Its
`node_modules` links to the shared install, while the workspace links point back into the local
frozen `packages/*`; `NODE_OPTIONS=--conditions=source` selects the frozen package `source`
condition. The frozen checkout did not contain `quickjs-wasi`, so `quickjs-wasi@3.6.2` was fetched
with `npm pack` and extracted into `/tmp/cch-1053-pi-frozen/node_modules/quickjs-wasi` (gitignored;
the tracked tree stayed clean). No shared package source, `node_modules` or build output was edited.
The capture writes only under `fixtures/pi-v1.3.0/audit-1053/`.

## 1. Inventory and ownership boundary

The ledger keeps the reusable library contract, the application consumer and the durable/remote
target distinct. This is the boundary the later extraction must preserve.

| Layer | Frozen pi seam | Pike seam | Consumer / boundary |
|---|---|---|---|
| Source and grammar | `packages/codemode/src/source.ts`, `identifier.ts` | `CodemodeSource.{hpp,cpp}`, `to_codemode_identifier` | Model-facing tool and every direct parser caller |
| Declarations | `packages/codemode/src/declarations.ts` | `CodemodeDiscovery.{hpp,cpp}:schema_to_type,render_tool_sample` | Catalog, discovery samples, script declarations |
| VM execution | `runtime/execution.ts`, `host.ts`, `worker.ts`, `vm.ts`, `prelude-source.ts`, `protocol.ts` | `CodemodeSandbox.{hpp,cpp}` over WasmEdge | Application `CodemodeToolSource` |
| Portable/Inline | `runtime/inline.ts`, `portable.ts` | — | Workers/browsers; no native CLI dependency |
| Remote | `runtime/remote.ts`, `protocol.ts` | — | Durable Object/isolate boundary |
| Cloudflare | `cloudflare.ts` | — | Excluded deployment target (ADR 0075); generic Remote remains in scope |
| Application tool | `coding-agent/src/extensions/codemode/{tool,execute,index,renderer,worker}.ts` | `CodemodeToolSource`, `CodemodeTool`, `McpProductionAdapters`, `SettingsManager` | Session prompt/tool registry, MCP refresh, TUI |
| Application store | `codemode-store` session entry | constant only (`CodemodeTool.hpp:24`) | Branch replay/fork and scripts using `store()` |
| Worker capacity | `MAX_CONCURRENT_CODEMODE_WORKERS=2`, `acquireWorker` | — | Application concurrency policy |

The proposed `cch_codemode` owner boundary starts at source/declarations/prelude and the generic
execution interfaces. `codemode-store`, settings, loadout/prompt assembly, MCP exposure and the TUI
are application integrations, not part of the reusable VM contract. Inline and Remote are library
destinations; the Cloudflare Durable Object wrapper is a deployment consumer of Remote and is
**excluded** by ADR 0075, not a missing Pike slice.

The largest structural finding is that Pike has no such library boundary: all codemode code lives
under `src/coding_agent/extensions/codemode`, uses WasmEdge, Boost.Asio, `std::thread` and
coding-agent session types, and cannot be reused by a portable or remote host. That is recorded as a
missing library row, not as a missing test.

## 2. Source and options

The captured parser contract is precise:

- The first line is an options line only when its first non-whitespace token is `// @options:`.
  CRLF, leading whitespace, an empty options object and `max_output_tokens: 0` are accepted.
- The options line is replaced by an empty line, preserving script line numbers.
- A misplaced options line is rejected with its line number
  (`The // @options: line must be the first line of the input; found one on line 2`).
- Empty input, invalid JSON, non-object options, unknown fields, invalid ranges and an options line
  with no following source are typed `CodemodeSourceError`s.
- `timeout_ms` becomes the whole-execution deadline; an absent option means an infinite application
  deadline, not the library's 300 s default. `max_output_tokens` is the application output budget.

Pike parses the basic forms (`CodemodeSource.cpp:91`), including CRLF and the empty-line
replacement, and bounds `timeout_ms`/`max_output_tokens` the same way. It does not detect a misplaced
options line: a later `// @options:` line is accepted as plain JavaScript. Its invalid-JSON message
also includes a parser detail that v1.3.0 does not include. Those are source rows
`codemode-source-placement-errors` (partial) and `codemode-source-parse` (implemented). There is no
dedicated C++ source test, so even the accepted-form behavior is `upstream-captured`, not
`verified` at the C++ seam.

The application option consumer has a larger gap. `max_output_tokens` is parsed into
`CodemodeSourceOptions` and then ignored: `CodemodeToolSource::to_extension_result` has no joining,
truncation, temp-file spill or `fullOutputPath`. The timeout is used, but only the worker deadline;
the application default-infinite rule is correctly mirrored by `limits_for`. The output-token row is
source-only evidence (`codemode-source-output-token-option`): no artifact scenario consumes it, so
its verification is `unverified`.

## 3. Declarations and the model-facing catalog

The captured declaration surface includes:

- identifier normalization (invalid characters to `_`, empty to `_`);
- JSON Schema to TypeScript rendering: primitives, literals, unions, nullable branches, objects,
  arrays/tuples, local refs, `unknown` for recursive/remote refs, sorted properties, quoted keys,
  description comments, integer/default/pattern/deprecated tags, boolean draft-04 bounds, escaped
  line terminators, tag dropping and the 16,000-character input budget;
- `renderToolOutputType`, `renderToolSignature`, `renderToolSample` and `renderDeclarations`;
- a missing output schema defaults to `Promise<string>`;
- the declaration description appends trimmed prompt-guideline bullets;
- `renderDeclarations` groups tools, globals and dotted namespaces with doc comments.

Pike has a solid `schema_to_type` implementation, but `render_tool_sample` hard-codes
`Promise<unknown>` (`CodemodeDiscovery.cpp:522`) and there is no public declaration/signature/
output-type/`render_declarations` function. The nested catalog in `CodemodeTool.cpp:43` is the old
bullet form (`- tools.name({ props }): description`), not a fenced declaration. It has no output
schema, guidelines, namespace grouping, deferred filtering or inline-budget selection. The settings
`codemode.mode` and `codemode.inline_budget` are parsed (`SettingsManager.cpp:1398-1408`) but never
passed to the renderer.

The captured catalog also defines per-tool script-call descriptions and the `on`/`only` loadout
split. Pike has no loadout hook at all: a direct tool is both model-visible and listed in the
codemode catalog, and an `only` project cannot hide direct declarations. That is application row
`codemode-description-loadout` (missing), not a formatting difference.

`models.*` provider execution is excluded: it is the application model-orchestration feature, not a
codemode VM/transport contract in A09. Pike has no model globals and no C++ equivalent. A separate
supported row, `codemode-models-description-gate`, records the capability-absent behavior: the
catalog omits the `models` line when model access is absent. The enabled branch is unsupported, but
this audit does not demand provider execution.

## 4. Script execution, output, store and errors

The frozen script contract covers eight interacting areas:

1. **Tools.** `tools.<jsName>` and `tools["<raw name>"]` are async, route through the host,
   preserve structured results when the tool declares an output schema, reject missing tools and
   failures with catchable `Error`s, and record status/duration.
2. **Globals.** Top-level spread calls and dotted frozen namespaces; constructor validation of
   identifiers, duplicates and namespace conflicts.
3. **Discovery.** `searchTools`, `describeTool` and `describeNamespace` are globals over the
   callable set.
4. **Output.** `text()` JSON-stringifies non-strings, `image()` accepts a data URL,
   `{image_url}` or an image block, console items carry `console: true`, and output stays in
   production order. The returned value is appended like `text()` on success.
5. **Exit.** `exit()` succeeds with prior output and store writes and suppresses later output.
6. **Store.** A JSON snapshot, per-value and total limits, `undefined` deletes, writes reported only
   on success, and application persistence across calls.
7. **Errors.** Script error name/message/stack in `codemode.js:line:col` form; syntax, non-Error
   throws, BigInt serialization, unknown tools, OOM, timeout, abort and sandbox failures have
   distinct shapes; script errors are catchable where applicable.
8. **Limits and cancellation.** Output/store limits, a 256 MiB application heap limit, deadline
   and caller abort, pre-abort before VM creation, and unawaited-call cancellation.

Pike implements a meaningful subset but has three end-to-end holes:

- **Globals are never passed.** `CodemodeSandbox.cpp:786` always passes `no_globals = "[]"` to the
  prelude, and `CodemodeSandbox::run` has no globals argument. `CodemodeDiscovery::globals_json`
  exists but is dead. Scripts therefore cannot call `searchTools`, `describeTool` or
  `describeNamespace`; the discovery handler in `CodemodeToolSource.cpp` is unreachable. The
  model-facing description still advertises these globals.
- **Store is never wired.** The host passes `no_store = "{}"` and `emit_done` ignores the prelude's
  writes argument (`CodemodeSandbox.cpp:321`). `CodemodeRunResult` has no `storeWrites`, and
  `kCodemodeStoreEntryType` is referenced only by the constant and its parity test. A later
  codemode call always starts empty; successful writes are discarded.
- **The prelude is stale.** `CodemodePrelude.hpp:3` says it was generated from pi v1.0.4. Compared
  with the v1.3.0 source, it emits console output as `text` instead of `console`, uses the old
  `raw MCP image block` TypeError wording, and its `settle(id, ok, payload)` has no
  `propertiesJson` parameter, so deterministic rejection properties cannot reach a script `Error`.
  The numeric output/store limits are unchanged. The existing prelude fixture test only proves that
  the C++ literal matches the old committed fixture, so it locks in the drift.

The C++ output item has only `Text`/`Image` and no console discriminant. `CodemodeSandboxTest.cpp`
checks text ordering and PNG detection but cannot distinguish `console.log()` from `text()`.
`CodemodeOutputPresentationTest.cpp` checks the fallback renderer and image sidecar; it does not
check the Application's output joining/truncation, image temp-file labels or nested-call details.

Pike's `CodemodeError` stores only `kind` and `message`, not name/stack. The script error text
usually contains a QuickJS stack, and the existing test checks a message substring, but the captured
structured error shape (including syntax, BigInt, non-Error and OOM cases) is not represented.
Pike also has no heap limit: `CodemodeLimits` has only `timeout`, and WasmEdge is configured without
a memory cap. The 256 MiB application limit and the captured `InternalError: out of memory` path are
absent.

Cancellation is partial. The worker path aborts an in-flight nested call, keeps prior output and
stops a computing script, but a `std::stop_token` carries no reason, so the v1.3.0 caller reason
(`cancelled`, `early`) is replaced by fixed messages. The pre-aborted path also creates the worker
thread before the engine checks the token, so the captured "never creates a VM" guarantee is not
proven.

## 5. Tool routing and nested-call records

The basic routing path is the strongest part of the current implementation. `CodemodeToolSource`
derives both names, builds descriptors, routes calls through `NestedToolCalls`, and
`script_value_from` preserves structured content when the tool declares an output schema. The
routing tests cover a normal call, both name bindings, missing tools, structured results (including
error results with structured content), text-only tools and the tool-name failure fallback.

The application consumer still differs: Pike records a generic `NestedToolCalls` record
(`id/name/status/durationMs/arguments/error`) but does not map it into codemode
`details.calls`; the status vocabulary is `unfinished` rather than `cancelled` for an interrupted
call, and the arguments are not the Application's truncated preview string. No codemode renderer
shows these calls and the failure text has no call summary. The captured `CodemodeNestedCall` shape
and the `Script error:` summary are application rows, not sandbox rows.

## 6. Worker lifetime and admission

The worker-lifetime artifact is the one most likely to be misread as a test-only contract. It is a
real API contract:

- reserve capacity before a worker starts; load wasm before reserving;
- denial rejects before worker creation and leaves `onWorkerStarted` uncalled;
- construction failure returns the reservation exactly once;
- `onWorkerStarted(exited)` exposes actual exit, which can be later than result settlement;
- `close()` settles an in-flight run before the worker exit releases admission;
- repeated aborts settle results while cancelled workers remain unreleased;
- a real Node child can report its result before its worker exits.

The guest-module create-error row is source-only and `unverified`: Pike's `CodemodeSandboxTest`
covers a missing module, but no frozen upstream artifact observes that failure, so the C++ test is
not attached as upstream evidence.

Pike has no admission, no `onWorkerStarted`, no close state and no result-versus-exit separation.
`CodemodeSandbox.cpp:928` creates a `std::thread`, and `:967-968` joins it before returning the
result. The application has no two-worker cap. A burst of codemode calls can create an unbounded
number of WasmEdge threads; a cancelled worker cannot be counted after its result because the result
does not exist until the join. This is a structural host-API gap, not a missing assertion.

## 7. Deterministic replay

The deterministic artifact captures a complete host-delivery API:

- `start(code, {startedAt, seed, tools, globals, timeoutMs, onStarted, onCall, onIdle, signal})`;
- `onStarted(fingerprint)` before the first call, with the VM/version/prelude/stack/heap/budget
  string;
- `deliver(seq, now, id, result)` with a positive seq used once; results apply in seq order even if
  delivered out of order;
- virtual UTC clock, seeded `Math.random`, no `WeakRef`/`FinalizationRegistry`;
- `onIdle(seq, issued)` and `during {seq, issued}`;
- exact rejection messages for a non-finite clock, unknown call, reused seq and unstartable VM;
- a changed seed changes randomness without changing the start time; `startedAt` fixes the clock;
- a deterministic abort is an uncatchable aborted error with `finished` true;
- nonserializable answers leave the call waiting; a later valid answer settles it;
- `start` validates start time, seed and duplicate script/tool names;
- a run-supplied `jsName` overrides the derived name; globals count as issued calls and rejection
  properties reach the script Error;
- `execute()` remains non-deterministic.

Pike has none of this. There is no `start`, `deliver`, query result handle, `onStarted`, `onCall`,
`onIdle`, `during`, fingerprint, seed, virtual clock or deterministic abort. `CodemodeSandbox::run`
answers calls internally and returns one terminal `AsyncResult`. The 14 deterministic scenarios are
therefore all structural missing rows, not failed assertions against a partial implementation.

## 8. Inline interrupt budget

The Inline contract separates two controls that must not be aliased:

- A poll-count interrupt budget (default 100,000) stops computing scripts and accrues across tool
  waits; its message is `Execution exceeded its interrupt budget of <n>`.
- A wall-clock deadline while a tool waits reports `Execution timed out after <n> ms`.
- A caller abort aborts the tool, keeps partial output and reports the caller reason; a
  pre-aborted execution creates no VM.

Pike has no Inline class and no poll-count budget. Every run gets a worker thread and a real-time
deadline. A synchronous runaway on the calling thread cannot occur, but the captured budget
semantics, budget-vs-deadline message selection and no-VM pre-abort are absent.

## 9. Generic Remote

The Remote contract is small and precise:

- `RemoteCodemodeSandbox` sends a JSON-safe request to `remote.execute(request, exchange)`;
- `serveCodemodeRemote` runs the script remotely and sends output/call/started/idle/done messages;
  the caller runs its own tools and returns result/stop replies;
- output is batched (5,000 console lines over six exchanges);
- the remote caps requested interrupt budget and memory;
- caller abort sends `stop`, settles locally without waiting for remote compute, and marks the call
  cancelled;
- failing, silent and unreachable remotes produce distinct `kind: "sandbox"` messages;
- deterministic start over a reordering transport still applies replies in seq order;
- the protocol data types are plain JSON, not class instances;
- `portable` exports the no-Node subset. The upstream `cloudflare` wrapper is excluded by ADR 0075;
  it is not part of the Linux deployment contract.

Pike has no Remote class, protocol type, exchange callback, serve function, batching policy, cap
negotiation or stop reply. Its in-process `SandboxLink` is a condition-variable/C++ struct channel,
not a wire protocol. The native CLI also has no portable/no-Node build target. These are library
misses. The absent Cloudflare wrapper is not a gap because ADR 0075 excludes that deployment.

## 10. Application registration, settings and result consumer

The registration shape is mostly right: one `codemode` tool, `model-only`, `defaultActive: false`,
the source grammar, prompt snippet/guidelines, exclusive concurrency and a lazy guest. The parity
test compares it to the **pi-v1.0.4** bundle, so it is not proof of v1.3.0 description/snippet
parity. A direct test that `load_tools()` does not open the guest is still absent.

The application result consumer is the old fallback:

- header `Script completed|failed` + wall time + `Output:`;
- text/image content in order;
- returned value appended after success;
- `Script error:` + sandbox message on failure.

It does not format adjacent text, a console block, saved-image labels, a kind-specific failure
prefix, a call summary, `details.calls`, `fullOutputPath`, or output truncation. `ToolResultMessage`
does carry a `nested_calls` record, but the codemode result does not use it as `details`, and no
codemode renderer consumes it. The fallback renderer test is real (`CodemodeOutputPresentationTest`)
but only covers the fallback, not the v1.3.0 codemode result details.

The `codemode` settings are parsed correctly (default `on`, nested merge, inline budget) but not
consumed. The MCP late-registration refresh exists but re-renders the old bullet description, so a
late MCP tool appears unbudgeted and ungrouped. The session `codemode-store` entry is entirely
missing; the constant is not a store.

## 11. C++ gap summary

The ledger has 68 rows: 66 supported (two exclusions: `models.*` provider execution and
Cloudflare deployment), 10 `implemented`, 20 `partial`, 36 `missing` and two `not-applicable`. This
table groups the rows by the slice a follow-up should take, not by file.

| Slice | Current C++ state | Captured contract missing |
|---|---|---|
| Source | Basic parser works; no misplaced-line check; invalid-JSON text differs | Line-numbered placement error, exact invalid-options text |
| Declarations | `schema_to_type` exists; sample is `Promise<unknown>`; no public renderer | Output schema, signature/output type, `renderDeclarations`, guideline bullets |
| Catalog/loadout | Old bullet description; settings dead | Namespace grouping, deferred, inline budget, `on`/`only`, per-direct-tool descriptions |
| Sandbox output | Text/image/order works; console flag lost; stale prelude | `console: true`, v1.3.0 image errors, saved-image labels |
| Globals/discovery | Prelude supports them; host always passes `[]`; `describeNamespace` returns null | Reachable `searchTools`/`describeTool`/`describeNamespace`, globals constructor |
| Store | Prelude supports it; host passes `{}`; writes ignored; no session entry | Snapshot/write result, branch persistence, exit writes |
| Errors | Message text only | Structured name/stack and the full taxonomy |
| Limits | Timeout and output/store limits; no heap cap | `memoryLimitBytes`, OOM, application 256 MiB |
| Cancellation | Aborts tool/compute; fixed messages; no pre-abort guarantee | Caller reason, partial-output contract, no-VM pre-abort |
| Worker lifetime | Joins thread before result; no admission/close/hooks | Admission, `onWorkerStarted`, result-vs-exit, close, repeated-cancel accounting |
| Deterministic | None | `start`/`deliver`, virtual clock/seed, fingerprint, seq/idle/during, validation, properties |
| Inline | None | Poll budget, budget-vs-deadline, abort/pre-abort, no-thread destination |
| Remote | In-process `SandboxLink` only | JSON request/exchange/reply protocol, batching, caps, stop, failure taxonomy, deterministic start |
| Library/durable | Embedded in coding_agent | `cch_codemode` library boundary, portable entry |
| Application result | Fallback content only | Formatter, nested-call details, call summary, renderer, output truncation, store entry |

A critical ordering constraint follows: discovery cannot be tested or used until globals are passed;
store cannot work until the host carries a snapshot and writes; deterministic and Remote cannot be
implemented correctly without the result-versus-exit/admission distinction; Inline and Remote should
not be added before the `cch_codemode` library boundary exists. The current code has dead pieces
(`CodemodeDiscovery::globals_json`, `CodemodeDiscovery::handle`, the prelude store) that make the
implementation look more complete than the end-to-end contract.

## 12. Validation and residual limits

- `python3 scripts/evidence/audit_evidence.py check --manifest fixtures/pi-v1.3.0/audit-1053/manifest.json`
  passed and printed the registry record above.
- Earlier captures of the eight artifacts were compared with scenario payloads normalized for the
  envelope `capturedAt`; the scenario payloads were byte-identical. The final recapture corrects the
  application test source endpoint (and therefore `application-catalog.json`'s `sourceEndpoints`),
  and refreshes every envelope timestamp. `manifest.json` itself changes per run because artifact
  SHA-256s include the envelope timestamp; the pinned SHA above is the committed capture.
- The capture asserts the private checkout is at `71023aec...`, has a clean tracked tree, is
  linux-x64/glibc, and has `packages/codemode/src/index.ts`; it refuses to overwrite a registered
  family.
- No C++ parity suite ran. Every implementation row is an assessment of the current seam against
  the captured contract; `upstream-captured` and `unverified` are deliberately not treated as
  `verified`.
- The ticket made no production fix and no shared-infrastructure edit. The baseline registry is
  left for the serial merger; the handoff carries the requested `audit-1053` registry entry.
- `quickjs-wasi` was absent from the frozen checkout. The private copy was populated from the
  published `quickjs-wasi@3.6.2` package; this is a capture-host setup detail, not a source change.

## 13. Recommended slice order

1. **Library boundary and prelude refresh.** Extract source/declarations/prelude/execution
   interfaces into `cch_codemode`; regenerate the prelude from v1.3.0 (console channel, image
   errors, `propertiesJson`) and pin it against the v1.3.0 source.
2. **Result, store and globals seams.** Add `storeWrites`/`calls`/structured errors to the result,
   pass globals and store snapshots into `CodemodeSandbox::run`, and wire
   `CodemodeDiscovery::globals_json`. Then the dead discovery/store code becomes testable.
3. **Declarations and catalog.** Add output-schema-aware declarations and the v1.3.0 catalog/
   loadout, consuming `codemode.mode`/`inlineBudget`, namespace/deferred/direct metadata and prompt
   guidelines.
4. **Worker lifetime.** Add admission, `onWorkerStarted`, `close()` and result-versus-exit
   settlement; rerun the worker-lifetime artifact against the C++ seam.
5. **Deterministic start/deliver.** Build the virtual clock/seed/fingerprint/seq/during machine on
   the separated execution state; port the 14 deterministic scenarios before using it in the
   Application.
6. **Inline and Remote.** Add the portable Inline launcher and the JSON Remote protocol/serve
   function after the library split; add batching, caps, stop and failure taxonomy. Cloudflare
   deployment is excluded by ADR 0075 and is not a follow-up slice.
7. **Application consumer.** Port the output formatter, nested-call details, renderer,
   output-token truncation, image save labels and `codemode-store` branch persistence. Keep the
   current fallback for unregistered tools but give codemode its real details.

Do not implement `models.*` provider execution or the Cloudflare wrapper in this port; both are
excluded. Keep the capability-absent `models` description gate and the generic Linux Remote contract.
Do not close #1053 from this audit; the missing C++ slices are the explicit next tickets.

## Reproduction

```bash
# Evidence acceptance (from the repository root)
python3 scripts/evidence/audit_evidence.py check \
  --manifest fixtures/pi-v1.3.0/audit-1053/manifest.json

# Capture (private checkout; run from the repository root)
NODE_OPTIONS=--conditions=source \
PI_CHECKOUT=/tmp/cch-1053-pi-frozen PI_BASELINE=pi-v1.3.0 \
  /tmp/cch-1053-pi-frozen/node_modules/.bin/tsx \
  fixtures/pi-v1.3.0/audit-1053/capture-1053.mts
```

The frozen checkout must remain at `71023aec1546d6d677a65669c5665c0927ce0dd0` with a clean tracked
tree. `quickjs-wasi@3.6.2` is present only in its gitignored `node_modules`.
