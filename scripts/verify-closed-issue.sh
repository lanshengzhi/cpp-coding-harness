#!/usr/bin/env bash
# Verify that a closed issue is actually done — closed state, ticked acceptance
# criteria, no in-flight state labels.
#
# Close procedure (AGENTS.md, Validation): tick every met acceptance criterion
# in the issue body, remove the in-flight state labels (needs-*, ready-for-*),
# then close. This script is the completion check: it exits 0 only when the
# issue's state, body, and labels all agree the work is done, and names the
# failing check otherwise. `wontfix` is not flagged — on a wontfix close it is
# the reason for closing, not an in-flight state.
#
# Usage: scripts/verify-closed-issue.sh <issue-number>

set -euo pipefail

n="${1:?usage: verify-closed-issue.sh <issue-number>}"

json="$(gh issue view "$n" --json state,body,labels)"
failing=0

if [[ "$(jq -r '.state' <<<"$json")" != "CLOSED" ]]; then
	echo "issue #$n: state is $(jq -r '.state' <<<"$json"), not CLOSED" >&2
	failing=1
fi

if [[ "$(jq '[.body | scan("^- \\[ \\]")] | length' <<<"$json")" -ne 0 ]]; then
	echo "issue #$n: unticked acceptance criteria remain in the body" >&2
	failing=1
fi

labels="$(jq -r '[.labels[].name | select(test("^(needs-|ready-for-)"))] | join(", ")' <<<"$json")"
if [[ -n "$labels" ]]; then
	echo "issue #$n: in-flight state labels remain: $labels" >&2
	failing=1
fi

if (( failing )); then
	exit 1
fi

echo "issue #$n: done — closed, criteria ticked, no state labels"
