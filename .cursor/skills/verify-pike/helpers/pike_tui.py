#!/usr/bin/env python3
"""PTY drive harness for the Pike native TUI.

`pike_tui.py` manages short-lived Pike sessions attached to a pseudo-terminal.
A tiny per-session broker process owns the PTY master for the lifetime of the
Pike child, so keystrokes (`send`) and screen snapshots (`screen`/`expect`) can
be issued from separate invocations of this script.

Commands:
  spawn   --name N --binary B [--cwd D] [--cols C] [--rows R] [--env K=V ...] -- [ARGS]
  send    --name N (--text "..." | --key NAME)
  screen  --name N [--path FILE]
  expect  --name N --text "..." [--timeout SEC]
  kill    --name N

All state lives under $PIKE_VERIFY_RUN_DIR (default /tmp/pike-verify).
This lets concurrent verification runs stay isolated by RUN_DIR.
"""

import argparse
import fcntl
import json
import os
import re
import select
import signal
import socket
import struct
import sys
import termios
import time
from pathlib import Path

RUN_DIR = Path(os.environ.get("PIKE_VERIFY_RUN_DIR", "/tmp/pike-verify"))

KEYS = {
    "enter": "\r",
    "escape": "\x1b",
    "tab": "\t",
    "backspace": "\x7f",
    "up": "\x1b[A",
    "down": "\x1b[B",
    "left": "\x1b[D",
    "right": "\x1b[C",
    "ctrl-c": "\x03",
    "ctrl-d": "\x04",
    "ctrl-l": "\x0c",
    "ctrl-o": "\x0f",
}


def session_dir(name):
    d = RUN_DIR / name
    d.mkdir(parents=True, exist_ok=True)
    return d


def sock_path(name):
    return str(session_dir(name) / "broker.sock")


def state_file(name):
    return session_dir(name) / "state.json"


def pid_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def broker_request(name, payload, timeout=30):
    sp = sock_path(name)
    if not os.path.exists(sp):
        print(f"no broker for session {name!r}; is it spawned?", file=sys.stderr)
        sys.exit(2)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(sp)
    s.sendall((json.dumps(payload) + "\n").encode())
    buf = b""
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
    s.close()
    return json.loads(buf.decode()) if buf else {}


# ---------------------------------------------------------------------------
# Screen reconstruction
# ---------------------------------------------------------------------------
def interpret_screen(data, rows, cols):
    grid = [[" "] * cols for _ in range(rows)]
    r = c = 0
    i = 0
    n = len(data)
    while i < n:
        ch = data[i]
        if ch == "\x1b":
            m = re.match(r"\x1b\[([0-9;?]*)([A-Za-z@`])", data[i:])
            if m:
                params, final = m.group(1), m.group(2)
                nums = [int(p) for p in params.split(";") if p.isdigit()]
                arg = nums[0] if nums else 1
                if final == "A":
                    r = max(0, r - arg)
                elif final == "B":
                    r = min(rows - 1, r + arg)
                elif final == "C":
                    c = min(cols - 1, c + arg)
                elif final == "D":
                    c = max(0, c - arg)
                elif final in "Hf":
                    rr = (nums[0] - 1) if len(nums) >= 1 and nums[0] else 0
                    cc = (nums[1] - 1) if len(nums) >= 2 and nums[1] else 0
                    r, c = min(max(rr, 0), rows - 1), min(max(cc, 0), cols - 1)
                elif final == "J":
                    if (nums[0] if nums else 0) == 2:
                        grid = [[" "] * cols for _ in range(rows)]
                        r = c = 0
                elif final == "K":
                    mode = nums[0] if nums else 0
                    if mode == 0:
                        for cc in range(c, cols):
                            grid[r][cc] = " "
                    elif mode == 2:
                        grid[r] = [" "] * cols
                i += m.end()
                continue
            if i + 1 < n and data[i + 1] == "]":
                end = data.find("\x07", i)
                if end != -1:
                    i = end + 1
                else:
                    e2 = data.find("\x1b\\", i)
                    i = (e2 + 2) if e2 != -1 else n
                continue
            i += 2
            continue
        if ch == "\r":
            c = 0
        elif ch == "\n":
            r = min(rows - 1, r + 1)
        elif ch == "\b":
            c = max(0, c - 1)
        elif ch in ("\x07",):
            pass
        elif ch == "\t":
            c = min(cols - 1, ((c // 8) + 1) * 8)
        elif ord(ch) >= 32:
            grid[r][c] = ch
            c += 1
            if c >= cols:
                c = cols - 1
        i += 1
    return ["".join(row).rstrip() for row in grid]


# ---------------------------------------------------------------------------
# Broker: owns the PTY master + pike child; services requests over the socket.
# ---------------------------------------------------------------------------
def broker_loop(name, master_fd, child_pid, rows, cols, raw_path):
    rawf = open(raw_path, "ab", buffering=0)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sp = sock_path(name)
    if os.path.exists(sp):
        os.unlink(sp)
    srv.bind(sp)
    srv.listen(4)
    srv.settimeout(0.2)
    running = True
    raw = bytearray()

    def drain():
        while True:
            rl, _, _ = select.select([master_fd], [], [], 0.05)
            if not rl:
                break
            try:
                chunk = os.read(master_fd, 65536)
            except OSError:
                chunk = b""
            if not chunk:
                break
            rawf.write(chunk)
            raw.extend(chunk)

    while running:
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            drain()
            if not pid_alive(child_pid):
                # child exited; drain remaining then shut down
                drain()
                time.sleep(0.2)
                drain()
                running = False
            continue
        data = b""
        while not data.endswith(b"\n"):
            chunk = conn.recv(4096)
            if not chunk:
                break
            data += chunk
        try:
            req = json.loads(data.decode())
        except Exception:
            conn.close()
            continue
        op = req.get("op")
        resp = {}
        if op == "send":
            text = req.get("text", "")
            os.write(master_fd, text.encode())
            time.sleep(req.get("settle", 0.3))
            drain()
            resp = {"ok": True}
        elif op == "screen":
            drain()
            text = "\n".join(interpret_screen(raw.decode("utf-8", "replace"), rows, cols))
            resp = {"screen": text}
        elif op == "expect":
            deadline = time.time() + req.get("timeout", 10)
            found = False
            while time.time() < deadline:
                drain()
                if req.get("text", "") in raw.decode("utf-8", "replace"):
                    found = True
                    break
                time.sleep(0.1)
            resp = {"found": found}
        elif op == "kill":
            running = False
            resp = {"ok": True}
        conn.sendall((json.dumps(resp) + "\n").encode())
        conn.close()

    # teardown
    try:
        os.kill(child_pid, signal.SIGTERM)
    except OSError:
        pass
    for _ in range(30):
        if not pid_alive(child_pid):
            break
        time.sleep(0.1)
    try:
        os.kill(child_pid, signal.SIGKILL)
    except OSError:
        pass
    try:
        os.close(master_fd)
    except OSError:
        pass
    rawf.close()
    srv.close()
    try:
        os.unlink(sp)
    except OSError:
        pass


def cmd_spawn(a):
    import pty

    name = a.name
    sdir = session_dir(name)
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", a.rows, a.cols, 0, 0))

    env = dict(os.environ)
    env["TERM"] = "xterm-256color"
    for kv in a.env:
        k, _, v = kv.partition("=")
        env[k] = v

    args = [a.binary] + list(a.args or [])
    pid = os.fork()
    if pid == 0:
        os.setsid()
        os.dup2(slave, 0)
        os.dup2(slave, 1)
        os.dup2(slave, 2)
        os.close(master)
        if a.cwd:
            os.chdir(a.cwd)
        os.execvpe(args[0], args, env)
        os._exit(127)
    os.close(slave)

    raw_path = str(sdir / "raw.ansi")
    with open(state_file(name), "w") as f:
        json.dump({"child_pid": pid, "cols": a.cols, "rows": a.rows,
                   "raw_path": raw_path, "binary": a.binary}, f)

    broker_pid = os.fork()
    if broker_pid == 0:
        broker_loop(name, master, pid, a.rows, a.cols, raw_path)
        os._exit(0)

    with open(state_file(name), "w") as f:
        json.dump({"child_pid": pid, "broker_pid": broker_pid, "cols": a.cols,
                   "rows": a.rows, "raw_path": raw_path, "binary": a.binary}, f)
    time.sleep(0.8)
    print(f"spawned {name}: pike pid={pid} broker pid={broker_pid} log={raw_path}")


def cmd_send(a):
    text = KEYS.get(a.key, a.key) if a.key else (a.text or "")
    broker_request(a.name, {"op": "send", "text": text, "settle": a.settle})
    print(f"sent to {a.name}: {a.key or a.text!r}")


def cmd_screen(a):
    resp = broker_request(a.name, {"op": "screen"})
    screen = resp.get("screen", "")
    if a.path:
        Path(a.path).parent.mkdir(parents=True, exist_ok=True)
        Path(a.path).write_text(screen + "\n")
        print(f"screen written to {a.path}")
    else:
        print(screen)


def cmd_expect(a):
    resp = broker_request(a.name, {"op": "expect", "text": a.text, "timeout": a.timeout}, timeout=a.timeout + 10)
    if resp.get("found"):
        print(f"found {a.text!r}")
    else:
        print(f"TIMEOUT: {a.text!r} not seen within {a.timeout}s", file=sys.stderr)
        sys.exit(1)


def cmd_kill(a):
    stf = state_file(a.name)
    if stf.exists():
        st = json.loads(stf.read_text())
        broker_request(a.name, {"op": "kill"})
        time.sleep(0.5)
        for pidkey in ("broker_pid", "child_pid"):
            pid = st.get(pidkey)
            if pid and pid_alive(pid):
                try:
                    os.kill(pid, signal.SIGKILL)
                except OSError:
                    pass
        stf.unlink(missing_ok=True)
    print(f"killed {a.name}")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("spawn")
    sp.add_argument("--name", required=True)
    sp.add_argument("--binary", required=True)
    sp.add_argument("--cwd")
    sp.add_argument("--cols", type=int, default=110)
    sp.add_argument("--rows", type=int, default=32)
    sp.add_argument("--env", action="append", default=[])
    sp.add_argument("args", nargs=argparse.REMAINDER)
    sp.set_defaults(fn=cmd_spawn)

    se = sub.add_parser("send")
    se.add_argument("--name", required=True)
    se.add_argument("--text")
    se.add_argument("--key", choices=list(KEYS.keys()))
    se.add_argument("--settle", type=float, default=0.3)
    se.set_defaults(fn=cmd_send)

    sc = sub.add_parser("screen")
    sc.add_argument("--name", required=True)
    sc.add_argument("--path")
    sc.set_defaults(fn=cmd_screen)

    ex = sub.add_parser("expect")
    ex.add_argument("--name", required=True)
    ex.add_argument("--text", required=True)
    ex.add_argument("--timeout", type=float, default=10)
    ex.set_defaults(fn=cmd_expect)

    k = sub.add_parser("kill")
    k.add_argument("--name", required=True)
    k.set_defaults(fn=cmd_kill)

    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
