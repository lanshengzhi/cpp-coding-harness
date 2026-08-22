#!/usr/bin/env bash
# Formatting gate for added or modified lines (CODING_STANDARDS.md section 2;
# validation-enforced per section 14). Checks the lines a branch or working
# tree changes against .clang-format through git clang-format. Untouched
# lines stay outside the gate, mirroring the grandfathering model of
# section 15.
#
# Usage:
#   scripts/format-check.sh                 # working tree changes vs HEAD
#   scripts/format-check.sh origin/main     # branch vs merge-base with REF
#
# Exit status: 0 when every changed line conforms; 1 with the offending patch
# on stdout when clang-format wants different formatting.

set -euo pipefail

cd "$(dirname "$0")/.."

if ! command -v git-clang-format >/dev/null 2>&1 && ! git clang-format -h >/dev/null 2>&1; then
	echo "error: git clang-format is not installed (ship it with clang-format or: pip install clang-format)" >&2
	exit 2
fi

base_commit=""
if [[ $# -gt 1 ]]; then
	echo "usage: scripts/format-check.sh [base-ref]" >&2
	exit 2
elif [[ $# -eq 1 ]]; then
	base_commit="$(git merge-base "$1" HEAD)"
fi

# Always pass an explicit baseline commit: with no argument git clang-format
# only examines the unstaged diff, which silently misses staged work. HEAD
# covers the working tree (staged and unstaged) against the last commit; a
# ref argument narrows the baseline to that branch's merge-base.
# git clang-format --diff exits nonzero whenever it wants to change lines, so
# capture under suppression and classify by content below.
diff_output="$(git clang-format --diff --extensions hpp,cpp "${base_commit:-HEAD}" 2>&1)" || true

if [[ -z "$diff_output" ]]; then
	echo "format check: clean"
	exit 0
fi

case "$diff_output" in
"no modified files to format"* | "no commit is currently checked out"* | "clang-format did not modify any files"*)
	echo "format check: clean"
	exit 0
	;;
esac

echo "$diff_output"
echo ""
echo "error: formatting differs from .clang-format on added or modified lines" >&2
echo "hint: run 'git clang-format' with the same arguments to fix, then re-stage" >&2
exit 1
