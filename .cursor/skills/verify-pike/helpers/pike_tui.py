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
import uuid
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


def sock_path(name, token):
    """The socket for one run, addressed by its token rather than by its name.

    A name can be restarted, and a replacement run binds the same path -- so a broker
    tearing down could remove the socket its successor is serving on. A token-specific
    path cannot collide that way: each run only ever unlinks its own.
    """
    return str(session_dir(name) / f"broker-{token}.sock")


def state_file(name):
    return session_dir(name) / "state.json"


def session_token(name):
    """The token of the run the state describes, or None when there is no usable state.

    Callers reach the broker through this rather than through the name, so a request
    cannot land on whichever run holds the name now.
    """
    try:
        return json.loads(state_file(name).read_text()).get("token")
    except (OSError, ValueError):
        return None


def state_lock(name):
    """Hold the session's state lock while publishing or conditionally deleting it.

    Without it a publish and a delete can interleave: `kill` compares the state, a
    `spawn` under the same name publishes a new one, and the delete then removes a
    record it never examined.

    Returns None when the lock cannot be taken. An unknown state is not one to write
    or delete, so callers treat that as failure rather than proceeding without it.
    """
    fd = os.open(str(session_dir(name) / "state.lock"), os.O_CREAT | os.O_RDWR, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        os.close(fd)
        return None
    return fd


def release_lock(fd):
    if fd is None:
        return
    fcntl.flock(fd, fcntl.LOCK_UN)
    os.close(fd)


def write_state(name, state):
    """Publish the state atomically.

    A reader sees either the old record or the new one, never a half-written file:
    `open(..., "w")` truncates in place, and a concurrent reader can catch that and
    read it as "no valid record" -- which is exactly when a delete is allowed.
    """
    path = state_file(name)
    tmp = path.with_suffix(".json.tmp")
    with open(tmp, "w") as f:
        json.dump(state, f)
    os.replace(tmp, path)


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


def open_verified(pid, starttime):
    """Open a handle on `pid`, but only while it is provably the recorded process.

    A pid is a name, not a handle: it can be released and reused between a liveness
    check and a signal, so comparing the start time and then calling `os.kill(pid,
    ...)` still leaves a window to land in. This returns `(fd, None)` with a pidfd
    bound to the process it was opened on, so a signal sent through it later cannot
    reach a different one -- or `(None, reason)`, with reason in "gone", "reused" or
    "unidentified".

    Splitting the check from the signal lets a caller resolve every target before it
    signals any of them, which is what makes "this failed without signalling" true.
    The returned fd belongs to the caller, which closes it on every path -- including
    the failure one, before it exits.
    """
    if starttime is None:
        return None, "unidentified"     # nothing recorded to compare against
    try:
        fd = os.pidfd_open(pid)
    except OSError as exc:
        if exc.errno == errno.ESRCH:
            return None, "gone"          # no such process: the number is free
        # No usable handle on this kernel, or no permission to take one. "Cannot
        # look" is not "nothing is there", so this fails closed rather than reading
        # an unsupported pidfd as an already-finished session.
        return None, "unidentified"
    current = process_starttime(pid)
    if current is None:
        os.close(fd)
        return None, "unidentified"     # cannot read an identity to compare
    if current != starttime:
        os.close(fd)
        return None, "reused"           # the name now points at another process
    return fd, None


def broker_request(name, payload, timeout=30):
    sp = sock_path(name, session_token(name) or "none")
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
def broker_loop(name, master_fd, slave_fd, argv, cwd, env, rows, cols, raw_path, token):
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

    def term_handler(signum, frame):
        # A parent may stop us before we ever serve -- a spawn that could not publish the
        # state does exactly that. A default SIGTERM would end us without running any
        # cleanup and leave the pike child orphaned, because the parent never learns its
        # pid; this process is the only one that has it. Installed here, before anything
        # else, so the only window without it is before the child exists at all.
        try:
            os.kill(child_pid, signal.SIGTERM)
        except OSError:
            pass
        for _ in range(30):
            try:
                if os.waitpid(child_pid, os.WNOHANG)[0]:
                    break
            except ChildProcessError:
                break
            time.sleep(0.1)
        else:
            try:
                os.kill(child_pid, signal.SIGKILL)
            except OSError:
                pass
            try:
                os.waitpid(child_pid, 0)
            except ChildProcessError:
                pass
        os._exit(1)

    signal.signal(signal.SIGTERM, term_handler)

    # Record the child so `cmd_kill` can still reach it if this broker is taken
    # out forcefully. The broker is the only process that knows the pid. spawn
    # writes the state file just after forking us, so wait briefly for it rather
    # than drop the pid to that race.
    registered = False
    for _ in range(50):                       # ~0.5s bound
        # Read, compare and update under one lock. Reading outside it would let this
        # broker write its stale snapshot back after a later spawn published a new
        # token -- the compare and the write have to see the same record.
        lock = state_lock(name)
        if lock is None:
            time.sleep(0.01)
            continue
        try:
            try:
                st = json.loads(state_file(name).read_text())
            except (OSError, ValueError):
                time.sleep(0.01)
                continue
            if st.get("token") != token:
                break                         # a later session owns the file now
            st["child_pid"] = child_pid
            st["child_starttime"] = process_starttime(child_pid)
            write_state(name, st)
            registered = True
            break
        except OSError:
            pass
        finally:
            release_lock(lock)

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
        sp = sock_path(name, token)
        if os.path.exists(sp):
            os.unlink(sp)
        srv.bind(sp)
        bound = True
        srv.listen(4)
        srv.settimeout(0.2)
        # Serve only while this run is registered in the state. Both ways of failing to
        # be registered land here: losing the name to a later run, and never managing to
        # publish the child at all -- a broker serving in either case is a session no
        # state points at. It still falls through to the teardown below, which reaps its
        # own child.
        running = registered
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
                if req.get("token") != token:
                    # The caller resolved the session it meant from the state file, and this
                    # broker is not that session: a name can be restarted, and the socket
                    # now belongs to a newer run. A pid could not tell the two apart, since
                    # the kernel reuses them; the token can, because it is unique per run.
                    # Stopping here would end a session the caller never inspected, so
                    # refuse and stay up -- the caller's verified handles still reclaim the
                    # session its state describes.
                    resp = {"ok": False, "error": "not the session the caller resolved"}
                else:
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
        # A stopped broker owns no live session, so it drops the state pointer with the
        # socket -- but only while the file still names this run. A later session may
        # have replaced it, and that one is not ours to remove. The compare and the
        # delete share the lock, because publishing is what would otherwise slip between
        # them. A SIGKILL leaves both; `cmd_kill` (or the operator) can still remove the
        # state file.
        lock = state_lock(name)
        if lock is not None:
            try:
                try:
                    st = json.loads(state_file(name).read_text())
                except FileNotFoundError:
                    st = "gone"
                except (OSError, ValueError):
                    st = None             # unknown is not ours to delete
                if st == "gone" or (st and st.get("token") == token):
                    state_file(name).unlink(missing_ok=True)
            finally:
                release_lock(lock)


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
    token = uuid.uuid4().hex
    broker_pid = os.fork()
    if broker_pid == 0:
        broker_loop(name, master, slave, args, a.cwd, env, a.rows, a.cols, raw_path, token)
        os._exit(0)
    os.close(master)
    os.close(slave)

    # Publish the state once, atomically, with the run's token in it. The token is what
    # identifies this run: a pid cannot, because the kernel reuses pids, so a later run
    # under the same name could inherit the number and be mistaken for this one. The
    # child pid arrives later and from the broker, because only the broker knows it.
    lock = state_lock(name)
    published = False
    if lock is not None:
        try:
            write_state(name, {"broker_pid": broker_pid,
                               "broker_starttime": process_starttime(broker_pid),
                               "token": token,
                               "cols": a.cols,
                               "rows": a.rows, "raw_path": raw_path, "binary": a.binary})
            published = True
        except OSError:
            pass
        finally:
            release_lock(lock)
    if not published:
        # Without the record, the broker this spawn just forked is one no command can
        # find: `send`/`screen`/`expect`/`kill` all resolve the session through the
        # state. Fail closed and take the broker down rather than report a spawn that
        # nothing can manage.
        try:
            os.kill(broker_pid, signal.SIGTERM)
        except OSError:
            pass
        try:
            os.waitpid(broker_pid, 0)
        except ChildProcessError:
            pass
        print(f"{name}: could not publish the session state; the broker was stopped "
              f"and no session was started", file=sys.stderr)
        sys.exit(1)
    # Wait for the broker to register its child. It writes `child_pid` only after it has
    # taken the state and confirmed this run owns it, so that field is what separates a
    # started session from one that died on the way up. Reporting `spawned` before it is
    # a success claim nothing has established -- and the broker now exits when it cannot
    # register, which makes that path reachable rather than theoretical.
    deadline = time.time() + 5.0
    registered = False
    broker_gone = False
    while time.time() < deadline:
        try:
            st_now = json.loads(state_file(name).read_text())
        except (OSError, ValueError):
            st_now = {}
        if st_now.get("token") == token and st_now.get("child_pid"):
            registered = True
            break
        try:
            if os.waitpid(broker_pid, os.WNOHANG)[0]:
                broker_gone = True
                break
        except ChildProcessError:
            broker_gone = True
            break
        time.sleep(0.05)

    if not registered:
        # Only drop the record this spawn published: a run started under the same name
        # since owns its own, and removing it would leave a live session with no state.
        lock = state_lock(name)
        if lock is not None:
            try:
                try:
                    current = json.loads(state_file(name).read_text())
                except (OSError, ValueError):
                    current = None
                if current is not None and current.get("token") == token:
                    state_file(name).unlink(missing_ok=True)
            finally:
                release_lock(lock)
        why = ("the broker exited during start-up" if broker_gone
               else "the broker did not register its child in time")
        print(f"{name}: no session was started -- {why}", file=sys.stderr)
        sys.exit(1)

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
        # Resolve every identity BEFORE anything that can act. Asking the broker to stop
        # makes it signal its pike child and drop the socket and the state, so a target
        # found unidentifiable after that request would force an exit claiming nothing
        # was signalled while something already had been. Resolving both pids up front
        # is what makes the failure contract hold on every path, not just the fallback.
        #
        # A pid is a name, not a handle: it can be released and reused between a check
        # and a signal, so each verified target yields a pidfd bound to the process it
        # was opened on. `kill(pid, 0)` (or a start-time comparison) would only show
        # that the number is in use.
        handles = {}
        unverified = None
        for pidkey in ("broker_pid", "child_pid"):
            pid = st.get(pidkey)
            if not pid:
                continue
            fd, reason = open_verified(pid, st.get(pidkey + "_starttime"))
            if fd is not None:
                handles[pidkey] = fd
            elif reason == "unidentified" and unverified is None:
                unverified = pidkey
        if unverified:
            # Nothing has been sent and nothing has been dropped: every handle closes
            # before any signal goes out, so the state and the socket stay and the
            # caller sees an explicit failure. Guessing here is what turns a cleanup
            # into a kill against an unrelated process.
            for fd in handles.values():
                os.close(fd)
            print(f"{a.name}: cannot confirm the process recorded for "
                  f"{unverified.replace('_pid', '')}; keeping state and socket",
                  file=sys.stderr)
            sys.exit(4)
        stopped = False
        try:
            resp = broker_request(a.name,
                                  {"op": "kill", "token": st.get("token")})
            if resp.get("ok") is True:
                stopped = True
            else:
                print(f"{a.name}: the name now belongs to another session; reclaiming "
                      f"only the one this state describes", file=sys.stderr)
        except SystemExit:
            # The broker is already gone, or its socket is stale. The verified handles
            # are exactly what that state needs, so do not abort here -- a transport
            # failure must not skip the reclamation.
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
            # be reaped and its number handed to an unrelated process. Only the broker
            # itself is still worth forcing.
            targets = ("broker_pid",)
        else:
            # The broker never answered, so both pids are this state's own
            # responsibility.
            targets = ("broker_pid", "child_pid")
        incomplete = False
        for pidkey in targets:
            fd = handles.get(pidkey)
            if fd is None:
                continue
            try:
                signal.pidfd_send_signal(fd, signal.SIGKILL)
            except OSError as exc:
                # ESRCH means the target is already gone, which is the goal. Anything
                # else means it was not reclaimed and may still be running.
                if exc.errno != errno.ESRCH:
                    print(f"{a.name}: could not signal the {pidkey.replace('_pid', '')}: "
                          f"{exc.strerror}; the session may still be running",
                          file=sys.stderr)
                    incomplete = True
        for fd in handles.values():
            os.close(fd)
        if incomplete:
            # Exit 5 rather than 4: the preflight passed and cleanup began, so a target
            # may already have been signalled. 4 promises that none was sent, and this
            # must not borrow that promise -- nor report a clean kill.
            sys.exit(5)
        # Drop the state only if it still describes the session this command acted on.
        # A run started under the same name meanwhile owns its own record, and removing
        # it would leave a live session with no state for `screen`/`expect`/`kill` to
        # find. The compare and the delete happen under the session lock, because
        # publishing is what would otherwise slip between them.
        lock = state_lock(a.name)
        if lock is None:
            print(f"{a.name}: could not lock the state to remove it; leaving it in "
                  f"place", file=sys.stderr)
            sys.exit(5)
        try:
            try:
                current = json.loads(stf.read_text())
            except FileNotFoundError:
                current = "gone"            # already removed; nothing to judge
            except (OSError, ValueError):
                # Unreadable or unparseable is not the same as absent: an unknown record
                # is not one to delete.
                current = None
            if current == "gone" or (current and current.get("token") == st.get("token")):
                stf.unlink(missing_ok=True)
            elif current is None:
                print(f"{a.name}: the state file is unreadable; leaving it in place",
                      file=sys.stderr)
                sys.exit(5)
        finally:
            release_lock(lock)
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
