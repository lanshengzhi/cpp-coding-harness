#!/usr/bin/env bash
# Build-performance benchmark — Stage 1 measurement contract (#428).
#
# Runs every Stage 1 scenario against an isolated Ninja + ccache build tree and
# records, in one invocation:
#   * fresh configure time (vcpkg manifest, dependencies served from the local
#     binary cache, no network);
#   * clean cold build time (empty build tree, empty ccache);
#   * no-op rebuild time;
#   * warm-cache clean rebuild time (ninja clean, then rebuild identical
#     sources);
#   * one typical production-source incremental build;
#   * one typical test-source incremental build;
#   * the leading-hotspot translation-unit rebuild;
#   * per-target and slowest-translation-unit summaries (from the Ninja log);
#   * compiler, generator, build type, job count, cache state, CPU, memory, and
#     background-load caveats.
#
# Source of truth: docs/build-performance-plan.md, Stage 1. The script only
# measures; it adds no absolute-time CI gate, requires no network once the
# vcpkg binary cache is warm, rejects a second concurrent run against the same
# build directory, and never touches the repository's normal build outputs.
# Results accumulate as timestamped JSON files under the benchmark root's
# results/ directory so later stages can trend them.
#
# Exit codes: 0 success; 1 environment or build failure; 2 usage; 3 a concurrent
# run already owns the benchmark build directory.

set -euo pipefail
exec 3>&1

usage() {
	cat <<'EOF'
Usage: scripts/benchmark-build.sh [options]

Stage 1 build-performance benchmark (see docs/build-performance-plan.md).
Runs every measurement scenario against an isolated Ninja + ccache build tree,
writes a timestamped JSON result under <root>/results, and prints a human
summary. Never touches the repository's normal build outputs.

Options:
  --root DIR           Benchmark root (default: <repo>/benchmark-build).
                       The script owns <root>/build, <root>/ccache and
                       <root>/results and rejects concurrent runs on it.
  --release            Benchmark a Release build instead of Debug.
  --jobs N             Parallel build jobs (default: 4).
  --samples N          Repeat the cheap scenarios (no-op, incremental, hotspot)
                       N times and report the median (default: 3).
  --prod-source FILE   Production source for the typical incremental scenario
                       (default: src/coding_agent/SettingsManager.cpp).
  --test-source FILE   Test source for the typical test-source incremental
                       scenario (default: tests/coding_agent/ModelConfigTest.cpp).
  --hotspot-source FILE
                       Leading-hotspot translation unit
                       (default: src/ai/providers/BoostBeastWebSocketTransport.cpp).
  --compiler PATH      C++ compiler to use (default: CMake's default).
  --vcpkg-root DIR     vcpkg checkout (default: $VCPKG_ROOT or <repo>/.deps/vcpkg).
  --allow-network      Permit vcpkg to download dependencies when the binary
                       cache misses them (default: strict offline).
  -h, --help           Show this help.

Dependencies: cmake, ninja, ccache, python3 (for parsing and JSON). A prior
`scripts/bootstrap.sh --no-build` warms the vcpkg binary cache so the benchmark
configure is fully offline.
EOF
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

root=""
build_type="Debug"
jobs=4
samples=3
prod_source="src/coding_agent/SettingsManager.cpp"
test_source="tests/coding_agent/ModelConfigTest.cpp"
hotspot_source="src/ai/providers/BoostBeastWebSocketTransport.cpp"
compiler=""
vcpkg_root_arg=""
allow_network=0

while [[ $# -gt 0 ]]; do
	case "$1" in
	--root)
		[[ $# -ge 2 ]] || { echo "error: --root requires a directory" >&2; exit 2; }
		root="$2"
		shift 2
		;;
	--release)
		build_type="Release"
		shift
		;;
	--jobs)
		[[ $# -ge 2 ]] || { echo "error: --jobs requires a number" >&2; exit 2; }
		jobs="$2"
		shift 2
		;;
	--samples)
		[[ $# -ge 2 ]] || { echo "error: --samples requires a number" >&2; exit 2; }
		samples="$2"
		shift 2
		;;
	--prod-source)
		[[ $# -ge 2 ]] || { echo "error: --prod-source requires a file" >&2; exit 2; }
		prod_source="$2"
		shift 2
		;;
	--test-source)
		[[ $# -ge 2 ]] || { echo "error: --test-source requires a file" >&2; exit 2; }
		test_source="$2"
		shift 2
		;;
	--hotspot-source)
		[[ $# -ge 2 ]] || { echo "error: --hotspot-source requires a file" >&2; exit 2; }
		hotspot_source="$2"
		shift 2
		;;
	--compiler)
		[[ $# -ge 2 ]] || { echo "error: --compiler requires a path" >&2; exit 2; }
		compiler="$2"
		shift 2
		;;
	--vcpkg-root)
		[[ $# -ge 2 ]] || { echo "error: --vcpkg-root requires a directory" >&2; exit 2; }
		vcpkg_root_arg="$2"
		shift 2
		;;
	--allow-network)
		allow_network=1
		shift
		;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		echo "error: unknown option: $1" >&2
		usage >&2
		exit 2
		;;
	esac
done

require_command() {
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "error: required command '$1' was not found on PATH" >&2
		exit 1
	fi
}

require_command cmake
require_command ninja
require_command ccache
require_command python3

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

root="${root:-$repo_root/benchmark-build}"
if [[ "$root" != /* ]]; then
	root="$repo_root/$root"
fi
build_dir="$root/build"
ccache_dir="$root/ccache"
results_dir="$root/results"
run_dir="$root/.run"
lock_dir="$root/.run.lock"

if [[ -n "$vcpkg_root_arg" ]]; then
	vcpkg_root="$vcpkg_root_arg"
elif [[ -n "${VCPKG_ROOT:-}" ]]; then
	vcpkg_root="$VCPKG_ROOT"
else
	vcpkg_root="$repo_root/.deps/vcpkg"
fi
if [[ "$vcpkg_root" != /* ]]; then
	vcpkg_root="$repo_root/$vcpkg_root"
fi

for f in "$prod_source" "$test_source" "$hotspot_source"; do
	if [[ ! -f "$repo_root/$f" ]]; then
		echo "error: benchmark source file does not exist: $f" >&2
		exit 2
	fi
done

if [[ ! -x "$vcpkg_root/vcpkg" ]]; then
	echo "error: vcpkg checkout not found at $vcpkg_root" >&2
	echo "       run scripts/bootstrap.sh --no-build once to set up dependencies" >&2
	exit 1
fi
if [[ ! -f "$vcpkg_root/scripts/buildsystems/vcpkg.cmake" ]]; then
	echo "error: $vcpkg_root does not look like a vcpkg checkout" >&2
	exit 1
fi

# ---------------------------------------------------------------------------
# Lock: reject a second concurrent run against the same build directory
# ---------------------------------------------------------------------------

acquire_lock() {
	if mkdir "$lock_dir" 2>/dev/null; then
		echo "$$" > "$lock_dir/pid"
		return 0
	fi
	local pid=""
	[[ -f "$lock_dir/pid" ]] && pid="$(cat "$lock_dir/pid" 2>/dev/null || true)"
	if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
		echo "error: another benchmark run (pid $pid) is using build directory $build_dir" >&2
		return 3
	fi
	# Stale lock left by a killed run: reclaim it once.
	rm -rf "$lock_dir"
	if mkdir "$lock_dir" 2>/dev/null; then
		echo "$$" > "$lock_dir/pid"
		return 0
	fi
	echo "error: could not acquire lock on $build_dir" >&2
	return 3
}

release_lock() {
	if [[ -d "$lock_dir" ]] && [[ -f "$lock_dir/pid" ]] \
		&& [[ "$(cat "$lock_dir/pid" 2>/dev/null || true)" == "$$" ]]; then
		rm -rf "$lock_dir"
	fi
	return 0
}

restore_perturbed() {
	if [[ -n "${perturbed_file:-}" && -f "$perturbed_file.benchmark-backup" ]]; then
		mv -f "$perturbed_file.benchmark-backup" "$perturbed_file" 2>/dev/null || true
	fi
	perturbed_file=""
}

cleanup() {
	restore_perturbed
	release_lock
}

# ---------------------------------------------------------------------------
# Timing helpers
# ---------------------------------------------------------------------------

now_s() {
	if [[ -n "${EPOCHREALTIME:-}" ]]; then
		printf '%s' "$EPOCHREALTIME"
	else
		python3 -c 'import time; print(f"{time.time():.6f}")'
	fi
}

elapsed_s() { # start end -> seconds with three decimals
	python3 -c 'import sys; print(f"{(float(sys.argv[2]) - float(sys.argv[1])):.3f}")' "$1" "$2"
}

# timed_run <cmd...> — runs the command with output teed to the per-run build
# log and the terminal, and prints the elapsed seconds on stdout. Returns the
# command's exit status so `set -e` aborts the benchmark on build failure.
timed_run() {
	local start end rc
	start="$(now_s)"
	set +e
	"$@" 2>&1 | tee -a "$run_dir/build.log" >&3
	rc=${PIPESTATUS[0]}
	set -e
	end="$(now_s)"
	echo "$(elapsed_s "$start" "$end")"
	return "$rc"
}

# Perturb a source file's content (a trailing comment) so the next build is a
# real content-change compile, not a ccache hit; restore() puts content and
# mtime back exactly (cp -p preserves the original mtime), leaving the tree
# clean for the next scenario.
perturb() {
	local file="$repo_root/$1"
	cp -p "$file" "$file.benchmark-backup"
	printf '\n// benchmark-build perturbation %s %s\n' "$RANDOM" "$$" >> "$file"
	perturbed_file="$file"
}

restore() {
	local file="$repo_root/$1"
	if [[ -f "$file.benchmark-backup" ]]; then
		mv -f "$file.benchmark-backup" "$file"
	fi
	perturbed_file=""
}

# ---------------------------------------------------------------------------
# System caveats (Linux-first, graceful macOS fallback)
# ---------------------------------------------------------------------------

detect_system() {
	cpu_model="unknown"
	cpu_cores="unknown"
	cpu_threads="unknown"
	mem_total_bytes="0"
	mem_available_bytes=""

	if [[ -f /proc/cpuinfo ]]; then
		cpu_model="$(awk -F': ' '/^model name/{print $2; exit}' /proc/cpuinfo)"
		cpu_cores="$(nproc 2>/dev/null || echo unknown)"
		cpu_threads="$(grep -c '^processor' /proc/cpuinfo || true)"
		mem_total_bytes="$(awk '/^MemTotal:/{print $2 * 1024}' /proc/meminfo)"
		mem_available_bytes="$(awk '/^MemAvailable:/{print $2 * 1024}' /proc/meminfo)"
	else
		# macOS / BSD fallback
		cpu_model="$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
		cpu_cores="$(sysctl -n hw.physicalcpu 2>/dev/null || echo unknown)"
		cpu_threads="$(sysctl -n hw.logicalcpu 2>/dev/null || echo unknown)"
		mem_total_bytes="$(sysctl -n hw.memsize 2>/dev/null || echo 0)"
	fi
}

top_processes() {
	ps -eo pcpu=,comm= --sort=-pcpu 2>/dev/null | head -8 | tr '\n' ';' \
		|| ps -Ao pcpu=,comm= 2>/dev/null | head -8 | tr '\n' ';' \
		|| echo "unknown"
}

current_load() {
	if [[ -f /proc/loadavg ]]; then
		cat /proc/loadavg 2>/dev/null || echo unknown
	else
		sysctl -n vm.loadavg 2>/dev/null || echo unknown
	fi
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

detect_system
load_before="$(current_load)"
top_before="$(top_processes)"
repo_rev="$(git -C "$repo_root" rev-parse --short HEAD 2>/dev/null || echo unknown)"

mkdir -p "$build_dir" "$ccache_dir" "$results_dir" "$run_dir" || true
acquire_lock || exit $?
trap cleanup EXIT

export CCACHE_DIR="$ccache_dir"
unset VCPKG_ROOT
if [[ "$allow_network" -eq 1 ]]; then
	network_required="true"
	unset VCPKG_BINARY_SOURCES
else
	network_required="false"
	cache_dir="${VCPKG_DEFAULT_BINARY_CACHE:-}"
	if [[ -z "$cache_dir" ]]; then
		cache_dir="${XDG_CACHE_HOME:-$HOME/.cache}/vcpkg/archives"
	fi
	export VCPKG_BINARY_SOURCES="clear;files,$cache_dir,read"
fi

echo "== build benchmark (Stage 1) =="
echo "root:       $root"
echo "build type: $build_type, jobs: $jobs, samples: $samples"
echo "compiler:   ${compiler:-CMake default}, generator: Ninja, ccache: $ccache_dir"
echo "network:    $([ "$network_required" = true ] && echo allowed || echo "offline ($VCPKG_BINARY_SOURCES)")"
echo "vcpkg root: $vcpkg_root"
echo ""

# --- 1. Fresh configure (cold build tree, cold ccache) ----------------------
echo "[1/8] wiping build and cache directories under $root"
rm -rf "$build_dir" "$ccache_dir" "$run_dir/build.log"
mkdir -p "$build_dir" "$ccache_dir" "$run_dir"
ccache -s > "$run_dir/ccache.before.txt" 2>&1 || true

configure_args=(
	-S "$repo_root"
	-B "$build_dir"
	-G Ninja
	-DCMAKE_BUILD_TYPE="$build_type"
	-DCMAKE_C_COMPILER_LAUNCHER=ccache
	-DCMAKE_CXX_COMPILER_LAUNCHER=ccache
	-DCMAKE_TOOLCHAIN_FILE="$vcpkg_root/scripts/buildsystems/vcpkg.cmake"
)
if [[ -n "$compiler" ]]; then
	configure_args+=( -DCMAKE_C_COMPILER="$compiler" -DCMAKE_CXX_COMPILER="$compiler" )
fi

echo "[2/8] configuring (fresh)"
configure_time="$(timed_run cmake "${configure_args[@]}")" || {
	echo "error: configure failed; if the vcpkg binary cache is cold, run once with --allow-network" >&2
	exit 1
}
compiler_path="$(grep -m1 '^CMAKE_CXX_COMPILER:FILEPATH=' "$build_dir/CMakeCache.txt" | cut -d= -f2- || echo "${compiler:-c++}")"
compiler_version="$("$compiler_path" --version 2>/dev/null | head -1 || echo unknown)"
cmake_version="$(cmake --version | head -1)"
ninja_version="$(ninja --version)"
ccache_version="$(ccache --version | head -1)"

# --- 2. Clean cold build -----------------------------------------------------
echo "[3/8] clean cold build"
cold_time="$(timed_run ninja -C "$build_dir" -j "$jobs")"
cp -f "$build_dir/.ninja_log" "$run_dir/cold.ninja_log" 2>/dev/null || true
echo "      cold build: ${cold_time}s"

# --- 3. No-op rebuild --------------------------------------------------------
echo "[4/8] no-op rebuild ($samples samples)"
noop_samples=()
for ((i = 0; i < samples; i++)); do
	t="$(timed_run ninja -C "$build_dir" -j "$jobs")"
	noop_samples+=("$t")
done
echo "      no-op: ${noop_samples[*]}s"

# --- 4. Warm-cache clean rebuild --------------------------------------------
echo "[5/8] warm-cache clean rebuild (ninja clean, identical sources)"
ninja -C "$build_dir" clean >/dev/null 2>&1
warm_time="$(timed_run ninja -C "$build_dir" -j "$jobs")"
echo "      warm rebuild: ${warm_time}s"

# --- 5. Typical production-source incremental build -------------------------
# sample_scenario <file> <target> <samples> — perturbs <file>, builds <target>,
# restores, and echoes the sample times space-separated.
sample_scenario() {
	local file=$1 target=$2 n=$3
	local vals=() t
	for ((i = 0; i < n; i++)); do
		perturb "$file"
		t="$(timed_run ninja -C "$build_dir" -j "$jobs" "$target")"
		restore "$file"
		vals+=("$t")
	done
	echo "${vals[*]}"
}

echo "[6/8] production-source incremental ($prod_source, $samples samples)"
read -r -a prod_samples <<< "$(sample_scenario "$prod_source" cpp_harness "$samples")"
echo "      prod incremental: ${prod_samples[*]}s"

# --- 6. Typical test-source incremental build -------------------------------
echo "[7/8] test-source incremental ($test_source, $samples samples)"
read -r -a test_samples <<< "$(sample_scenario "$test_source" cpp_harness_tests "$samples")"
echo "      test incremental: ${test_samples[*]}s"

# --- 7. Leading-hotspot rebuild ---------------------------------------------
echo "[8/8] hotspot rebuild ($hotspot_source, $samples samples)"
read -r -a hotspot_samples <<< "$(sample_scenario "$hotspot_source" cpp_harness "$samples")"
echo "      hotspot: ${hotspot_samples[*]}s"

# --- Post-run caveats --------------------------------------------------------
load_after="$(current_load)"
top_after="$(top_processes)"
ccache -s > "$run_dir/ccache.after.txt" 2>&1 || true

# ---------------------------------------------------------------------------
# Summarize + persist
# ---------------------------------------------------------------------------

timestamp="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
stamp="$(date -u +%Y%m%dT%H%M%SZ)"
results_file="$results_dir/benchmark-$stamp.json"

export BM_TIMESTAMP="$timestamp"
export BM_REPO_REV="$repo_rev"
export BM_CONFIGURE_S="$configure_time"
export BM_COLD_S="$cold_time"
export BM_NOOP_SAMPLES="${noop_samples[*]}"
export BM_WARM_S="$warm_time"
export BM_PROD_SAMPLES="${prod_samples[*]}"
export BM_TEST_SAMPLES="${test_samples[*]}"
export BM_HOTSPOT_SAMPLES="${hotspot_samples[*]}"
export BM_PROD_SOURCE="$prod_source"
export BM_TEST_SOURCE="$test_source"
export BM_HOTSPOT_SOURCE="$hotspot_source"
export BM_COMPILER_PATH="$compiler_path"
export BM_COMPILER_VERSION="$compiler_version"
export BM_GENERATOR="Ninja"
export BM_GENERATOR_VERSION="$ninja_version"
export BM_CMAKE_VERSION="$cmake_version"
export BM_CCACHE_VERSION="$ccache_version"
export BM_BUILD_TYPE="$build_type"
export BM_JOBS="$jobs"
export BM_NETWORK_REQUIRED="$network_required"
export BM_VCPKG_ROOT="$vcpkg_root"
export BM_BUILD_DIR="$build_dir"
export BM_CCACHE_DIR="$ccache_dir"
export BM_CPU_MODEL="$cpu_model"
export BM_CPU_CORES="$cpu_cores"
export BM_CPU_THREADS="$cpu_threads"
export BM_MEM_TOTAL_BYTES="$mem_total_bytes"
export BM_MEM_AVAILABLE_BYTES="${mem_available_bytes:-}"
export BM_LOAD_BEFORE="$load_before"
export BM_LOAD_AFTER="$load_after"
export BM_TOP_BEFORE="$top_before"
export BM_TOP_AFTER="$top_after"
export BM_COLD_LOG="$run_dir/cold.ninja_log"
export BM_CCACHE_BEFORE="$run_dir/ccache.before.txt"
export BM_CCACHE_AFTER="$run_dir/ccache.after.txt"
export BM_RESULTS_FILE="$results_file"

cat > "$run_dir/summarize.py" <<'PY'
#!/usr/bin/env python3
"""Stage 1 build-benchmark summarizer.

Reads the per-run state (Ninja cold-build log, ccache stats, BM_* environment
variables) and writes the accumulated JSON result plus a human summary.
"""
import json
import os
import re
import statistics


def f2(x):
    return round(float(x), 3)


def get(name, default=""):
    return os.environ.get(name, default)


def int_or(s, default=None):
    try:
        return int(s)
    except (TypeError, ValueError):
        return default


def samples_env(name):
    raw = get(name).split()
    vals = [float(v) for v in raw]
    if not vals:
        return None
    return {
        "samples": [f2(v) for v in vals],
        "median": f2(statistics.median(vals)),
        "min": f2(min(vals)),
        "max": f2(max(vals)),
    }


def scenario(name, file):
    s = samples_env(name)
    if s is None:
        s = {"samples": [], "median": None, "min": None, "max": None}
    s["file"] = file
    return s


def parse_ninja_log(path):
    edges = []
    if not os.path.exists(path):
        return edges
    with open(path) as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 4:
                continue
            try:
                start = int(parts[0])
                end = int(parts[1])
            except ValueError:
                continue
            edges.append((start, end, parts[3]))
    return edges


TARGET_RE = re.compile(r"CMakeFiles/([^/]+)\.dir/")


def classify(output):
    m = TARGET_RE.match(output)
    if m:
        return m.group(1)
    base = os.path.basename(output)
    if base in ("cpp_harness", "cpp_harness_tests"):
        return base
    if base.startswith("libcch_") and base.endswith(".a"):
        return base[len("lib"):-len(".a")]
    return None


def parse_ccache(path):
    res = {}
    if not os.path.exists(path):
        return res
    with open(path) as fh:
        text = fh.read()
    m = re.search(r"Cacheable calls:\s*(\d+)\s*/\s*(\d+)", text)
    if m:
        res["cacheable_calls"] = int(m.group(1))
        res["total_calls"] = int(m.group(2))
    m = re.search(r"\n\s*Hits:\s*(\d+)\s*/\s*(\d+)", text)
    if m:
        res["hits"] = int(m.group(1))
        res["hits_total"] = int(m.group(2))
    m = re.search(r"\n\s*Misses:\s*(\d+)\s*/\s*(\d+)", text)
    if m:
        res["misses"] = int(m.group(1))
        res["misses_total"] = int(m.group(2))
    m = re.search(r"Cache size \(GiB\):\s*([\d.]+)", text)
    if m:
        res["cache_size_gib"] = float(m.group(1))
    cacheable = res.get("cacheable_calls", 0)
    if cacheable:
        res["hit_rate"] = f2(res.get("hits", 0) / cacheable)
    return res


def clean_tu_path(output):
    p = TARGET_RE.sub("", output)
    if p.endswith(".o"):
        p = p[:-2]
    return p


def summarize(edges, wall_s):
    per_target = {}
    tu_times = []
    for start, end, output in edges:
        dur_ms = max(0, end - start)
        if dur_ms <= 0:
            continue
        key = classify(output) or "other"
        per_target.setdefault(key, [0, 0])
        per_target[key][0] += dur_ms
        per_target[key][1] += 1
        if output.endswith(".o"):
            tu_times.append((output, dur_ms))
    targets = [
        {"target": key, "seconds": f2(ms / 1000.0), "edges": count}
        for key, (ms, count) in sorted(
            per_target.items(), key=lambda kv: kv[1][0], reverse=True
        )
    ]
    tus = sorted(tu_times, key=lambda x: x[1], reverse=True)[:10]
    slowest = [{"file": clean_tu_path(out), "seconds": f2(ms / 1000.0)} for out, ms in tus]
    aggregate_ms = sum(ms for _, ms in tu_times)
    wall = float(wall_s or 0)
    return {
        "per_target": targets,
        "slowest_translation_units": slowest,
        "aggregate_compile_seconds": f2(aggregate_ms / 1000.0),
        "compiled_units": len(tu_times),
        "total_edges": len(edges),
        "estimated_parallelism": f2(aggregate_ms / 1000.0 / wall) if wall > 0 else None,
    }


measurements = {
    "configure_seconds": f2(get("BM_CONFIGURE_S") or 0),
    "cold_clean_build_seconds": f2(get("BM_COLD_S") or 0),
    "noop_build_seconds": samples_env("BM_NOOP_SAMPLES"),
    "warm_cache_clean_rebuild_seconds": f2(get("BM_WARM_S") or 0),
    "prod_source_incremental_seconds": scenario("BM_PROD_SAMPLES", get("BM_PROD_SOURCE")),
    "test_source_incremental_seconds": scenario("BM_TEST_SAMPLES", get("BM_TEST_SOURCE")),
    "hotspot_rebuild_seconds": scenario("BM_HOTSPOT_SAMPLES", get("BM_HOTSPOT_SOURCE")),
}

environment = {
    "compiler": {"path": get("BM_COMPILER_PATH"), "version": get("BM_COMPILER_VERSION")},
    "generator": get("BM_GENERATOR"),
    "generator_version": get("BM_GENERATOR_VERSION"),
    "cmake_version": get("BM_CMAKE_VERSION"),
    "ccache_version": get("BM_CCACHE_VERSION"),
    "build_type": get("BM_BUILD_TYPE"),
    "jobs": int_or(get("BM_JOBS"), 0),
    "network_required": get("BM_NETWORK_REQUIRED") == "true",
    "vcpkg_root": get("BM_VCPKG_ROOT"),
    "build_dir": get("BM_BUILD_DIR"),
    "ccache_dir": get("BM_CCACHE_DIR"),
    "cache_state_before": parse_ccache(get("BM_CCACHE_BEFORE")),
    "cache_state_after": parse_ccache(get("BM_CCACHE_AFTER")),
    "cpu": {
        "model": get("BM_CPU_MODEL"),
        "cores": int_or(get("BM_CPU_CORES"), get("BM_CPU_CORES")),
        "threads": int_or(get("BM_CPU_THREADS"), get("BM_CPU_THREADS")),
    },
    "memory_bytes": {
        "total": int_or(get("BM_MEM_TOTAL_BYTES"), 0),
        "available": int_or(get("BM_MEM_AVAILABLE_BYTES"), 0),
    },
    "load_average_before": get("BM_LOAD_BEFORE"),
    "load_average_after": get("BM_LOAD_AFTER"),
    "top_processes_before": get("BM_TOP_BEFORE"),
    "top_processes_after": get("BM_TOP_AFTER"),
}

summary = summarize(parse_ninja_log(get("BM_COLD_LOG")), measurements["cold_clean_build_seconds"])

doc = {
    "schema": "cpp-coding-harness/build-benchmark/1",
    "generated_at": get("BM_TIMESTAMP"),
    "repo_revision": get("BM_REPO_REV"),
    "measurements": measurements,
    "summary": summary,
    "environment": environment,
}

results_file = get("BM_RESULTS_FILE")
with open(results_file, "w") as fh:
    json.dump(doc, fh, indent=2)
    fh.write("\n")

# --- Human summary -----------------------------------------------------------
def col(name, sec, detail=""):
    line = "  %-26s %10s s" % (name, sec)
    if detail:
        line += "   (%s)" % detail
    return line


cache = environment["cache_state_after"]
if cache.get("cacheable_calls"):
    cache_state = "%d hits / %d cacheable (%.0f%%)" % (
        cache.get("hits", 0),
        cache["cacheable_calls"],
        100.0 * cache.get("hit_rate", 0),
    )
else:
    cache_state = "no cacheable calls"

print()
print("Build benchmark results (%s)" % get("BM_TIMESTAMP"))
print("=" * 60)
print(col("configure", measurements["configure_seconds"]))
print(col("clean cold build", measurements["cold_clean_build_seconds"]))
print(col("no-op build (median)", (measurements["noop_build_seconds"] or {}).get("median", "-")))
print(col("warm-cache clean rebuild", measurements["warm_cache_clean_rebuild_seconds"]))
print(col("prod-source incremental", measurements["prod_source_incremental_seconds"]["median"], get("BM_PROD_SOURCE")))
print(col("test-source incremental", measurements["test_source_incremental_seconds"]["median"], get("BM_TEST_SOURCE")))
print(col("hotspot rebuild", measurements["hotspot_rebuild_seconds"]["median"], get("BM_HOTSPOT_SOURCE")))

print()
print("Per-target (cold build):")
for t in summary["per_target"]:
    print("  %-26s %10.1f s (%d edges)" % (t["target"], t["seconds"], t["edges"]))

print()
print("Slowest translation units:")
for t in summary["slowest_translation_units"]:
    print("  %-26s %10.1f s" % (t["file"], t["seconds"]))
print("  aggregate compile: %.1f s over %d units; estimated parallelism %.2f" % (
    summary["aggregate_compile_seconds"],
    summary["compiled_units"],
    summary["estimated_parallelism"] or 0,
))

print()
print("Environment: %s | %s %s | %s | %d jobs | %s | ccache: %s" % (
    environment["compiler"]["version"],
    environment["generator"],
    environment["generator_version"],
    environment["build_type"],
    environment["jobs"],
    "offline" if not environment["network_required"] else "network allowed",
    cache_state,
))
print("CPU: %s (%s threads)" % (environment["cpu"]["model"], environment["cpu"]["threads"]))
print("Memory: %.1f GiB total, %.1f GiB available" % (
    environment["memory_bytes"]["total"] / (1024 ** 3),
    environment["memory_bytes"]["available"] / (1024 ** 3),
))
print("Load average: before %s | after %s" % (get("BM_LOAD_BEFORE"), get("BM_LOAD_AFTER")))
print("Top processes (after): %s" % get("BM_TOP_AFTER"))
PY
python3 "$run_dir/summarize.py"

cp -f "$results_file" "$results_dir/latest.json" 2>/dev/null || true
echo ""
echo "Results: $results_file"
echo "Benchmark complete."
