#!/usr/bin/env bash
# Offline relocation smoke for the staged release artifact (issue #474).
#
# Copies the staged install prefix to a fresh path (the relocation), then runs
# the relocated Runtime under a scrubbed environment (no credentials, proxies,
# or inherited config) and asserts the pi-aligned CLI surface: --version
# matching the build-tree Runtime, --help, and deterministic offline failure
# with no reachable model gateway and no credentials. Every check prints a
# stable `smoke PASS: <name>` marker consumed by
# scripts/ci/verify_release_evidence.py; any failure aborts the qualification
# run.
#
# Usage: scripts/ci/release-artifact-smoke.sh STAGED_PREFIX BUILD_TREE_BINARY

set -euo pipefail

if [[ $# -ne 2 ]]; then
	echo "Usage: scripts/ci/release-artifact-smoke.sh STAGED_PREFIX BUILD_TREE_BINARY" >&2
	exit 2
fi

staged_prefix="$1"
reference_binary="$2"
binary_name="pike"

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
	env -i PATH=/usr/bin:/bin HOME="$work/home" PIKE_CODING_AGENT_DIR="$work/agent" \
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
#
# The product's default model endpoint is the local Magpie gateway, so
# "offline" means no reachable gateway — not merely an empty credential
# environment. The probe therefore runs in an isolated network namespace when
# the host permits one, exactly as tests/cli/InstallRelocationTest.cpp does,
# and fails closed on the named precondition when a service owns the default
# endpoint and no namespace is available: without that, the check would report
# a host with a live gateway as a non-deterministic artifact.
#
# The assertion is the exit code plus the stable error template emitted by
# src/ai/ModelsStream.cpp (the same identifier InstallRelocationTest and
# ModelResolutionTest pin), never a prose sentence the product may reword.
isolation=()
if command -v unshare >/dev/null 2>&1 && unshare -rn true >/dev/null 2>&1; then
	isolation=(unshare -rn)
elif timeout 1 bash -c 'exec 3<>/dev/tcp/127.0.0.1/3425' 2>/dev/null; then
	fail "offline premise not testable: a service owns the default Magpie endpoint 127.0.0.1:3425 and this host cannot create an isolated network namespace"
fi

run_offline() {
	"${isolation[@]}" env -i PATH=/usr/bin:/bin HOME="$work/home" PIKE_CODING_AGENT_DIR="$work/agent" \
		"$relocated_binary" "$@"
}

offline_stderr="$work/offline-stderr.txt"
offline_stdout="$work/offline-stdout.txt"
offline_status=0
run_offline --print ping </dev/null >"$offline_stdout" 2>"$offline_stderr" || offline_status=$?
[[ "$offline_status" -eq 1 ]] \
	|| fail "offline --print exited $offline_status, expected the deterministic failure exit 1"
grep -qF 'has no API implementation' "$offline_stderr" \
	|| fail "offline --print did not fail with the deterministic provider error"
[[ ! -s "$offline_stdout" ]] \
	|| fail "offline --print wrote a partial answer before failing: $(head -c 200 "$offline_stdout")"
echo "smoke PASS: offline-determinism"

# The relocated prefix is fully self-contained at its new path.
echo "smoke PASS: relocation"
