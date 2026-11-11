#!/bin/sh
# with-lane.sh — run one command inside an owned checkout lane lock.
#
# Two hand-rolled steps caused measurable incidents on 2026-11-11: a lane acquired
# .lane-lock.json but never exported CCH_LANE_NAME, so the consolidation CTest cases (and
# every subprocess they spawn) were refused by the lane gate with 7 failures + 1 error, and
# a lane that finished without releasing the lock blocked the next lane for about 1.5 hours.
# This wrapper makes both steps one operation: it acquires the lock, exports the lane name
# into the command's environment, runs the command, and releases the lock from a trap on
# success, failure and HUP/INT/TERM.
#
# Usage:
#   scripts/dev/with-lane.sh <lane> [--timeout <seconds>] -- <command> [args...]
#
# The wrapper owns the lock for exactly the life of the command. That includes a hold the
# same lane already had: acquire by the holding lane is a no-op, and the wrapper releases at
# exit, so a nested invocation ends the outer hold too. Start one wrapper per lane session.
#
# Exit status is the command's own status (128+signal when the command died from a signal).
# The wrapper traps HUP/INT/TERM, releases the lock and exits 129/130/143. A POSIX shell runs a
# trap set for a signal only after the foreground command finishes, so a signal sent to the
# wrapper alone releases the lock once that command exits; a terminal interrupt reaches the
# whole process group, which stops the command and releases at once. A failed release is
# reported on stderr and never changes the command's status; a SIGKILL cannot run the trap, and
# the lock tool's 4-hour staleness takeover is the only recovery for that case.
#
# Environment: CCH_LANE_TASK_ID and CCH_LANE_AGENT are recorded in the lock exactly as
# scripts/dev/lane-lock.sh reads them. CCH_LANE_NAME is exported for the command; a wrapper
# invoked from a shell that already names a different lane refuses to run.
#
# Runtime: POSIX sh and the tooling scripts/dev/lane-lock.sh already requires (Git, GNU
# coreutils `date`/`stat`, `mkdir`, `sed`, `uname`). No other shell is assumed; no
# non-POSIX syntax, arrays or `local` are used.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P) || {
    printf 'with-lane: cannot resolve the wrapper directory\n' >&2
    exit 2
}
LOCK_TOOL="$SCRIPT_DIR/lane-lock.sh"

usage() {
    cat <<'EOF'
with-lane — run one command while holding the checkout lane lock
(scripts/dev/lane-lock.sh; docs/wiki/pi-alignment/ops-lanes.md).

Usage:
  scripts/dev/with-lane.sh <lane> [--timeout <seconds>] -- <command> [args...]

Acquires the lane lock for <lane> (waiting up to --timeout seconds, default 0: fail at
once), exports CCH_LANE_NAME=<lane> for the command, runs it in the foreground and
releases the lock from a trap on success, failure and HUP/INT/TERM. The command sees the
wrapper's stdin, stdout and stderr. Exit status is the command's own.

The lane name must use [A-Za-z0-9._-]; CCH_LANE_TASK_ID and CCH_LANE_AGENT are recorded
in the lock when set. A shell already inside a different lane is refused.
EOF
}

die() {
    printf 'with-lane: %s\n' "$*" >&2
    exit 2
}

main() {
    [ $# -ge 1 ] || die "no lane given; see --help"
    case $1 in
        --help | -h | help)
            [ $# -eq 1 ] || die "--help takes no other arguments"
            usage
            exit 0
            ;;
    esac
    lane=$1
    shift
    [ -n "$lane" ] || die "empty lane name; see --help"
    [ "$lane" != "--" ] || die "no lane given before '--'; see --help"
    timeout=
    while [ $# -gt 0 ]; do
        case $1 in
            --)
                shift
                break
                ;;
            --timeout)
                [ $# -ge 2 ] || die "--timeout needs a non-negative integer number of seconds"
                timeout=$2
                shift 2
                ;;
            *)
                die "expected '--' before the command, got '$1'; see --help"
                ;;
        esac
    done
    [ $# -ge 1 ] || die "no command after '--'; see --help"
    case $timeout in
        '') ;;
        *[!0-9]*) die "--timeout must be a non-negative integer number of seconds, got '$timeout'" ;;
    esac
    if [ -n "${CCH_LANE_NAME:-}" ] && [ "$CCH_LANE_NAME" != "$lane" ]; then
        die "CCH_LANE_NAME already names a different lane; start this wrapper outside that lane"
    fi
    if [ ! -f "$LOCK_TOOL" ]; then
        die "lane-lock.sh is missing at $LOCK_TOOL; run from a full checkout"
    fi

    if [ -n "$timeout" ]; then
        "$LOCK_TOOL" acquire "$lane" "$timeout" >&2
    else
        "$LOCK_TOOL" acquire "$lane" >&2
    fi
    acquire_status=$?
    [ "$acquire_status" -eq 0 ] || exit "$acquire_status"

    export CCH_LANE_NAME="$lane"

    release_done=0
    lock_release() {
        [ "$release_done" -eq 1 ] && return 0
        release_done=1
        if ! "$LOCK_TOOL" release "$lane" >/dev/null; then
            printf "with-lane: WARNING: release failed; lane '%s' may still hold %s/.lane-lock.json\n" \
                "$lane" "$(git rev-parse --show-toplevel 2>/dev/null || printf '?')" >&2
        fi
    }

    trap 'lock_release' EXIT
    trap 'lock_release; exit 129' HUP
    trap 'lock_release; exit 130' INT
    trap 'lock_release; exit 143' TERM

    # Foreground: the command keeps the wrapper's process group, stdin, stdout, stderr and
    # default signal dispositions. An asynchronous list would be started with SIGINT ignored,
    # so a test run under the wrapper could not be interrupted.
    "$@"
    status=$?
    lock_release
    exit "$status"
}

main "$@"
