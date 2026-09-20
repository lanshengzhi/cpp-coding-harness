#!/usr/bin/env bash
# Startup-performance benchmark (see docs/build-performance-baseline.md for the
# build-side contract; runtime capacities live in docs/runtime-capacities.md).
# Measures pike cold-process startup: wall time and peak RSS for the offline
# CLI surface, plus binary size and shared-library closure. Prints a human
# summary and writes a timestamped JSON result under <root>/results so later
# runs can trend it.
#
# The script never builds anything, never touches the repository's normal
# build outputs, requires no network, and uses a blanked HOME plus an
# isolated PIKE_CODING_AGENT_DIR, so no live provider or credential is used.
# Default binary resolution prefers the LTO release artifact
# (build/release/pike); a Debug binary is measurable but not comparable.
#
# Out of scope (demoted, not deleted): TUI idle residency and first-frame
# latency need a pty and are nondeterministic under script driving; measure
# them manually until a pty harness exists.
#
# Go: repeated runs on the same host distinguish a ~20% wall/RSS change.
# No-Go / no absolute CI gate yet: accumulate results first, review a >20%
# regression against the previous JSON, same rule as the build baseline.
#
# Exit codes: 0 success; 1 environment or run failure; 2 usage;
# 3 a concurrent run already owns the benchmark root.

set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
root="$repo_root/benchmark-startup"
pike_arg=""
samples=10

usage() {
	cat <<'EOF'
Usage: scripts/benchmark-startup.sh [options]

Options:
  --root DIR     Benchmark root (default: <repo>/benchmark-startup).
                 The script owns <root>/lock, <root>/results and
                 <root>/home and rejects concurrent runs on it.
  --pike PATH    Pike binary to measure (default: build/release/pike,
                 fallback: build/pike).
  --samples N    Samples per scenario (default: 10).
  --help         Print this help.
EOF
}

# A missing option value must reach the usage contract (exit 2) instead of
# failing on the unbound `$2` under `set -u` (exit 1).
require_value() {
	if [[ $# -lt 2 ]]; then
		echo "error: $1 requires a value" >&2
		usage >&2
		exit 2
	fi
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--root) require_value "$@"; root="$2"; shift 2 ;;
		--pike) require_value "$@"; pike_arg="$2"; shift 2 ;;
		--samples) require_value "$@"; samples="$2"; shift 2 ;;
		--help) usage; exit 0 ;;
		*) echo "error: unknown argument $1" >&2; usage >&2; exit 2 ;;
	esac
done

if ! [[ "$samples" =~ ^[1-9][0-9]*$ ]]; then
	echo "error: --samples must be a positive integer" >&2
	exit 2
fi

# run_sample executes under this scrubbed PATH (see below), so python3 is
# checked there rather than against the ambient PATH the script itself runs
# with.
scrubbed_path="/usr/bin:/bin"

require_command() {
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "error: required command '$1' not found" >&2
		exit 1
	fi
}

require_command_on_scrubbed_path() {
	if ! env -i PATH="$scrubbed_path" sh -c 'command -v "$1" >/dev/null 2>&1' sh "$1"; then
		echo "error: required command '$1' not found in $scrubbed_path" >&2
		exit 1
	fi
}

require_command_on_scrubbed_path python3
require_command ldd
require_command free

pike="$pike_arg"
if [[ -z "$pike" ]]; then
	if [[ -x "$repo_root/build/release/pike" ]]; then
		pike="$repo_root/build/release/pike"
	elif [[ -x "$repo_root/build/pike" ]]; then
		pike="$repo_root/build/pike"
	else
		echo "error: no pike binary found (tried build/release/pike, build/pike); pass --pike PATH" >&2
		exit 1
	fi
fi
if [[ ! -x "$pike" ]]; then
	echo "error: pike binary is not executable: $pike" >&2
	exit 1
fi

lock_dir="$root/lock"
results_dir="$root/results"
fake_home="$root/home"
agent_dir="$root/agent-dir"
mkdir -p "$results_dir" "$fake_home" "$agent_dir"

# ---------------------------------------------------------------------------
# Lock: reject a second concurrent run against the same benchmark root
# ---------------------------------------------------------------------------

acquire_lock() {
	if mkdir "$lock_dir" 2>/dev/null; then
		echo "$$" > "$lock_dir/pid"
		return 0
	fi
	local pid=""
	[[ -f "$lock_dir/pid" ]] && pid="$(cat "$lock_dir/pid" 2>/dev/null || true)"
	if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
		echo "error: another startup benchmark run (pid $pid) owns $root" >&2
		return 3
	fi
	# Stale lock left by a killed run: reclaim it once.
	rm -rf "$lock_dir"
	if mkdir "$lock_dir" 2>/dev/null; then
		echo "$$" > "$lock_dir/pid"
		return 0
	fi
	echo "error: could not acquire lock on $root" >&2
	return 3
}

release_lock() {
	if [[ -d "$lock_dir" ]] && [[ -f "$lock_dir/pid" ]] \
		&& [[ "$(cat "$lock_dir/pid" 2>/dev/null || true)" == "$$" ]]; then
		rm -rf "$lock_dir"
	fi
	return 0
}

cleanup() {
	release_lock
}
trap cleanup EXIT

acquire_lock

# Scrubbed environment (mirrors InstallRelocationTest's clean_env): no inherited
# credentials, tokens, or proxies. Without this, an ambient *_API_KEY in the
# calling shell is picked up by Request Authentication and --print goes live,
# which makes the measurement depend on whose shell runs it.
# One sample: spawn pike under python so peak RSS comes from wait4
# (resource.getrusage(RUSAGE_CHILDREN)), exact even for ~10ms processes
# where /proc polling races. Prints: wall_s peak_rss_kb exit_code token_state.
# ---------------------------------------------------------------------------

run_sample() {
	env -i PATH="$scrubbed_path" HOME="$fake_home" PIKE_CODING_AGENT_DIR="$agent_dir" python3 - "$pike" "$@" <<'PYEOF'
import resource, subprocess, sys, time
pike, args = sys.argv[1], sys.argv[2:]
start = time.monotonic()
proc = subprocess.run([pike] + args, stdin=subprocess.DEVNULL,
                      stdout=subprocess.PIPE, stderr=subprocess.PIPE)
wall = time.monotonic() - start
peak = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
# One fresh python process per sample, so RUSAGE_CHILDREN holds exactly this
# sample's pike child: ru_maxrss is that child's absolute peak. The caller
# keeps the max over samples.
sys.stdout.write(f"{wall:.6f} {peak} {proc.returncode} ")
sys.stdout.write("token_ok" if b"Unknown provider" in proc.stderr else "token_absent")
sys.stdout.write("\n")
PYEOF
}

median_of() {
	printf '%s\n' "$@" | sort -g | awk -v n="$#" '{a[NR]=$1} END {if (n%2) print a[(n+1)/2]; else print (a[n/2]+a[n/2+1])/2}'
}

max_of() {
	printf '%s\n' "$@" | sort -g | tail -n 1
}

min_of() {
	printf '%s\n' "$@" | sort -g | head -n 1
}

# sample_scenario <name> <expected_exit> <token:required|absent|any> -- <args...>
sample_scenario() {
	local name="$1" expected_exit="$2" token="$3"
	shift 3
	if [[ "${1:-}" == "--" ]]; then shift; fi
	# shellcheck disable=SC2124
	local args="$@"
	local walls=() rss=() i line wall peak exit_code token_state
	for ((i = 0; i < samples; i++)); do
		# shellcheck disable=SC2086
		line="$(run_sample $args)"
		read -r wall peak exit_code token_state <<<"$line"
		walls+=("$wall")
		rss+=("$peak")
		if [[ "$exit_code" != "$expected_exit" ]]; then
			echo "error: scenario '$name' exited $exit_code, expected $expected_exit" >&2
			return 1
		fi
		if [[ "$token" == "required" && "$token_state" != "token_ok" ]]; then
			echo "error: scenario '$name' misses the 'Unknown provider' stderr token" >&2
			return 1
		fi
	done
	local wall_median wall_min wall_max rss_max
	wall_median="$(median_of "${walls[@]}")"
	wall_min="$(min_of "${walls[@]}")"
	wall_max="$(max_of "${walls[@]}")"
	rss_max="$(max_of "${rss[@]}")"
	printf '%s wall_median=%ss wall_min=%ss wall_max=%ss peak_rss_max=%sKB exit=%s\n' \
		"$name" "$wall_median" "$wall_min" "$wall_max" "$rss_max" "$expected_exit" >&2
	printf '%s|%s|%s|%s|%s' "$name" "$wall_median" "$wall_min" "$wall_max" "$rss_max"
}

echo "pike: $pike"
echo "samples per scenario: $samples"
echo "version: $(env -i PATH="$scrubbed_path" HOME="$fake_home" \
	PIKE_CODING_AGENT_DIR="$agent_dir" "$pike" --version)"
echo

results=()
# shellcheck disable=SC2155
results+=("$(sample_scenario "version" 0 any -- --version)")
# shellcheck disable=SC2155
results+=("$(sample_scenario "help" 0 any -- --help)")
# Deterministic offline failure: no credentials, no network (InstallRelocationTest pins this).
# shellcheck disable=SC2155
results+=("$(sample_scenario "print_offline" 1 required -- --print ping)")

echo
binary_bytes="$(stat -c%s "$pike")"
ldd_count="$(ldd "$pike" 2>/dev/null | grep -c '=>' || true)"
timestamp="$(date -u +%Y%m%dT%H%M%SZ)"
out="$results_dir/$timestamp.json"

python3 - "$out" "${results[@]}" <<PYEOF
import json, sys
out, rows = sys.argv[1], sys.argv[2:]
scenarios = {}
for row in rows:
    name, median, mn, mx, rss = row.split("|")
    scenarios[name] = {
        "samples": $samples,
        "wall_median_s": float(median),
        "wall_min_s": float(mn),
        "wall_max_s": float(mx),
        "peak_rss_max_kb": int(rss),
    }
doc = {
    "schema_version": 1,
    "timestamp_utc": "$timestamp",
    "pike": {"path": "$pike", "bytes": $binary_bytes,
             "ldd_closure_entries": $ldd_count},
    "system": {"uname": "$(uname -srm)",
               "nproc": $(nproc),
               "mem_total_kb": $(free -k | awk '/^Mem:/{print $2}')},
    "scenarios": scenarios,
    "notes": "TUI idle residency out of scope (needs pty). "
             "No absolute gate: review >20% regressions vs the previous JSON.",
}
with open(out, "w") as f:
    json.dump(doc, f, indent=2)
PYEOF

echo "binary: $binary_bytes bytes, ldd closure: $ldd_count entries"
echo "result: $out"
