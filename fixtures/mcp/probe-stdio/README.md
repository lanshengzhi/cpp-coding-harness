# MCP stdio transport roundtrip probe (#866)

Measurement harness for spec #865. It answers one question: can Pike's stdio
seam drive a real MCP server roundtrip with pi v1.0.4 framing, and what does
the transport layer cost? This is **not** product code and is **not** wired
into CMake or the CTest suite.

## Contents

| File | Role |
| --- | --- |
| `echo_server.py` | Minimal newline-delimited JSON-RPC 2.0 MCP echo server (fixture). Faults via `debug/*` methods. |
| `probe_stdio.cpp` | Probe using raw `pipe2`/`fork`/`execvp` + `boost::asio::posix::stream_descriptor`, mirroring `src/agent/harness/Process.cpp`. Full roundtrip + three fault cases. |
| `probe_boost_process.cpp` | Probe using `boost::process::v2::process` + `boost::asio::readable_pipe`/`writable_pipe`. Initialize + tools/call. |
| `build.sh` | Compiles both probes directly with the pinned toolchain (no CMake). |
| `run.sh` | Builds if needed and runs both probes. |

## Prerequisites

- A configured build tree exposing the pinned vcpkg deps. Configure once with
  `VCPKG_ROOT=<primary checkout>/.deps/vcpkg cmake --preset vcpkg`; the tree
  appears at `<build>/vcpkg_installed/x64-linux`.
- `python3` on `PATH` (the fixture server).
- GCC 16 (`g++`) or override with `CXX=`.

## Re-run

From this directory:

```sh
# A worktree with its own build/:
./run.sh

# A linked worktree that shares the primary checkout's build tree:
VCPKG_INSTALLED_DIR=<primary>/build/vcpkg_installed/x64-linux/include \
VCPKG_LIB_DIR=<primary>/build/vcpkg_installed/x64-linux/lib \
  ./run.sh
```

`run.sh` builds both probes if missing, then runs each under a 60s hard
timeout. Exit status is 0 only when every case passes.

Environment knobs: `BUILD_DIR`, `VCPKG_INSTALLED_DIR`, `VCPKG_LIB_DIR`, `CXX`.
Set `PROBE_TRACE=1` to print every wire frame the raw probe exchanges.

## Observed result (GCC 16.2.1, Debug/-O0)

```
[probe] PASS initialize: responded=1 protocolVersion=2025-06-18 serverInfo.name=probe-stdio-echo
[probe] PASS tools/list: responded=1 first tool=echo
[probe] PASS tools/call: responded=1 echoed text=hello
[probe] PASS malformed-frame: recovered=1 transport-error="malformed JSON frame: this is not json"
[probe] PASS invalid-jsonrpc: recovered=1 transport-error="invalid JSON-RPC message: {"jsonrpc":"2.0"}"
[probe] PASS crash-mid-request: closed=1 pending-request-failed=true child-exit=3
[probe] RESULT: PASS (failures=0)

[probe-bp] PASS initialize: response id=1
[probe-bp] PASS tools/call: response id=2
[probe-bp] RESULT: PASS (failures=0)
```

Full observations and the integration recommendation live in the shared notes
report `pike-notes/mcp-codemode-865/probe-866-stdio.md`.
