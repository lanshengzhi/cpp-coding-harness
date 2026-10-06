#!/usr/bin/env bash
# Compiles the standalone MCP stdio probe directly with the pinned toolchain.
# This is not a CMake target and is not part of the test suite (#866 probe).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$here" rev-parse --show-toplevel)"

CXX="${CXX:-g++}"
# The vcpkg installed tree for the pinned manifest baseline. A configured
# worktree exposes it at <build>/vcpkg_installed/x64-linux; a linked worktree
# that has no build/ of its own can point VCPKG_INSTALLED_DIR at the primary
# checkout's build/vcpkg_installed/x64-linux/include.
build_dir="${BUILD_DIR:-$repo_root/build}"
vcpkg_inc="${VCPKG_INSTALLED_DIR:-$build_dir/vcpkg_installed/x64-linux/include}"
vcpkg_lib="${VCPKG_LIB_DIR:-$build_dir/vcpkg_installed/x64-linux/lib}"

if [[ ! -d "$vcpkg_inc" ]]; then
  echo "build.sh: vcpkg include tree not found at $vcpkg_inc" >&2
  echo "Configure once (cmake --preset vcpkg) or set VCPKG_INSTALLED_DIR." >&2
  exit 1
fi

compile() {
  local source="$1"
  local output="$2"
  "$CXX" -std=c++23 -fno-exceptions -DBOOST_ASIO_NO_EXCEPTIONS -O0 -g \
    -isystem "$vcpkg_inc" \
    -o "$here/$output" "$here/$source" \
    -L"$vcpkg_lib" -lboost_process -lpthread
  echo "built $here/$output"
}

compile probe_stdio.cpp probe_stdio
compile probe_boost_process.cpp probe_boost_process
