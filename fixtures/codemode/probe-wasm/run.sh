#!/usr/bin/env bash
# run.sh — run the probe suite and print the evidence the ticket asks for.
#
#   ./run.sh [<quickjs.wasm>]
#
# Without an argument it fetches the pi-pinned guest from npm (quickjs-wasi
# 3.6.2, the version packages/codemode depends on) into build/quickjs/.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
build="$here/build"

if [[ $# -ge 1 ]]; then
  quickjs="$1"
else
  quickjs="$build/quickjs/quickjs.wasm"
  if [[ ! -f "$quickjs" ]]; then
    mkdir -p "$build/quickjs"
    version="3.6.2"
    curl -sSL -o "$build/quickjs/quickjs-wasi.tgz" \
      "https://registry.npmjs.org/quickjs-wasi/-/quickjs-wasi-${version}.tgz"
    tar -xzf "$build/quickjs/quickjs-wasi.tgz" -C "$build/quickjs" --strip-components=1 package/quickjs.wasm
  fi
fi

run() {
  echo "### $*"
  stdbuf -o0 -e0 "$@" || echo "  (exit $?)"
  echo
}

if [[ -x "$build/wasmedge_probe" ]]; then
  echo "================= wasmedge_probe ================="
  run "$build/wasmedge_probe" validate "$quickjs"
  run "$build/wasmedge_probe" raw-guest "$build/guest/call_host_guest.wasm"
  run "$build/wasmedge_probe" raw-guest-missing "$build/guest/missing_import_guest.wasm"
  run "$build/wasmedge_probe" raw-guest "$build/guest/network_import_guest.wasm"
  run "$build/wasmedge_probe" quickjs "$quickjs" "$here/scripts/escape_probe.js"
fi

if [[ -x "$build/wamr_probe" ]]; then
  echo "================= wamr_probe ================="
  run "$build/wamr_probe" raw-guest "$build/guest/call_host_guest.wasm"
  run "$build/wamr_probe" raw-guest "$build/guest/missing_import_guest.wasm"
  run "$build/wamr_probe" raw-guest "$build/guest/network_import_guest.wasm"
  run "$build/wamr_probe" quickjs "$quickjs" "$here/scripts/escape_probe.js"
fi
