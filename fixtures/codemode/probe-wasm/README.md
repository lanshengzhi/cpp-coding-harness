# probe-wasm — codemode wasm runtime feasibility (issue #868)

Standalone measurement harness for the spec #865 codemode slice. It answers one
question before any codemode product code is written: **can a wasm runtime
embedded from the Pike C++23 / no-exceptions toolchain run pi's codemode guest,
and does the sandbox fail closed?**

This directory is deliberately **not** wired into CMake or the test suite. The
report with candidates, evidence, and the recommendation lives outside the repo
at `pike-notes/mcp-codemode-865/probe-868-wasm.md`.

```
guest/      freestanding wasm32 guests (clang --target=wasm32 -nostdlib)
host/       two probe hosts driving a runtime's C API
scripts/    the JS the real pi guest evaluates (escape_probe.js)
build.sh    compile guests + hosts (runtime libraries must be built first)
run.sh      run the suite and print the evidence
```

## What pi's codemode guest actually needs

pi v1.0.4 (`7c10bd43`) runs JavaScript in QuickJS compiled to wasm
(`quickjs-wasi` 3.6.2, `packages/codemode/src/wasm.ts`). Its host
(`runtime/worker.ts`) instantiates `quickjs.wasm`, supplies a WASI shim and a
`bridge` host function, and drives the exported `qjs_*` entry points. The
boundary is therefore exactly the guest's import list, measured with
`WebAssembly.Module.imports`:

- `env`: `host_call`, `host_interrupt`, `host_promise_rejection`,
  `host_module_normalize`, `host_module_load`, `host_get_timezone_offset`
- `wasi_snapshot_preview1`: `clock_time_get`, `fd_write`, `fd_close`,
  `fd_fdstat_get`, `fd_seek`, `random_get`

There is **no** filesystem, socket, process, or module import. A script can only
reach the host through `host_call`, which is what the codemode prelude wires to
`tools.*`, `text()`, `image()`, `store()`, and `console.*`. The probe measures
that the runtime can drive this guest and that the boundary fails closed.

## Candidate inventory (pinned vcpkg baseline `2f1d605400`)

| Runtime | In `.deps/vcpkg` baseline? | Version | License | Notes |
| --- | --- | --- | --- | --- |
| WasmEdge | yes (`ports/wasmedge`) | 0.13.5#2 | Apache-2.0 | heavy deps (boost-algorithm/align/predef, spdlog); port **fails to build on GCC 16** as-is |
| wasmtime | **no** | — | Apache-2.0 WITH LLVM-exception | would need a new port/overlay (Rust/cargo build) |
| wasm-micro-runtime (WAMR) | **no** | — | Apache-2.0 | would need a new port/overlay |
| v8 (port exists) | port only, `supports: !(... linux)` | 9.1.269.39 | BSD-3-Clause | not supported on linux |
| quickjs-ng / duktape | yes | 0.16.1 / 2.7.0 | MIT | native JS engines, not wasm runtimes |

Only WasmEdge is resolvable from the pinned baseline. `wasmtime` and WAMR were
built from upstream source to get a second data point.

## Building the runtimes

### WasmEdge (from the pinned port, with a GCC 16 workaround)

`vcpkg install wasmedge` against baseline `2f1d605400` **fails on GCC 16**: the
port compiles with `-Wall -Wextra -Werror` and GCC 16 raises
`-Werror=sfinae-incomplete` (`include/host/wasi/inode.h`) and
`-Werror=maybe-uninitialized` (`lib/loader/filemgr.cpp`). Build the downloaded
source directly with `-Werror` demoted (`cmake/Helper.cmake` appends
`-Wno-error` after `-Werror`):

```bash
export VCPKG_ROOT=<repo>/.deps/vcpkg
mkdir -p /tmp/wasmedge-mf && cat >/tmp/wasmedge-mf/vcpkg.json <<'JSON'
{ "name":"wasmedge-probe","version-string":"0","builtin-baseline":"2f1d605400c8727cc00c15797aba796c88ccd523",
  "dependencies":[{"name":"wasmedge","default-features":false}] }
JSON
# fetch spdlog/fmt + WasmEdge sources without touching the repo manifest:
$VCPKG_ROOT/vcpkg install --triplet x64-linux --x-install-root=/tmp/wasmedge-mf/installed \
  --x-buildtrees-root=/tmp/wasmedge-mf/buildtrees --x-packages-root=/tmp/wasmedge-mf/packages \
  --downloads-root=/tmp/wasmedge-mf/downloads     # (fails on wasmedge; leaves the source + spdlog/fmt)
# then patch Helper.cmake in the extracted source (-Wno-error) and:
cmake -S /tmp/wasmedge-mf/buildtrees/wasmedge/src/*.clean -B /tmp/wasmedge-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/tmp/wasmedge-mf/installed/x64-linux \
  -DWASMEDGE_BUILD_AOT_RUNTIME=OFF -DWASMEDGE_BUILD_TOOLS=OFF -DWASMEDGE_BUILD_TESTS=OFF \
  -DWASMEDGE_BUILD_PLUGINS=OFF -DWASMEDGE_BUILD_STATIC_LIB=ON -DWASMEDGE_BUILD_SHARED_LIB=OFF \
  -DCMAKE_INSTALL_PREFIX=/tmp/wasmedge-install
cmake --build /tmp/wasmedge-build -j4 && cmake --install /tmp/wasmedge-build
```

### WAMR (from upstream source)

```bash
git clone --depth 1 --branch WAMR-2.4.5 https://github.com/bytecodealliance/wasm-micro-runtime.git /tmp/wamr-src
cmake -S /tmp/wamr-src/product-mini/platforms/linux -B /tmp/wamr-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DWAMR_BUILD_INTERP=1 -DWAMR_BUILD_FAST_INTERP=1 \
  -DWAMR_BUILD_AOT=0 -DWAMR_BUILD_JIT=0 -DWAMR_BUILD_LIBC_WASI=0 \
  -DWAMR_BUILD_LIBC_BUILTIN=0 -DWAMR_BUILD_TESTS=0 -DWAMR_BUILD_SIMD=0
cmake --build /tmp/wamr-build -j4
```

`WAMR_BUILD_LIBC_WASI=0` is required for a fair comparison: WAMR's default
libc-wasi links a real `sock_open`, which is a capability the codemode contract
must not expose.

## Building and running the probe

```bash
export WASMEDGE_PREFIX=/tmp/wasmedge-install
export SPDLOG_PREFIX=/tmp/wasmedge-mf/installed/x64-linux
export WAMR_SRC=/tmp/wamr-src
export WAMR_LIB=/tmp/wamr-build/libiwasm.a
./build.sh
./run.sh                      # fetches quickjs-wasi 3.6.2 into build/quickjs/
# or: ./run.sh /path/to/quickjs.wasm
```

Guest compilation needs clang with the wasm32 target (clang 23 works).

## Expected results

- **`validate`** — WasmEdge accepts pi's `quickjs.wasm` (load + validate ok).
- **`raw-guest call_host_guest`** — `run() -> 42` and the host `bridge` callback
  fires on both runtimes: a host function is reachable across the boundary.
- **`raw-guest missing_import_guest` / `network_import_guest`** — fail closed on
  both, but at different points: WasmEdge refuses **instantiation** ("unknown
  import"); WAMR instantiates and traps **on call**
  ("failed to call unlinked import function").
- **`quickjs escape_probe.js`** — WasmEdge runs the real pi guest end to end:
  `qjs_init() -> 0`, the script calls the host `output` callback through
  `host_call`, and `require`/`process`/`fetch`/`WebAssembly` are all `undefined`,
  so the filesystem and network escapes are refused. WAMR 2.4.5 **SIGSEGVs**
  while running `_initialize`/`qjs_init` on `quickjs.wasm`; see the report.
