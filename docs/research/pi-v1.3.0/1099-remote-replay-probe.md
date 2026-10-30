# #1099 deterministic Remote failure probe

## Scope

A local probe of the frozen pi v1.3.0 Remote public caller at revision
`71023aec1546d6d677a65669c5665c0927ce0dd0`. This is upstream evidence only;
it does not establish C++ implementation or parity.

## Observation

On 2026-10-30 UTC, Linux x86-64 glibc, Node v26.13.0, tsx 4.22.1 and
quickjs-wasi 3.6.2, the clean frozen checkout ran
[capture-1099-remote.mts](capture-1099-remote.mts). The script starts a real
`RemoteCodemodeSandbox` backed by `serveCodemodeRemote`, waits for two calls
and idle `(0, 2)`, then submits an unknown call and result 2 while result 1 is
missing. A duplicate result number is subsequently attempted. After 50 ms the
caller aborts the pending run.

- Call 99 is rejected with `Call 99 is not waiting for a result`.
- Reusing result number 2 is rejected with `Result number 2 is not a positive integer used once`.
- With result 1 absent and result 2 accepted, `finished` remains false and the
  only idle is `(0, 2)`. The observation does not prove an infinite wait.
- Aborting with `missing result` yields an aborted outcome with that message;
  call statuses are `cancelled`, then `ok` in issue order. Durations are omitted
  because they are wall-clock measurements.

This separates an unanswered run from successful replay: the upstream run does
not autonomously signal an absent result within the observed interval. A replay
coordinator that knows its input journal is complete must detect the missing
result itself, or explicitly cancel; it cannot infer success from ordinary run
completion.

## Reproduction

Use a clean checkout at the named revision with `quickjs-wasi@3.6.2` and
`tsx@4.22.1` installed, then run:

```sh
PI_CHECKOUT=<checkout> NODE_OPTIONS=--conditions=source \
TSX_TSCONFIG_PATH=<checkout>/tsconfig.json \
<checkout>/node_modules/.bin/tsx docs/research/pi-v1.3.0/capture-1099-remote.mts
```

The observation is registered as [audit-1099](../../../fixtures/pi-v1.3.0/audit-1099/manifest.json).
It is not a C++ delivery receipt. #1099 remains open and no capability status
is advanced.
