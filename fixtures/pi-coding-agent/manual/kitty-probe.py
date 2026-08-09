#!/usr/bin/env python3
"""Deterministic Kitty keyboard-protocol negotiation probe for cpp_harness.

Scenario 1 (kitty-capable terminal):
  - capture the startup byte stream; assert the app pushes ESC[>7u and
    queries ESC[?u ESC[c
  - answer the query with kitty flags ESC[?3u + device attributes ESC[?1;2c
  - wait for the main-screen render (header hint line)
  - inject kitty-encoded modified keys (ESC[1;5u = Ctrl+Up) and text
  - assert the app stays alive and keeps rendering (no misparse/crash)

Scenario 2 (legacy terminal, degradation):
  - same startup, but never answer the kitty query
  - assert the app still reaches the main screen (graceful degradation)
"""
import os, pty, select, sys, time, signal, fcntl, termios, struct

BIN = "/home/lansy/Work/github/coding-agent/cpp-coding-harness/build/cpp_harness"
ARGS = [BIN, "--provider", "openai-codex", "--model", "gpt-5.6-terra"]
PUSH = b"\x1b[>7u"
QUERY = b"\x1b[?u\x1b[c"
HEADER_HINT = "escape".encode()
KITTY_CTRL_UP = b"\x1b[1;5u"

def run_scenario(name, answer_kitty):
    pid, fd = pty.fork()
    if pid == 0:
        os.chdir("/tmp")
        os.environ["TERM"] = "xterm-ghostty"
        os.environ["TERM_PROGRAM"] = "ghostty"
        os.execv(BIN, ARGS)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    os.set_blocking(fd, False)
    captured = bytearray()
    answered = False
    main_screen = False
    alive_at_end = False
    deadline = time.time() + 20
    injected = False
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.25)
        if r:
            try:
                data = os.read(fd, 65536)
            except OSError:
                break
            if not data:
                break
            captured += data
            if answer_kitty and not answered:
                if PUSH in bytes(captured) and QUERY in bytes(captured):
                    os.write(fd, b"\x1b[?3u")     # kitty flags response (1|2 = 3)
                    os.write(fd, b"\x1b[?1;2c")   # device attributes
                    answered = True
            if HEADER_HINT in bytes(captured):
                main_screen = True
            if main_screen and not injected:
                # kitty-encoded Ctrl+Up, then plain text + Enter
                os.write(fd, KITTY_CTRL_UP)
                os.write(fd, b"kitty-probe-ok")
                os.write(fd, b"\r")
                injected = True
        if main_screen and injected and time.time() > deadline - 6:
            # let it render the injected input, then check liveness
            r2, _, _ = select.select([fd], [], [], 0.5)
            if r2:
                try:
                    more = os.read(fd, 65536)
                    if more:
                        captured += more
                except OSError:
                    pass
            break
    # liveness: process must not have exited
    wpid, status = os.waitpid(pid, os.WNOHANG)
    if wpid == 0:
        alive_at_end = True
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    os.close(fd)
    raw = bytes(captured)
    checks = {
        "push_emitted": PUSH in raw,
        "query_emitted": QUERY in raw,
        "main_screen_rendered": HEADER_HINT in raw,
        "injected_text_echoed": b"kitty-probe-ok" in raw,
        "still_alive": alive_at_end,
    }
    print(f"=== {name} ===")
    for k, v in checks.items():
        print(f"  {k}: {'PASS' if v else 'FAIL'}")
    ok = all(checks.values())
    # evidence: strip to a readable tail
    tail = raw[-3000:]
    try:
        text = tail.decode("utf-8", errors="replace")
    except Exception:
        text = repr(tail)
    with open(f"/tmp/kitty-probe-{name}.log", "w") as f:
        f.write(text)
    print(f"  evidence tail saved: /tmp/kitty-probe-{name}.log ({len(raw)} bytes captured)")
    print(f"  RESULT: {'PASS' if ok else 'FAIL'}")
    return ok

if __name__ == "__main__":
    r1 = run_scenario("scenario1-kitty", answer_kitty=True)
    r2 = run_scenario("scenario2-legacy", answer_kitty=False)
    print()
    print("OVERALL:", "PASS" if (r1 and r2) else "FAIL")
    sys.exit(0 if (r1 and r2) else 1)
