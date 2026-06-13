#!/usr/bin/env bash
set -euo pipefail

if [[ "${CCH_LIVE_KIMI:-}" != "1" ]]; then
	echo "Kimi live smoke not enabled; set CCH_LIVE_KIMI=1 to run."
	exit 0
fi

if [[ -z "${KIMI_API_KEY:-}" ]]; then
	echo "missing API key; set KIMI_API_KEY before running Kimi live smoke" >&2
	exit 2
fi

binary="${CCH_BINARY:-./build/cpp_harness}"
if [[ ! -x "$binary" ]]; then
	echo "harness binary is not executable: $binary" >&2
	echo "build first, or set CCH_BINARY=/path/to/cpp_harness" >&2
	exit 2
fi

tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/cch-kimi-live.XXXXXX")"
workspace="$tmpdir/workspace"
session="$tmpdir/session.jsonl"
stdout_file="$tmpdir/stdout.log"
stderr_file="$tmpdir/stderr.log"
mkdir -p "$workspace"
printf 'harmless fixture for Kimi live smoke\n' >"$workspace/fixture.txt"

cmd=(
	"$binary"
	--workspace "$workspace"
	--session "$session"
	--base-url https://api.kimi.com/coding/v1
	--model kimi-for-coding
	--api-key-env KIMI_API_KEY
	--max-turns 3
	"Reply with exactly: kimi live smoke ok"
)

set +e
"${cmd[@]}" >"$stdout_file" 2>"$stderr_file"
status=$?
set -e

scan_targets=("$stdout_file" "$stderr_file")
if [[ -f "$session" ]]; then
	scan_targets+=("$session")
fi
if grep -F -- "$KIMI_API_KEY" "${scan_targets[@]}" >/dev/null 2>&1; then
	echo "Kimi live smoke aborted: raw KIMI_API_KEY appeared in captured output or session." >&2
	echo "Diagnostics are retained at: $tmpdir" >&2
	exit 1
fi

if [[ $status -ne 0 ]]; then
	echo "Kimi live smoke failed with exit code $status. Secret scan passed. Diagnostics: $tmpdir" >&2
	echo "--- stdout ---" >&2
	sed -n '1,80p' "$stdout_file" >&2
	echo "--- stderr ---" >&2
	sed -n '1,80p' "$stderr_file" >&2
	exit "$status"
fi

if ! grep -F '[model-request]' "$stdout_file" >/dev/null; then
	echo "Kimi live smoke failed: no model request marker observed. Diagnostics: $tmpdir" >&2
	exit 1
fi
if ! grep -F '[assistant]' "$stdout_file" >/dev/null; then
	echo "Kimi live smoke failed: no assistant output observed. Diagnostics: $tmpdir" >&2
	exit 1
fi
if ! grep -F '[completed]' "$stdout_file" >/dev/null; then
	echo "Kimi live smoke failed: completion marker missing. Diagnostics: $tmpdir" >&2
	sed -n '1,80p' "$stdout_file" >&2
	exit 1
fi

echo "Kimi live smoke passed."
echo "Throwaway workspace/session retained at: $tmpdir"
