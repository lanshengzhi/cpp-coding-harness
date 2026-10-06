#!/usr/bin/env bash
# build.sh — compile the probe guests and hosts.
#
# The wasm runtimes are not vendored here; build them first (see README.md
# "Building the runtimes") and point the environment at the results:
#
#   WASMEDGE_PREFIX  prefix of the WasmEdge install (include/, lib/libwasmedge.a)
#   SPDLOG_PREFIX    prefix holding libspdlog.a and libfmt.a (a vcpkg install root)
#   WAMR_SRC         WAMR source root (contains core/iwasm/include)
#   WAMR_LIB         path to a built libiwasm.a
#
# Guest compilation needs clang with the wasm32 target (clang-23 works).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
build="$here/build"
mkdir -p "$build/guest"

CC_WASM="${CC_WASM:-clang}"

echo "== building guests (wasm32, freestanding) =="
for guest in call_host_guest missing_import_guest network_import_guest; do
  "$CC_WASM" --target=wasm32 -nostdlib -O1 -Wl,--no-entry -Wl,--export=run \
    -o "$build/guest/$guest.wasm" "$here/guest/$guest.c"
  echo "  built guest/$guest.wasm"
done

if [[ -n "${WASMEDGE_PREFIX:-}" ]]; then
  : "${SPDLOG_PREFIX:?set SPDLOG_PREFIX to a prefix holding libspdlog.a and libfmt.a}"
  echo "== building WasmEdge probe host =="
  g++ -std=c++23 -fno-exceptions -Wall -Wextra -O1 \
    -I "$WASMEDGE_PREFIX/include" \
    "$here/host/wasmedge_probe.cpp" -o "$build/wasmedge_probe" \
    "$WASMEDGE_PREFIX/lib/libwasmedge.a" \
    "$SPDLOG_PREFIX/lib/libspdlog.a" "$SPDLOG_PREFIX/lib/libfmt.a" \
    -ldl -lpthread -lm
  echo "  built build/wasmedge_probe"
else
  echo "== skipping WasmEdge host (WASMEDGE_PREFIX unset) =="
fi

if [[ -n "${WAMR_SRC:-}" && -n "${WAMR_LIB:-}" ]]; then
  echo "== building WAMR probe host =="
  g++ -std=c++23 -fno-exceptions -Wall -Wextra -O1 \
    -I "$WAMR_SRC/core/iwasm/include" \
    "$here/host/wamr_probe.cpp" -o "$build/wamr_probe" \
    "$WAMR_LIB" -ldl -lpthread -lm
  echo "  built build/wamr_probe"
else
  echo "== skipping WAMR host (WAMR_SRC/WAMR_LIB unset) =="
fi
