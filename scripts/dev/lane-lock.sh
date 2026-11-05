#!/bin/sh
# lane-lock.sh — one-code-lane-at-a-time advisory lock for the main checkout.
#
# The main checkout serializes code/evidence lanes (docs/wiki/pi-alignment/ops-lanes.md):
# every lane's endpoint is the same read-modify-write evidence pipeline, so two lanes must
# never build or rewrite it concurrently. This tool is layer three of the owner-approved
# 2026-11-05 mitigation: a lane takes an explicit, self-identifying hold on the checkout,
# and the build/check entry points (consolidate_audits.py build/check) refuse to run under
# a foreign hold. Light tasks (gh, docs/wiki edits, read-only research) never take the lock.
#
# The lock is `.lane-lock.json` at the repository root: one machine-written line naming
# the holding lane, the acquiring agent, an optional task id and the acquisition time.
# Acquisition is atomic through `set -C` noclobber redirection, so exactly one contender's
# create succeeds. A lock held by a different lane refuses further acquisitions until it is
# released or goes stale (older than 4 hours), at which point a new lane may take it over.
# `release` is fail-closed: only the holding lane may release. A lock whose JSON cannot be
# read is treated as fresh, never stale, so staleness can never silently justify a takeover.
#
# Usage:
#   scripts/dev/lane-lock.sh acquire <lane-name> [timeout-seconds]
#   scripts/dev/lane-lock.sh release <lane-name>
#   scripts/dev/lane-lock.sh status
#   scripts/dev/lane-lock.sh --help
#
# Environment:
#   CCH_LANE_TASK_ID   recorded as `taskId` in the lock when set
#   CCH_LANE_AGENT     recorded as `agent` (default USER@HOST)
#
# Exit status: acquire/release return 0 on success and 1 on refusal; `acquire` by the
# holding lane again is a successful no-op. status returns 0 when the lock is free and 1
# when it is held, with the holder on stdout.

set -eu

LOCK_FILE_NAME=".lane-lock.json"
STALE_SECONDS=14400 # 4 hours

die() {
    printf 'lane-lock: %s\n' "$*" >&2
    exit 1
}

lock_path() {
    root=$(git rev-parse --show-toplevel 2>/dev/null) \
        || die "not inside a git work tree; run from the checkout the lane owns"
    printf '%s/%s\n' "$root" "$LOCK_FILE_NAME"
}

lock_field() {
    # $1 = lock file, $2 = field name. The lock is a machine-written single-line JSON
    # object; absent or unreadable fields print nothing.
    sed -n "s/.*\"$2\": *\"\\([^\"]*\\)\".*/\\1/p" "$1" 2>/dev/null | sed -n '1p' || true
}

agent_name() {
    if [ -n "${CCH_LANE_AGENT:-}" ]; then
        printf '%s\n' "$CCH_LANE_AGENT"
    else
        printf '%s@%s\n' "${USER:-unknown}" "$(uname -n 2>/dev/null || printf 'unknown')"
    fi
}

valid_lane() {
    # Lane names are single-line identifiers; a conservative whitelist keeps the JSON
    # valid and the name shell-safe without character-by-character escaping.
    case $1 in
        '' | *[!A-Za-z0-9._-]*)
            return 1
            ;;
    esac
    return 0
}

require_lane_arg() {
    if ! valid_lane "$1"; then
        die "invalid lane name '$1' (use single-line [A-Za-z0-9._-] identifier characters)"
    fi
}

write_lock() {
    # $1 = lock file, $2 = lane. Atomic create: with noclobber set, the redirection fails
    # instead of truncating when the file already exists.
    agent=$(agent_name)
    started=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    task=${CCH_LANE_TASK_ID:-}
    if [ -n "$task" ]; then
        content=$(printf '{"lane":"%s","agent":"%s","taskId":"%s","startedAt":"%s"}' \
            "$2" "$agent" "$task" "$started")
    else
        content=$(printf '{"lane":"%s","agent":"%s","startedAt":"%s"}' "$2" "$agent" "$started")
    fi
    if (set -C; printf '%s\n' "$content" >"$1") 2>/dev/null; then
        return 0
    fi
    return 1
}

holder_summary() {
    # $1 = lock file; one-line description of the current holder.
    holder=$(lock_field "$1" lane)
    agent=$(lock_field "$1" agent)
    task=$(lock_field "$1" taskId)
    started=$(lock_field "$1" startedAt)
    task_text=''
    if [ -n "$task" ]; then
        task_text=" taskId=$task"
    fi
    printf "lane '%s' (agent=%s%s since=%s)" "${holder:-unknown}" "${agent:-unknown}" "$task_text" "${started:-unknown}"
}

lock_age_seconds() {
    # $1 = lock file; prints the age in seconds, or nothing when startedAt cannot be
    # parsed — an unparsable lock must read as fresh, never stale.
    started=$(lock_field "$1" startedAt)
    started_epoch=$(date -u -d "$started" +%s 2>/dev/null) || return 0
    printf '%s\n' $(( $(date -u +%s) - started_epoch ))
}

human_age() {
    printf '%dh%02dm\n' $(( $1 / 3600 )) $(( ($1 % 3600) / 60 ))
}

cmd_acquire() {
    # $1 = lane, $2 = timeout seconds (default 0: fail immediately while held).
    [ $# -ge 1 ] || die "acquire needs a lane name; see --help"
    [ $# -le 2 ] || die "acquire takes at most a lane name and a timeout; see --help"
    lane=$1
    timeout=${2:-0}
    require_lane_arg "$lane"
    case $timeout in
        '' | *[!0-9]*) die "acquire: timeout-seconds must be a non-negative integer, got '$timeout'" ;;
    esac
    lock=$(lock_path)
    deadline=$(( $(date -u +%s) + timeout ))
    while :; do
        if write_lock "$lock" "$lane"; then
            printf "acquired: lane '%s' holds %s\n" "$lane" "$lock"
            exit 0
        fi
        holder=$(lock_field "$lock" lane) || holder=''
        if [ -n "$holder" ] && [ "$holder" = "$lane" ]; then
            printf "already held by this lane '%s' (since %s)\n" "$lane" "$(lock_field "$lock" startedAt)"
            exit 0
        fi
        age=$(lock_age_seconds "$lock")
        if [ -n "$age" ] && [ "$age" -gt "$STALE_SECONDS" ]; then
            printf 'lane-lock: taking over stale lock (age %s > 4h) held by %s\n' \
                "$(human_age "$age")" "$(holder_summary "$lock")" >&2
            rm -f "$lock"
            continue
        fi
        now=$(date -u +%s)
        if [ "$now" -ge "$deadline" ]; then
            die "acquire: timed out after ${timeout}s; lock held by $(holder_summary "$lock")"
        fi
        wait=$(( deadline - now ))
        if [ "$wait" -gt 2 ]; then
            wait=2
        fi
        sleep "$wait"
    done
}

cmd_release() {
    # $1 = lane. Fail-closed: only the holding lane may release.
    [ $# -eq 1 ] || die "release needs exactly a lane name; see --help"
    lane=$1
    require_lane_arg "$lane"
    lock=$(lock_path)
    if [ ! -f "$lock" ]; then
        printf 'lane-lock: no lock held; nothing to release\n'
        exit 0
    fi
    holder=$(lock_field "$lock" lane) || holder=''
    if [ "$holder" != "$lane" ]; then
        die "release: fail-closed — lock held by $(holder_summary "$lock"), not by lane '$lane'"
    fi
    rm -f "$lock"
    printf "released: lane '%s'\n" "$lane"
    exit 0
}

cmd_status() {
    lock=$(lock_path)
    if [ ! -f "$lock" ]; then
        printf 'free (no %s)\n' "$LOCK_FILE_NAME"
        exit 0
    fi
    age=$(lock_age_seconds "$lock")
    if [ -n "$age" ]; then
        if [ "$age" -gt "$STALE_SECONDS" ]; then
            stale=yes
        else
            stale=no
        fi
        age_text=$(human_age "$age")
    else
        stale=unknown
        age_text=unknown
    fi
    printf 'held by %s age=%s stale=%s\n' "$(holder_summary "$lock")" "$age_text" "$stale"
    exit 1
}

usage() {
    cat <<'EOF'
lane-lock — one code lane at a time in the main checkout
(ops-lanes: docs/wiki/pi-alignment/ops-lanes.md).

Usage:
  scripts/dev/lane-lock.sh acquire <lane-name> [timeout-seconds]
      Atomically create .lane-lock.json at the repository root naming the lane.
      Refuses while a different lane holds the lock, waiting up to timeout-seconds
      (default 0: fail at once); a lock older than 4 hours is stale and a new lane
      takes it over. Re-acquire by the holding lane is a successful no-op.
  scripts/dev/lane-lock.sh release <lane-name>
      Remove the lock. Fail-closed: only the holding lane may release; releasing
      a free lock is a successful no-op.
  scripts/dev/lane-lock.sh status
      Print "free" or the holder with age and staleness. Exit 0 when free, 1 when held.
  scripts/dev/lane-lock.sh --help
      Print this text.

The lock records {lane, agent, taskId?, startedAt}; CCH_LANE_TASK_ID is recorded as
taskId when set, CCH_LANE_AGENT overrides the agent (default USER@HOST). The
consolidation build/check entry points refuse to run under a foreign lock; the caller
proves its lane with CCH_LANE_NAME and only the owning lane sets CCH_LANE_LOCK_SKIP=1.
EOF
}

main() {
    command_name=${1:-}
    case $command_name in
        acquire)
            shift
            cmd_acquire "$@"
            ;;
        release)
            shift
            cmd_release "$@"
            ;;
        status)
            shift
            [ $# -eq 0 ] || die "status takes no arguments; see --help"
            cmd_status
            ;;
        --help | -h | help)
            usage
            exit 0
            ;;
        '')
            usage >&2
            exit 2
            ;;
        *)
            die "unknown command '$command_name'; see --help"
            ;;
    esac
}

main "$@"
