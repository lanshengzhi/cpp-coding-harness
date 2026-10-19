#!/usr/bin/env bash
# Branch cleanup audit (docs/agents/validation.md, section "Git authority").
# Classifies every local branch matching a ref pattern against an integration
# branch:
#   merged         — an ancestor of the integration branch
#   patch-applied  — no commit patch is unique; the work landed elsewhere
#                    (equivalent commits), safe to delete
#   UNAPPLIED      — commits whose patches landed nowhere; keep the branch
#                    and resolve it before any cleanup
#
# Usage:
#   scripts/branch-audit.sh <integration-branch> [regex]
#
# The regex matches full ref names and defaults to every `pike/` branch.
#
# Exit status: 0 when no branch holds unapplied commits; 1 when at least one
# does (its work must be resolved before deleting branches or worktrees).

set -euo pipefail

cd "$(dirname "$0")/.."

integration="${1:-}"
pattern="${2:-^pike/}"
if [[ -z "$integration" ]]; then
	echo "usage: scripts/branch-audit.sh <integration-branch> [regex]" >&2
	exit 2
fi
if ! git rev-parse --verify --quiet "refs/heads/${integration}" >/dev/null; then
	echo "error: ${integration} is not a local branch" >&2
	exit 2
fi

unapplied_total=0
while IFS= read -r branch; do
	[[ "$branch" == "$integration" ]] && continue
	if git merge-base --is-ancestor "$branch" "$integration" 2>/dev/null; then
		printf '%-55s merged\n' "$branch"
		continue
	fi
	unapplied="$(git cherry "$integration" "$branch" 2>/dev/null | grep -c '^+' || true)"
	if [[ "$unapplied" -eq 0 ]]; then
		printf '%-55s patch-applied (safe to delete)\n' "$branch"
	else
		printf '%-55s UNAPPLIED (%s commit(s) landed nowhere)\n' "$branch" "$unapplied"
		unapplied_total=$((unapplied_total + 1))
	fi
done < <(git for-each-ref --format='%(refname:short)' refs/heads | grep -E "$pattern" | sort || true)

if [[ "$unapplied_total" -gt 0 ]]; then
	echo "branch audit: ${unapplied_total} branch(es) hold unapplied commits; resolve before cleanup" >&2
	exit 1
fi
echo "branch audit: no unapplied commits against ${integration}"
