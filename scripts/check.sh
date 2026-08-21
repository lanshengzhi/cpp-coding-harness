#!/usr/bin/env bash
# Focused Validation entry point (issue #512; CONTEXT.md validation tiers):
# an incremental build on the default Debug preset followed by a CTest
# selection, for the during-implementation loop. This script never bootstraps
# vcpkg and never configures --fresh; it requires an existing build/ tree
# configured with the vcpkg preset and points at Fresh Validation
# (scripts/bootstrap.sh --test) otherwise.
#
# CTest names and labels are the sole selection authority (ADR 0039): every
# argument this script does not consume passes straight through to ctest and
# combines with the tier selection.
#
# Usage:
#   scripts/check.sh                       # incremental build, suite minus the architecture label
#   scripts/check.sh -R 'session assembly'   # incremental build, matching tests only
#   scripts/check.sh --target cch_tests_coding_agent -L coding_agent
#   scripts/check.sh --architecture        # architecture-labeled tests only
#
# Full Validation (once before delivery: complete unfiltered suite, gates
# included) is the manual preset loop in README.md; Fresh Validation
# (environment level) is scripts/bootstrap.sh --test.

set -euo pipefail

usage() {
	cat <<'EOF'
Usage: scripts/check.sh [options] [ctest args...]

Focused Validation: incremental build plus CTest on the default Debug preset
(build/). Never bootstraps vcpkg and never configures --fresh. Requires an
existing build/ tree configured with the vcpkg preset; run Fresh Validation
(scripts/bootstrap.sh --test) once to create it.

Options:
  --architecture   Select the architecture label (ctest -L architecture) for
                   architecture-sensitive changes, instead of the default
                   exclusion (ctest -LE architecture).
  --target TARGET  Build only TARGET (for example the owning test shard,
                   cch_tests_agent) instead of the full build.
  -h, --help       Show this help.

Every other argument passes through to ctest unchanged (-R, -L, -E, -LE, -j,
...) and combines with the tier selection above; CTest names and labels
remain the sole selection authority (ADR 0039). An empty selection is an
error (--no-tests=error), so a mistyped name or label never passes silently.

Full Validation (once before delivery: the complete unfiltered offline suite,
including every architecture gate test):
  cmake --build --preset vcpkg && ctest --preset vcpkg
EOF
}

architecture=0
target=""
ctest_args=()

while [[ $# -gt 0 ]]; do
	case "$1" in
	--architecture)
		architecture=1
		shift
		;;
	--target)
		if [[ $# -lt 2 ]]; then
			echo "error: --target requires a value" >&2
			exit 2
		fi
		target="$2"
		shift 2
		;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		ctest_args+=("$1")
		shift
		;;
	esac
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"

# --- Configured-tree guard -------------------------------------------------
#
# The default Debug preset configures into build/ with the pinned vcpkg
# toolchain. Without that tree there is nothing incremental to build or test,
# so fail fast toward Fresh Validation instead of silently configuring.

if [[ ! -f "$repo_root/build/CMakeCache.txt" ]] ||
	! grep -q 'CMAKE_TOOLCHAIN_FILE:.*vcpkg\.cmake' "$repo_root/build/CMakeCache.txt"; then
	echo "error: build/ is not configured with the vcpkg preset" >&2
	echo "run Fresh Validation once to create it: scripts/bootstrap.sh --test" >&2
	exit 1
fi

cmake_bin="$(command -v cmake)"
ctest_bin="$(dirname "$cmake_bin")/ctest"
if [[ ! -x "$ctest_bin" ]]; then
	ctest_bin="$(command -v ctest)"
fi

if [[ "$architecture" -eq 1 ]]; then
	selection=(-L architecture)
else
	selection=(-LE architecture)
fi

build_args=(--build --preset vcpkg)
if [[ -n "$target" ]]; then
	build_args+=(--target "$target")
fi

# CMake presets resolve CMakePresets.json relative to the current working
# directory, so run from repo_root regardless of where the script is invoked.
(
	cd "$repo_root" || exit 1

	echo "Building incrementally with CMake preset 'vcpkg'${target:+ (target: $target)}"
	"$cmake_bin" "${build_args[@]}"

	echo "Running CTest preset 'vcpkg' with selection: ${selection[*]}${ctest_args:+ ${ctest_args[*]}}"
	"$ctest_bin" --preset vcpkg --no-tests=error "${selection[@]}" ${ctest_args:+"${ctest_args[@]}"}
)
