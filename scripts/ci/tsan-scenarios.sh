#!/usr/bin/env bash
# Focused repeated ThreadSanitizer scenarios over the high-risk asynchronous
# ownership and shutdown seams (issue #473).
#
# Each scenario is a CTest label selection over the supported test suite's
# issue-traceability labels (CODING_STANDARDS.md section 11.4): the label names
# the seam-owning issue, so the scenario set tracks the tests that cover each
# seam. Every scenario runs its tests repeatedly (`ctest --repeat until-fail`)
# up to a bounded repetition budget, with a bounded per-test timeout, and stops
# at the first sanitizer finding. Fatal-contract probes (label "fatal") are
# excluded: they terminate the process by design and are covered by the plain
# and ASan+UBSan suites.
#
# Reproduction information: before each scenario the script prints the exact
# ctest invocation, the seam, the label selection, the repetition budget, the
# timeout, the git revision, and TSAN_OPTIONS. A failure names the scenario and
# the failing test, so the exact command can be rerun locally to reproduce.
#
# Usage: scripts/ci/tsan-scenarios.sh [build-dir] [repetitions]
#   build-dir    TSan build tree (default: build/tsan, the vcpkg-tsan preset).
#   repetitions  Per-scenario repetition budget (default: 10).

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"

build_dir="${1:-$repo_root/build/tsan}"
repetitions="${2:-10}"
per_test_timeout=180

if [[ ! -d "$build_dir" ]]; then
	echo "error: TSan build tree '$build_dir' does not exist; configure and build the vcpkg-tsan preset first" >&2
	exit 2
fi
case "$repetitions" in
	'' | *[!0-9]*)
		echo "error: repetitions must be a positive integer, got '$repetitions'" >&2
		exit 2
		;;
esac

# Seam -> CTest label regex. Labels come from Catch2 tags via
# ADD_TAGS_AS_LABELS; the issue label is the seam's owning issue. The
# cancellation seam also selects the "abort" tag: the suspended-tool abort
# lifecycle (issue #40) exercises the same cancellation ownership path.
scenarios=(
	"async-result|issue451"
	"mailboxes|issue459|issue465"
	"subscriptions|issue452"
	"process-draining|issue458"
	"persistence|issue464"
	"cancellation|issue88|^abort$"
	"session-replacement|issue466"
	"session-close|issue467"
)

echo "TSan scenario run: build=$build_dir repetitions=$repetitions per-test-timeout=${per_test_timeout}s"
echo "git revision: $(git -C "$repo_root" rev-parse HEAD 2>/dev/null || echo unknown)"
echo "TSAN_OPTIONS: ${TSAN_OPTIONS:-<unset>}"

for entry in "${scenarios[@]}"; do
	seam="${entry%%|*}"
	label_regex="${entry#*|}"
	echo "== scenario: $seam (labels: $label_regex, up to $repetitions repetitions, fatal probes excluded) =="
	# Echo the exact invocation so a failure can be reproduced verbatim.
	echo "+ ctest --test-dir $build_dir -L '$label_regex' -LE '^fatal$' --repeat until-fail:$repetitions --timeout $per_test_timeout --output-on-failure --no-tests=error"
	ctest \
		--test-dir "$build_dir" \
		-L "$label_regex" \
		-LE "^fatal$" \
		--repeat "until-fail:$repetitions" \
		--timeout "$per_test_timeout" \
		--output-on-failure \
		--no-tests=error
	echo "scenario $seam: clean"
done

echo "All TSan scenarios completed within their repetition budgets without sanitizer findings."
