#!/usr/bin/env bash
# Drives the frozen audit-1035 placement-span scenario through BOTH runtimes inside a real
# Kitty 0.49.2 window on this host (Wayland/Hyprland), capturing per-phase compositor
# screenshots (grim) plus each side's emitted byte stream. Host-pinned evidence driver for
# issue #1035's reopened "dated Kitty/WezTerm verification" criterion.
#
# Usage: bash run-capture.sh <wezterm|control> <outDir>   (run from the repository root)
#
# WezTerm is not installed on this host, so the real-emulator arm is Kitty 0.49.2 with the
# WezTerm second-tier trigger selected by WEZTERM_PANE exactly as the frozen capture did; the
# control arm runs the same scenario without WEZTERM_PANE in the same real Kitty.
set -u
cd "$(dirname "$0")/../../../.." || exit 1
REPO=$PWD
SCENARIO=${1:?wezterm|control}
OUT=${2:?outDir}
mkdir -p "$OUT/shots-$SCENARIO"
B64=$(mktemp /tmp/placement-image.XXXXXX.b64)
base64 -w0 "$REPO/docs/research/pi-v1.3.0/1035-emulator-evidence/placement-image.png" >"$B64"
PI_TSX="$REPO/../pi/node_modules/.bin/tsx"
HARNESS=/tmp/capture_placement_span

shot_loop() { # $1 hyprland class, $2 shot dir, $3 running flag
    while [ -f "$3" ]; do
        geo=$(hyprctl clients -j 2>/dev/null | python3 -c '
import json,sys
try:
    clients=json.load(sys.stdin)
except Exception:
    sys.exit(0)
for c in clients:
    if c.get("class")=="'"$1"'":
        x,y=c["at"]; w,h=c["size"]
        print(f"{x},{y} {w}x{h}")
        break
')
        if [ -n "$geo" ]; then
            grim -g "$geo" "$2/$(date +%s%3N).png" 2>/dev/null
        fi
        sleep 0.25
    done
}

pick_frames() { # $1 phases dir, $2 runtime
    python3 - "$1" "$OUT/shots-$SCENARIO/$2" "$OUT" "$2" "$SCENARIO" <<'PY'
import glob, os, shutil, sys
phases_dir, shots_dir, out, runtime, scenario = sys.argv[1:6]
markers = []
for path in glob.glob(os.path.join(phases_dir, "*.txt")):
    name = os.path.basename(path)
    head, _, phase = name.partition("-")
    if head.isdigit():
        markers.append((int(head), phase[:-4]))
markers.sort()
shots = sorted(glob.glob(os.path.join(shots_dir, "*.png")))
shots = [p for p in shots if os.path.basename(p)[:-4].isdigit()]
shots.sort(key=lambda p: int(os.path.basename(p)[:-4]))
for index, (ts, phase) in enumerate(markers):
    end = markers[index + 1][0] if index + 1 < len(markers) else ts + 1500
    candidates = [p for p in shots if ts + 300 <= int(os.path.basename(p)[:-4]) <= end]
    if candidates:
        dst = os.path.join(out, f"{runtime}-{scenario}-{phase}.png")
        shutil.copyfile(candidates[-1], dst)
        print(f"{runtime}/{scenario}/{phase}: {os.path.basename(candidates[-1])}")
    else:
        print(f"{runtime}/{scenario}/{phase}: NO FRAME (ts={ts}, shots={len(shots)})")
PY
}

run_one() { # $1 runtime(pi|cpp)
    local cls="pike1035$1$SCENARIO"
    local phases
    phases=$(mktemp -d /tmp/pike1035-phases.XXXXXX)
    local running
    running=$(mktemp /tmp/pike1035-running.XXXXXX)
    rm -f "$running"; touch "$running"
    mkdir -p "$OUT/shots-$SCENARIO/$1"
    shot_loop "$cls" "$OUT/shots-$SCENARIO/$1" "$running" &
    local loop_pid=$!
    if [ "$1" = pi ]; then
        if [ "$SCENARIO" = wezterm ]; then
            kitty --class "$cls" sh -c "cd '$REPO' && PHASE_DIR='$phases' PLACEMENT_IMAGE_B64=\"\$(cat '$B64')\" WEZTERM_PANE=1 '$PI_TSX' '$REPO/docs/research/pi-v1.3.0/1035-emulator-evidence/capture-placement-span-emulator.mts' wezterm '$OUT'" &
        else
            kitty --class "$cls" sh -c "cd '$REPO' && PHASE_DIR='$phases' PLACEMENT_IMAGE_B64=\"\$(cat '$B64')\" '$PI_TSX' '$REPO/docs/research/pi-v1.3.0/1035-emulator-evidence/capture-placement-span-emulator.mts' control '$OUT'" &
        fi
    else
        if [ "$SCENARIO" = wezterm ]; then
            kitty --class "$cls" sh -c "PHASE_DIR='$phases' WEZTERM_PANE=1 '$HARNESS' wezterm '$B64' '$OUT/cpp-wezterm.log'" &
        else
            kitty --class "$cls" sh -c "PHASE_DIR='$phases' '$HARNESS' control '$B64' '$OUT/cpp-control.log'" &
        fi
    fi
    local kitty_pid=$!
    wait "$kitty_pid"
    rm -f "$running"
    wait "$loop_pid" 2>/dev/null
    pick_frames "$phases" "$1"
    rm -rf "$phases" "$running"
}

echo "== pi / $SCENARIO =="
run_one pi
echo "== cpp / $SCENARIO =="
run_one cpp

rm -f "$B64"
echo "done -> $OUT"
