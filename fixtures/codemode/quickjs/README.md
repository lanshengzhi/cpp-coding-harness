# quickjs.wasm — the codemode sandbox guest (issue #874)

The WebAssembly guest the codemode sandbox runs. It is pi v1.0.4's actual
codemode guest, pinned by digest so the sandbox runs the same bytes the probe
(#868) and pi's `packages/codemode` run — not a rebuilt or hand-assembled
module.

## Provenance

| field | value |
|---|---|
| package | `quickjs-wasi` |
| version | `3.6.2` (the version pi v1.0.4 `7c10bd43` depends on — `packages/codemode/package.json`) |
| source | `https://registry.npmjs.org/quickjs-wasi/-/quickjs-wasi-3.6.2.tgz` |
| artifact | `package/quickjs.wasm` inside that tarball |
| tarball sha256 | `f1f4349f19a2d849e33ea0ae9bec2e7062b8839f4eceb17c9051ddbaa2720982` |
| `quickjs.wasm` sha256 | `d4c9375f2b1ca4dc95f72c8aa2982a7a9951ac8011490d79c6582df732b4bbd9` |
| license | MIT (`LICENSE`, copied from the same tarball) |

Reuse the probe's acquisition path verbatim (`fixtures/codemode/probe-wasm/run.sh`):

```bash
curl -sSL -o quickjs-wasi.tgz \
  https://registry.npmjs.org/quickjs-wasi/-/quickjs-wasi-3.6.2.tgz
tar -xzf quickjs-wasi.tgz --strip-components=1 package/quickjs.wasm
sha256sum quickjs.wasm   # d4c9375f…
```

`wasi-shim.js` is copied from the same tarball and kept next to the guest as
the readable reference for the WASI subset the guest imports (the sandbox
registers those imports directly through WasmEdge). `prelude.js` is the runtime
string of pi's codemode prelude (`packages/codemode/src/runtime/prelude-source.ts`
`PRELUDE_SOURCE`, TS placeholders expanded); the sandbox embeds the same bytes
as `src/coding_agent/extensions/codemode/CodemodePrelude.hpp` and a test asserts
they stay equal.

## Imports the guest declares

Measured in the probe with `WebAssembly.Module.imports`; the sandbox registers
exactly these twelve and nothing more:

- `env`: `host_call`, `host_interrupt`, `host_promise_rejection`,
  `host_module_normalize`, `host_module_load`, `host_get_timezone_offset`
- `wasi_snapshot_preview1`: `clock_time_get`, `fd_write`, `fd_close`,
  `fd_fdstat_get`, `fd_seek`, `random_get`

There is no filesystem, socket, process, or module import. A missing import is
refused at instantiation (fail-closed).

## Why a committed fixture, not a download at build time

The guest is a frozen external artifact (pi v1.0.4 is the comparison baseline),
it is small (622 KB), and the sandbox must not depend on network access to run
or to test. The digest above is asserted by `tests/coding_agent/CodemodeSandboxTest.cpp`
(WebAssembly magic + exact byte count; the size is 637405).
