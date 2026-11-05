# #1192–#1196 — codemode pi-v1.3.0 alignment: tool table, close spelling, worker lifetime, source-only captures, host semantics and the portable entry

## Scope

Advances the reusable `cch_codemode` owner to the frozen pi-v1.3.0 comparison
surface under [ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md)
and [#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044), with
[#1192](https://github.com/lanshengzhi/cpp-coding-harness/issues/1192) (public
tool table and close spelling), [#1193](https://github.com/lanshengzhi/cpp-coding-harness/issues/1193)
(source-only row captures), [#1194](https://github.com/lanshengzhi/cpp-coding-harness/issues/1194)
(portable no-Node entry), [#1195](https://github.com/lanshengzhi/cpp-coding-harness/issues/1195)
(discovery absence and host timezone) and [#1196](https://github.com/lanshengzhi/cpp-coding-harness/issues/1196)
(worker lifetime and actual exit) as tickets, reconciling the bounded gaps of
[#1179](https://github.com/lanshengzhi/cpp-coding-harness/issues/1179). Named
baseline: `pi-v1.3.0` / `71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux
x86-64 with glibc; the local reference checkout is `../pi` at exactly that
revision.

## Delivered behavior

- **Public tool table (#1192).** `CodemodeSandbox` implements pi
  `CodemodeSandboxBase`'s table: constructor tools are registered before the
  sandbox is usable (a duplicate constructor tool is refused), `register_tool`
  refuses a duplicate with the frozen message `Tool "a-b" is already
  registered`, `unregister_tool` returns the frozen `true`/`false`, and the
  listing preserves registration order. `run` snapshots the registered table
  at run start with the per-run descriptors overlaying it by name, exactly as
  pi's `execute()` copies the table. The close contract takes pi's two frozen
  spellings: a later `run` or `start` after close is refused with `Sandbox is
  closed` (local and `RemoteCodemodeSandbox`), while the in-flight abort of a
  running execution keeps `Sandbox closed` (audit-1053 `worker-lifetime.json`
  close-settles observation).
- **Worker lifetime and actual exit (#1196).** Deterministic `start` enters
  through the same `CodemodeWorkerHost` admission seam as `execute`:
  admission is acquired after option validation, `start_worker` runs the VM
  driver, and the exit callback releases the admission at the worker's actual
  exit. A pre-aborted start settles during construction and takes no
  admission; denied admission or a failing lifetime hook settles the run with
  the frozen sandbox error (`lifetime hook failed` in the comparison) and the
  admission is released exactly once, the strict no-exception seam for pi's
  throwing `onWorkerStarted` (the host models the termination it requests for
  its already-started worker). The deterministic driver owns no thread; the
  sandbox and the remote serve each start their worker through their own
  seam and keep the driver alive by value until `drive` returns. A timed-out
  run settles before the worker's exit releases its admission.
- **Source-only row captures (#1193).** Two independent frozen families were
  captured from the frozen checkout and validated with
  `scripts/evidence/audit_evidence.py check`:
  - `audit-1193` (three artifacts): the generic Remote
    request/exchange/reply contract — the request's code, tool table with
    script names, globals, store snapshot and absent interrupt budget; the
    exchange's caller-side tool call and terminal `done`; the writes set
    empty on success without `store()` and carrying the real write set after
    it; the three distinct transport errors (`Remote sandbox failed: object
    reset`, `Remote sandbox returned before the script settled`, `Failed to
    reach the remote sandbox: no binding`); the guest-module create/load
    boundary (pi constructs with a missing guest and fails the first run with
    `Failed to load QuickJS: wasm unavailable`, never touching the guest when
    nothing executes); and the application output-token budget (head/tail
    kept, `Warning: truncated output` header, `[Full output: <path> (read
    with offset/limit)]`, `details.fullOutputPath`, the spill file holding
    the full text, the zero budget truncating over the error text, and the
    80-token option consumed rather than ignored).
  - `audit-1195` (two artifacts): the discovery-absence observation (an
    absent `describeTool`/`describeNamespace` name reads as JS `undefined`
    through the real application globals) and the host-clock observation
    (non-deterministic `execute()` reads the host timezone offset; deterministic
    `start()` pins UTC).
- **Discovery absence and host timezone (#1195).** The run-path bridge
  preserves pi's `undefined`: a tool or global whose value is `undefined`
  crosses as the empty payload, which the settle maps to the guest's
  undefined value (the deterministic path already used this spelling; an
  empty payload is never valid JSON text, so no value collides). The
  application discovery handler returns the empty payload for absent names.
  The worker-mode guest's `host_get_timezone_offset` reports the real host
  offset in seconds east (glibc `tm_gmtoff` at the given time); the
  deterministic VM keeps its own import pinned to UTC.
- **Portable no-Node entry (#1194).** `cch_codemode_portable` is a public
  target exporting the source options prefix/grammar, the prelude and
  declaration constants (`Prelude.hpp` moved to the public include root with
  identical bytes; `Types.hpp` and `Portable.hpp` carry the passive value
  types and the Remote protocol types) with no Boost.Asio, WasmEdge or thread
  dependency. `cmake/targets/Codemode.cmake` fail-closes the declared
  dependency graph against native-only tokens at configure time, and the
  check shard compiles native-header guards, so a portable subset that
  secretly imports or links a native runtime fails the build-level check, not
  just a source-only assertion. The application bridge stays in
  `cch_coding_agent`; the worker-mode engine stays in `cch_codemode`.

### Recorded product divergences (intentional, owner-visible)

- **No separate `InlineCodemodeSandbox` class (#1192/#1194).** pi's Inline
  entry runs the VM on the calling thread because QuickJS yields to the host
  event loop between slices. The C++ engine is a synchronous blocking call:
  a same-thread inline runner would deadlock on any script that awaits a
  tool, rather than reproduce pi's behavior. The frozen inline behavioral
  semantics (interrupt budget, deadline during tool waits, abort during tool
  waits, success shape) are delivered and verified on `CodemodeSandbox`
  through `CodemodeLimits::interrupt_budget` (rows `codemode-inline-success`,
  `codemode-inline-interrupt-budget`, `codemode-inline-deadline-abort`,
  verified under #1179); the tool-table/close contract is compared through
  the public `CodemodeSandbox` caller. The portable entry therefore exports
  the constants and protocol types, not an Inline class (#1194 makes that
  export conditional: "Depends on #1192 for the shared Inline/close surface
  **if** the portable entry exports Inline").
- **Guest-module validation timing (#1193).** pi validates the guest lazily
  at the first run; the C++ `CodemodeSandbox::create` validates eagerly, so
  a missing module is an explicit create error before any run. The
  Application `CodemodeToolSource` holder keeps pi's laziness — a session
  that never calls codemode never touches the guest — and the first script
  surfaces the create error as the call's explicit failure. The frozen
  first-run message is recorded in `audit-1193/sandbox-create.json`.
- **Native worker host, not a Node child (#1196).** The frozen Node-child
  scenario's process-exit timing premise (a real Node child exiting within a
  second of its result) has no library equivalent: the C++ host is a native
  pthread worker. The compared shapes — result before worker exit, abort,
  timeout and close releasing admission only at actual exit — are delivered
  and compared; the Node-process premise is recorded here.
- **Pi's `close()` does not abort in-flight deterministic runs in C++**
  (destruction of the run handle interrupts them). Frozen pi aborts `start()`
  runs from `close()`; the C++ `close()` covers `execute()`-path runs. The
  durable runtime tears down through handle destruction, so teardown is
  unaffected; the gap is recorded for the owner.

## Frozen evidence

| Family | Artifact | Scenarios | Notes |
|---|---|---:|---|
| `audit-1193` | `remote-generic.json` | 3 | generic Remote contract |
| `audit-1193` | `sandbox-create.json` | 3 | guest create/load boundary |
| `audit-1193` | `output-budget.json` | 3 | application output-token budget |
| `audit-1195` | `discovery-absence.json` | 1 | undefined spelling through a script |
| `audit-1195` | `clock-timezone.json` | 1 | host offset vs pinned UTC |

Both families were captured from the frozen checkout at `71023aec…` (verified
HEAD, clean tracked sources), Linux x86-64 / glibc / Node v26.13.0 / tsx
4.22.1, and validated with `scripts/evidence/audit_evidence.py check`.
Registration in `fixtures/pi-v1.3.0/baselines.json` remains with the serial
merger, as does the whole-row claim for the six rows verified against these
families (`codemode-remote`, `codemode-sandbox-create-errors`,
`codemode-source-output-token-option`, `codemode-script-discovery`,
`codemode-script-clock-timezone`, and the `codemode-inline-close` /
`codemode-worker-lifetime` divergence ruling).

## Validation

On testedRevision `2fd077c4dcf3` (implementation commit `7f27390d05fe` + test
commit `2fd077c4dcf3`), the default strict Debug preset:

- Focused selections: `[issue1192]` 2/2, `[issue1193]` 6/6, `[issue1194]`
  1/1, `[issue1195]` 2/2, `[issue1196]` 3/3, all pass, with the JUnit
  artifacts pinned in [1058-deliveries.json](1058-deliveries.json)
  (`deliveries/1192-codemode.xml` 5 cases including the two pre-existing
  execute-outcome comparisons, `1193`–`1196-codemode.xml`).
- Owning label: `ctest --preset vcpkg -LE architecture -L codemode` and the
  coding_agent codemode selections pass (176 cases across the touched
  labels); the architecture selection 43/43.
- Full Validation: `cmake --build --preset vcpkg` then `ctest --preset
  vcpkg` (recorded below under Gate).
- Receipt revalidation: the 1118, 1177, 1179 and 1090 records pin inputs this
  lane touched (and 1181/1182/1184/1176 pins went stale through the
  committed 2953743b5 registry registration and 75f602ad5 ADR 0076 doc); all
  selections were rebuilt and rerun at the new testedRevision into
  `deliveries/1192-revalidation-*.xml`, re-pinned under `revalidation1192`.
  1179: 52/52, 1118: 19/19, 1177: 9/9, 1090: 36/36, 1181: 16/16,
  1184: 10/10, 1186: 89/89, 1182: 13/13, 1176: 5/5.

## Capability status

Three rows are claimed with whole-row receipts (empty remaining boundary):
`cch_codemode:codemode-execute-outcome` and `cch_codemode:codemode-worker-close`
(1192-codemode-tool-table-close) and `cch_codemode:codemode-portable-entry`
(1194-codemode-portable-entry). The other seven rows keep their status:
`codemode-inline-close` and `codemode-worker-lifetime` await the owner's
acceptance of the recorded product divergences above; the five rows verified
against the unregistered audit-1193/audit-1195 families await the serial
merger's registration before their whole-row receipts can bind registered
artifacts.

## Remaining boundary

- Serial-merger registration of `audit-1193`/`audit-1195` and the whole-row
  receipts for the five capture-verified rows.
- Owner ruling on the recorded divergences: no `InlineCodemodeSandbox` class,
  the Node-child process premise, and `close()` not aborting in-flight
  deterministic runs.
- The empty-catalog/undefined discovery spelling and the host-clock semantics
  are compared through audit-1195; the two rows' ledger promotion waits on
  the registration above.
