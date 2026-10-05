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
import errno
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


def process_starttime(pid):
    """The kernel's start time for a pid, in clock ticks (field 22 of /proc/pid/stat).

    `kill(pid, 0)` only reports that *some* process currently holds the number. A
    pid the kernel has since reused holds a different start time, so this is what
    separates the process this state names from a later one that inherited its
    number. Returns None when it cannot be read -- and a caller that cannot read it
    must not signal, because unknown is not the same process.
    """
    try:
        with open(f"/proc/{pid}/stat", "rb") as f:
            data = f.read()
    except OSError:
        return None
    try:
        # comm may contain spaces and parentheses, so split after its last ')'.
        return int(data[data.rindex(b")") + 2:].split()[19])
    except (ValueError, IndexError):
        return None


def signal_if_same(pid, starttime, sig=signal.SIGKILL):
    """Signal `pid` only while it is provably still the process this state recorded.

    A pid is a name, not a handle: it can be released and reused between a liveness
    check and the signal, so comparing the start time and then calling
    `os.kill(pid, ...)` still leaves a window to land in. A pidfd is bound to the
    process it was opened on, so the signal goes through that handle instead and no
    such window remains. Returns "signalled", "gone", "reused" or "unidentified".
    """
    if starttime is None:
        return "unidentified"            # nothing recorded to compare against
    try:
        fd = os.pidfd_open(pid)
    except OSError as exc:
        if exc.errno == errno.ESRCH:
            return "gone"                # no such process: the number is free
        # No usable handle on this kernel, or no permission to take one. "Cannot
        # look" is not "nothing is there", so this fails closed rather than reading
        # an unsupported pidfd as an already-finished session.
        return "unidentified"
    try:
        current = process_starttime(pid)
        if current is None:
            return "unidentified"        # cannot read an identity to compare
        if current != starttime:
            return "reused"              # the name now points at another process
        try:
            signal.pidfd_send_signal(fd, sig)
        except OSError:
            return "gone"
        return "signalled"
    finally:
        os.close(fd)


def broker_request(name, payload, timeout=30):
    sp = sock_path(name)
    if not os.path.exists(sp):
        print(f"no broker for session {name!r}; is it spawned?", file=sys.stderr)
        sys.exit(2)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(sp)
        s.sendall((json.dumps(payload) + "\n").encode())
        buf = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    except (OSError, socket.timeout) as exc:
        # Exit 2 is "cannot reach the broker", distinct from `cmd_expect`'s exit
        # 1 "the anchor was not seen". Without this the two collapse into one
        # traceback, and "not found" cannot be told from "could not look".
        print(f"cannot reach broker for session {name!r}: {exc}", file=sys.stderr)
        sys.exit(2)
    finally:
        s.close()
    if not buf:
        # The broker accepted the connection and closed it without answering.
        # Exit 3 is "protocol/transport", distinct from 2 "cannot reach the
        # broker" and 1 "the anchor was not seen" -- otherwise a silent broker
        # reads as a missing anchor.
        print(f"broker for session {name!r} closed without a response", file=sys.stderr)
        sys.exit(3)
    try:
        resp = json.loads(buf.decode())
    except ValueError:
        print(f"broker for session {name!r} sent an unparseable response", file=sys.stderr)
        sys.exit(3)
    if not isinstance(resp, dict):
        # A syntactically valid answer of the wrong shape is still a protocol
        # error; callers index it, so it must not pass as a normal reply.
        print(f"broker for session {name!r} sent a {type(resp).__name__}, not an object",
              file=sys.stderr)
        sys.exit(3)
    return resp


# ---------------------------------------------------------------------------
# Screen reconstruction
# ---------------------------------------------------------------------------
def interpret_screen(data, rows, cols):
    """Rebuild the visible screen from a raw ANSI stream.

    Implements the CSI subset pike emits: cursor motion (A/B/C/D/H/f), erase
    (J/K), CR/LF/BS/TAB, and printable text. It does NOT implement the alternate
    screen (``\x1b[?1049h`` / ``l``). On an application that switches screens, or
    uses sequences outside this subset, the reconstruction is incomplete: do not
    read a snapshot from such an application as the whole picture.
    """
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


def screen_text(raw, rows, cols):
    """The visible screen as text -- one rendering shared by `screen` and `expect`.

    Both ops must read the same thing. `expect` searching the raw ANSI stream
    instead would miss an anchor the terminal wrapped or that sits behind
    intervening control bytes, and would match text the application had already
    erased -- so a run record's "found" could name something the user never saw.
    """
    return "\n".join(interpret_screen(raw.decode("utf-8", "replace"), rows, cols))


# ---------------------------------------------------------------------------
# Broker: owns the PTY master + pike child; services requests over the socket.
# ---------------------------------------------------------------------------
def broker_loop(name, master_fd, slave_fd, argv, cwd, env, rows, cols, raw_path):
    """Own the session: fork the pike child, drive its PTY, and reap it.

    The broker is the pike child's parent, so it is also the process that reaps
    it. Ownership of the child therefore stays inside the helper instead of
    resting on an outside reaper.
    """
    child_pid = os.fork()
    if child_pid == 0:
        os.setsid()
        os.dup2(slave_fd, 0)
        os.dup2(slave_fd, 1)
        os.dup2(slave_fd, 2)
        os.close(master_fd)
        os.close(slave_fd)
        if cwd:
            os.chdir(cwd)
        os.execvpe(argv[0], argv, env)
        os._exit(127)
    os.close(slave_fd)

    # Record the child so `cmd_kill` can still reach it if this broker is taken
    # out forcefully. The broker is the only process that knows the pid. spawn
    # writes the state file just after forking us, so wait briefly for it rather
    # than drop the pid to that race.
    for _ in range(50):                       # ~0.5s bound
        try:
            st = json.loads(state_file(name).read_text())
        except (OSError, ValueError):
            time.sleep(0.01)
            continue
        if st.get("broker_pid") != os.getpid():
            break                             # a later session owns the file now
        st["child_pid"] = child_pid
        st["child_starttime"] = process_starttime(child_pid)
        try:
            state_file(name).write_text(json.dumps(st))
        except OSError:
            pass
        break

    child_collected = False
    rawf = None
    srv = None
    bound = False

    def child_reaped():
        """Answer "has the child exited?" and collect it in the same call.

        `kill(pid, 0)` cannot answer this for our own unreaped child: a zombie
        still holds the pid, so it always looks alive. `waitpid(WNOHANG)` answers
        and reaps, which is why the parent uses it. Collecting is recorded, so a
        later teardown can tell an already-reaped child from one still running.
        """
        nonlocal child_collected
        if child_collected:
            return True
        try:
            reaped, _ = os.waitpid(child_pid, os.WNOHANG)
        except ChildProcessError:
            child_collected = True
            return True                      # already collected
        if reaped:
            child_collected = True
        return child_collected

    try:
        rawf = open(raw_path, "ab", buffering=0)
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sp = sock_path(name)
        if os.path.exists(sp):
            os.unlink(sp)
        srv.bind(sp)
        bound = True
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
                if child_reaped():
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
                text = screen_text(raw, rows, cols)
                resp = {"screen": text}
            elif op == "expect":
                deadline = time.time() + req.get("timeout", 10)
                found = False
                while time.time() < deadline:
                    drain()
                    if req.get("text", "") in screen_text(raw, rows, cols):
                        found = True
                        break
                    time.sleep(0.1)
                resp = {"found": found}
            elif op == "kill":
                running = False
                resp = {"ok": True}
            conn.sendall((json.dumps(resp) + "\n").encode())
            conn.close()

    finally:
        if not child_collected:
            # Signal only a child we have not already collected. Once reaped the pid
            # is free, and a signal aimed at that number reaches whatever process the
            # kernel has since given it.
            try:
                os.kill(child_pid, signal.SIGTERM)
            except OSError:
                pass
            for _ in range(30):
                if child_reaped():
                    break
                time.sleep(0.1)
            if not child_reaped():
                try:
                    os.kill(child_pid, signal.SIGKILL)
                except OSError:
                    pass
                try:
                    os.waitpid(child_pid, 0)     # collect what we just killed
                except ChildProcessError:
                    pass
        try:
            os.close(master_fd)
        except OSError:
            pass
        if rawf is not None:
            rawf.close()
        if srv is not None:
            srv.close()
        if bound:
            try:
                os.unlink(sp)
            except OSError:
                pass
        # A stopped broker owns no live session, so it drops the state pointer with
        # the socket -- but only while the file still names this broker. A later
        # session may have replaced it, and that one is not ours to remove. A
        # SIGKILL leaves both; the next bind unlinks the socket, and `cmd_kill`
        # (or the operator) can still remove the state file.
        try:
            st = json.loads(state_file(name).read_text())
            if st.get("broker_pid") == os.getpid():
                state_file(name).unlink(missing_ok=True)
        except (OSError, ValueError):
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
    raw_path = str(sdir / "raw.ansi")

    # The broker owns the pike child: it forks it, drives it, and reaps it. spawn
    # owns the PTY setup and the broker and nothing deeper, so every process here
    # is reaped by its own parent -- except the detached broker, which the
    # boundary note below states instead of leaving implied.
    broker_pid = os.fork()
    if broker_pid == 0:
        broker_loop(name, master, slave, args, a.cwd, env, a.rows, a.cols, raw_path)
        os._exit(0)
    os.close(master)
    os.close(slave)

    # Write the state file once, with the broker pid in it. The child pid arrives
    # later and from the broker, because only the broker knows it.
    with open(state_file(name), "w") as f:
        json.dump({"broker_pid": broker_pid,
                   "broker_starttime": process_starttime(broker_pid),
                   "cols": a.cols,
                   "rows": a.rows, "raw_path": raw_path, "binary": a.binary}, f)
    time.sleep(0.8)

    # Reap a broker that already exited during the settle. Non-blocking on
    # purpose: a live broker must keep serving later `send`/`screen`/`expect`
    # calls, so this never waits on it. Once spawn returns, the broker is a
    # detached process and the OS reaps it -- a boundary this helper states
    # rather than claims to close.
    try:
        reaped, _ = os.waitpid(broker_pid, os.WNOHANG)
    except ChildProcessError:
        reaped = 0
    if reaped:
        try:
            state_file(name).unlink(missing_ok=True)
        except OSError:
            pass
        print(f"broker for {name!r} exited during start-up", file=sys.stderr)

    print(f"spawned {name}: broker pid={broker_pid} "
          f"(pike child owned by the broker) log={raw_path}")


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
    if "found" not in resp:
        # A valid answer that carries no `found` field is a protocol error, not a
        # missing anchor: reporting TIMEOUT would say "the anchor was not seen"
        # about a broker that never looked for it.
        print(f"broker for session {a.name!r} answered without a 'found' field", file=sys.stderr)
        sys.exit(3)
    if not isinstance(resp["found"], bool):
        # The field's type is part of the answer. `{"found": "false"}` is truthy, so
        # testing the value directly would read a protocol error as a positive anchor
        # hit -- the one direction a run record cites as proof the anchor appeared --
        # and "could not look" would be recorded as "looked and found it".
        print(f"broker for session {a.name!r} answered with "
              f"{type(resp['found']).__name__} for 'found', not a boolean", file=sys.stderr)
        sys.exit(3)
    if resp["found"]:
        print(f"found {a.text!r}")
    else:
        print(f"TIMEOUT: {a.text!r} not seen within {a.timeout}s", file=sys.stderr)
        sys.exit(1)


def cmd_kill(a):
    stf = state_file(a.name)
    if stf.exists():
        st = json.loads(stf.read_text())
        stopped = False
        try:
            broker_request(a.name, {"op": "kill"})
            stopped = True
        except SystemExit:
            # The broker is already gone, or its socket is stale. The identity
            # fallback below is exactly what that state needs, so do not abort here -- a
            # transport failure must not skip the reclamation.
            pass
        # Let the broker's own teardown finish before forcing anything: it may
        # wait for the pike child before it unlinks the socket and the state
        # file. A fixed short wait would SIGKILL it mid-teardown and leave both
        # behind.
        broker_pid = st.get("broker_pid")
        for _ in range(40):                       # ~4s bound
            if not broker_pid or not pid_alive(broker_pid):
                break
            time.sleep(0.1)
        if stopped:
            # A broker that accepted this request stops under its own control: its
            # teardown terminates and reaps the pike child and unlinks this state
            # file. Nothing of the child's is ours to kill then -- by that point it may
            # be reaped and its number handed to an unrelated process, and neither a
            # stale snapshot nor `kill(pid, 0)` can tell the two apart. Only the broker
            # itself is still worth forcing.
            targets = ("broker_pid",)
        else:
            # The broker never answered, so both pids are this state's own
            # responsibility.
            targets = ("broker_pid", "child_pid")
        # Either way a signal goes only to a process we can still prove is the one
        # this state names, through a handle rather than the pid: `kill(pid, 0)` (or a
        # start-time comparison) would only show that the number is in use.
        outcomes = []
        for pidkey in targets:
            pid = st.get(pidkey)
            if not pid:
                continue
            outcomes.append((pidkey, signal_if_same(pid, st.get(pidkey + "_starttime"))))
        if any(outcome == "unidentified" for _, outcome in outcomes):
            # A pid this state names is in use, or its identity cannot be read, so we
            # cannot say the process is still ours. Signal nothing and keep the state
            # and socket: guessing here is what turns a cleanup into a kill against an
            # unrelated process.
            which = [key for key, outcome in outcomes if outcome == "unidentified"][0]
            print(f"{a.name}: cannot confirm the process recorded for "
                  f"{which.replace('_pid', '')}; keeping state and socket",
                  file=sys.stderr)
            sys.exit(4)
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
