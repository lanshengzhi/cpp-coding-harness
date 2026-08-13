#!/usr/bin/env bash
# Package-aligned test entry point — build-performance-plan Stage 4 (#433).
#
# Runs every package test shard. This is the uniform tag/filter entry point:
# an optional Catch2 filter argument (a test name or tag substring such as
# "[architecture]") is passed to each shard, and shards whose registered tests
# do not match are skipped so a focused run does not pay for the other
# packages. With no filter, every shard runs in full.
#
# Each shard is a separate executable under the build directory and registers
# with ctest, so `ctest --preset <name>` remains the parallel equivalent; this
# script is the developer/agent-facing sequential entry point with readable
# per-shard output and exit codes.
#
# Usage: scripts/run-tests.sh [--build-dir DIR] [filter...]
#   --build-dir DIR  Directory containing the shard executables
#                    (default: <repo>/build, the vcpkg preset tree).
#   -h, --help       Show this help.
#
# Exit codes: 0 all shards passed, or the filter matched nothing; 1 any shard
# reported failures, or a shard binary is missing with no filter set.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$repo_root/build"

args=()
while [[ $# -gt 0 ]]; do
	case "$1" in
		--build-dir)
			[[ $# -ge 2 ]] || { echo "error: --build-dir requires a directory" >&2; exit 2; }
			build_dir="$2"
			case "$build_dir" in
				/*) ;;
				*) build_dir="$repo_root/$build_dir" ;;
			esac
			shift 2
			;;
		-h | --help)
			sed -n '2,28p' "$0"
			exit 0
			;;
		*)
			args+=("$1")
			shift
			;;
	esac
done

shards=(
	cch_tests_util
	cch_tests_tui
	cch_tests_ai
	cch_tests_agent
	cch_tests_harness_tools
	cch_tests_coding_agent
	cch_tests_coding_agent_interactive
	cch_tests_cli_arch
)

# Catch2 accepts the last non-option argument as a single filter.
filter=""
for arg in "${args[@]}"; do
	if [[ "$arg" != -* ]]; then
		filter="$arg"
	fi
done

failures=0
ran_shards=0
for shard in "${shards[@]}"; do
	exe="$build_dir/$shard"
	if [[ ! -x "$exe" ]]; then
		if [[ -n "$filter" ]]; then
			echo "[skip] $shard: not built ($exe missing)" >&2
		else
			echo "[error] $shard: not built ($exe missing)" >&2
			failures=$((failures + 1))
		fi
		continue
	fi
	if [[ -n "$filter" ]] && ! "$exe" --list-tests | grep -Fq -- "$filter"; then
		echo "[skip] $shard: no tests match '$filter'"
		continue
	fi
	ran_shards=$((ran_shards + 1))
	echo "=== $shard ==="
	if ! "$exe" "${args[@]}"; then
		failures=$((failures + 1))
	fi
done

if ((failures > 0)); then
	echo "FAILED: $failures shard(s) reported failures" >&2
	exit 1
fi
if ((ran_shards == 0)); then
	echo "note: filter '$filter' matched no tests in any shard" >&2
fi
