#!/usr/bin/env bash
# Offline relocation smoke for the staged release artifact (issue #474).
#
# Copies the staged install prefix to a fresh path (the relocation), then runs
# the relocated Runtime under a scrubbed environment (no credentials, proxies,
# or inherited config) and asserts the pi-aligned CLI surface: --version
# matching the build-tree Runtime, --help, and deterministic offline failure
# without credentials. Every check prints a stable `smoke PASS: <name>` marker
# consumed by scripts/ci/verify_release_evidence.py; any failure aborts the
# qualification run.
#
# Usage: scripts/ci/release-artifact-smoke.sh STAGED_PREFIX BUILD_TREE_BINARY

set -euo pipefail

if [[ $# -ne 2 ]]; then
	echo "Usage: scripts/ci/release-artifact-smoke.sh STAGED_PREFIX BUILD_TREE_BINARY" >&2
	exit 2
fi

staged_prefix="$1"
reference_binary="$2"
binary_name="cpp_harness"

fail() {
	echo "smoke FAIL: $1" >&2
	exit 1
}

[[ -x "$staged_prefix/bin/$binary_name" ]] || fail "staged Runtime missing at $staged_prefix/bin/$binary_name"
[[ -x "$reference_binary" ]] || fail "build-tree reference Runtime missing at $reference_binary"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Relocate the whole prefix outside its staged spelling.
relocated="$work/relocated"
cp -a "$staged_prefix" "$relocated"
relocated_binary="$relocated/bin/$binary_name"
[[ "$(readlink -f "$relocated_binary")" != "$(readlink -f "$staged_prefix/bin/$binary_name")" ]] \
	|| fail "relocated binary did not leave the staged path"

mkdir -p "$work/home" "$work/agent"

# Scrubbed environment: no inherited credentials, proxies, or config.
run_relocated() {
	env -i PATH=/usr/bin:/bin HOME="$work/home" PI_CODING_AGENT_DIR="$work/agent" \
		"$relocated_binary" "$@"
}

# --version matches the build-tree Runtime.
reference_version="$("$reference_binary" --version)" || fail "--version rejected on the build-tree Runtime"
relocated_version="$(run_relocated --version)" || fail "--version rejected after relocation"
[[ -n "$relocated_version" ]] || fail "--version printed nothing after relocation"
[[ "$relocated_version" == "$reference_version" ]] \
	|| fail "--version drifted after relocation: '$relocated_version' != '$reference_version'"
echo "smoke PASS: version"

# --help prints the pi-aligned flag surface.
help_output="$(run_relocated --help)" || fail "--help rejected after relocation"
grep -qF "Usage:" <<<"$help_output" || fail "--help lost the usage line after relocation"
grep -qF -- "--print" <<<"$help_output" || fail "--help lost --print after relocation"
echo "smoke PASS: help"

# Offline run without credentials fails deterministically.
offline_stderr="$work/offline-stderr.txt"
if run_relocated --print ping </dev/null 2>"$offline_stderr"; then
	fail "offline --print succeeded without credentials"
fi
grep -qF "Unknown provider" "$offline_stderr" \
	|| fail "offline --print did not fail with the deterministic provider error"
echo "smoke PASS: offline-determinism"

# The relocated prefix is fully self-contained at its new path.
echo "smoke PASS: relocation"
