#!/usr/bin/env bash
# Real terminal-emulator A/B for issue #831's reopened criterion: both runtimes' real TUI
# driven through identical dock growth/shrink steps in tmux (a real terminal emulator on a
# real PTY), every frame's footer rows captured. Complements the existing 831-pty-evidence
# (pike-only) and the VirtualTerminal/frozen-byte comparisons with the missing A/B.
#
# Usage: bash drive-tmux.sh <pike|pi> <outDir>   (run from the repository root)
set -u
cd "$(dirname "$0")/../../../.." || exit 1
REPO=$PWD
RUNTIME=${1:?pike|pi}
OUT=${2:?outDir}
mkdir -p "$OUT/frames-$RUNTIME"

ROOT=$(mktemp -d /tmp/pike831.XXXXXX)
mkdir -p "$ROOT/home" "$ROOT/ws"
export HOME="$ROOT/home" XDG_CONFIG_HOME="$ROOT/home/.config"
SOCK="$ROOT/tmux.sock"
SESS="s831"

if [ "$RUNTIME" = pike ]; then
    BIN="$REPO/build/pike"
    ARGS="--no-session"
else
    # The dist bundle in the frozen checkout is stale (its theme resolver
    # predates the okhsl themes at 71023aec), so drive the real TUI from the
    # frozen source with the checkout's own tsx.
    BIN="$REPO/../pi/node_modules/.bin/tsx"
    ARGS="/home/lansy/Work/github/coding-agent/cpp-coding-harness/../pi/packages/coding-agent/src/cli.ts"
fi

tmux -S "$SOCK" new-session -d -s "$SESS" -c "$ROOT/ws" -x 50 -y 30 \
    "env HOME=$HOME XDG_CONFIG_HOME=$XDG_CONFIG_HOME TSX_TSCONFIG_PATH=$REPO/../pi/tsconfig.json $BIN $ARGS"

frame() { # $1 step name
    tmux -S "$SOCK" capture-pane -t "$SESS" -p >"$OUT/frames-$RUNTIME/$1.txt"
}

# Wait for the footer stats row (both runtimes render a bottom stats row once booted).
wait_boot() {
    for _ in $(seq 1 60); do
        if tmux -S "$SOCK" capture-pane -t "$SESS" -p | grep -qE '\(auto\)|unknown|glm|auto/'; then
            return 0
        fi
        sleep 0.5
    done
    return 1
}

if [ "$RUNTIME" = pi ]; then
    # Fresh state root: the trust prompt gates the editor. Move to 'Do not trust' + Enter.
    for _ in $(seq 1 40); do
        if tmux -S "$SOCK" capture-pane -t "$SESS" -p | grep -q "Trust project folder"; then
            tmux -S "$SOCK" send-keys -t "$SESS" Down Down Down
            sleep 0.3
            tmux -S "$SOCK" send-keys -t "$SESS" Enter
            break
        fi
        sleep 0.5
    done
fi

wait_boot || { echo "BOOT TIMEOUT"; tmux -S "$SOCK" kill-server 2>/dev/null; exit 1; }
sleep 1
frame 01-startup

# 2) slash-command menu opens (dock growth #1)
tmux -S "$SOCK" send-keys -t "$SESS" -l "/"
sleep 1
frame 02-slash-menu

# 3) bracketed nine-line paste (dock growth #2, the reporter's step)
NINE=$'paste line 1\npaste line 2\npaste line 3\npaste line 4\npaste line 5\npaste line 6\npaste line 7\npaste line 8\npaste line 9'
tmux -S "$SOCK" send-keys -t "$SESS" -l $'\x1b[200~'
tmux -S "$SOCK" send-keys -t "$SESS" -l "$NINE"
tmux -S "$SOCK" send-keys -t "$SESS" -l $'\x1b[201~'
sleep 1
frame 03-nine-line-paste

# 4) close the menu, then clear the editor (shrink back)
tmux -S "$SOCK" send-keys -t "$SESS" Escape
sleep 0.5
tmux -S "$SOCK" send-keys -t "$SESS" C-u
sleep 0.5
frame 04-shrunk

# 5) over-budget attempt: menu + eighteen-line paste
tmux -S "$SOCK" send-keys -t "$SESS" -l "/"
sleep 0.5
EIGHTEEN=""
for i in $(seq 1 18); do EIGHTEEN+="bulk line $i"; [ "$i" -lt 18 ] && EIGHTEEN+=$'\n'; done
tmux -S "$SOCK" send-keys -t "$SESS" -l $'\x1b[200~'
tmux -S "$SOCK" send-keys -t "$SESS" -l "$EIGHTEEN"
tmux -S "$SOCK" send-keys -t "$SESS" -l $'\x1b[201~'
sleep 0.5
frame 05-overbudget-paste
sleep 1
frame 06-overbudget-settled

# 6) resize down and back: recovery must not mask an intermediate bad frame
tmux -S "$SOCK" resize-window -t "$SESS" -x 50 -y 20
sleep 1
frame 07-resized-50x20
tmux -S "$SOCK" resize-window -t "$SESS" -x 50 -y 30
sleep 1
frame 08-resized-back-50x30

# Teardown: exit the TUI (C-c twice, C-d), then kill the server before removing the root.
tmux -S "$SOCK" send-keys -t "$SESS" C-c
sleep 0.3
tmux -S "$SOCK" send-keys -t "$SESS" C-c
sleep 0.3
tmux -S "$SOCK" send-keys -t "$SESS" C-d
sleep 0.5
tmux -S "$SOCK" kill-server 2>/dev/null
rm -rf "$ROOT"
echo "done: $OUT/frames-$RUNTIME"
