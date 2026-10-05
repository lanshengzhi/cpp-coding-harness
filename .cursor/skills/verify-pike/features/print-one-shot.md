# Print one-shot

`--print` (or any non-TTY input/output) selects the one-shot text frontend: it
processes a single prompt and writes only the final assistant text to stdout,
exiting 0. A terminal or abort outcome writes a diagnostic to stderr and exits
1. With no model configured, the offline failure is deterministic.

## Sub-features

- `print-success-shape` — stdout carries only the final assistant text, exit 0.
- `print-offline-failure` — no credential yields the deterministic provider
  error on stderr, exit 1.
- `print-stdin` — a prompt may come from stdin instead of an argument.

## How to get to it (user POV)

- Run `pike --print "question"` or `printf 'question' | pike --print`.

## Driving it with pike_tui.py

Preconditions:

- Doctor passes; disposable `HOME`/`XDG_CONFIG_HOME` exported.
- This surface uses a plain subprocess, not the PTY harness.

- **Offline failure.** `out="$(env HOME="$HOME" XDG_CONFIG_HOME="$XDG_CONFIG_HOME"
  build/release/pike --print ping </dev/null 2>err.txt)"; echo "exit=$?"`. Expect
  exit 1, empty stdout, and `err.txt` containing `Unknown provider: unknown`.
- **Stdin prompt.** `printf 'hello' | env HOME="$HOME" XDG_CONFIG_HOME="$XDG_CONFIG_HOME"
  build/release/pike --print 2>err.txt`; offline it fails the same deterministic
  way.
- **Success shape (configured provider).** With a valid credential/model, the
  same command exits 0 and stdout holds only the assistant text. Requires live
  credentials and user authorization; out of scope for the offline default.
- **Proof.** Capture stdout, stderr, and exit code into
  `$EVIDENCE_DIR/print-*.txt`. The offline proof shows the deterministic
  error and exit code; do not present it as a successful model call.

## Gotchas

- `--print` does not dispatch slash commands; `--print "/help"` is sent as a
  literal prompt.
- A run with no prompt prints nothing and exits 0.
- `--print` exits 143 on SIGTERM and 129 on SIGHUP; match on the documented
  exit codes, not on a generic nonzero. The offline provider failure is exit 1;
  do not lump it in with the signal exits.
- The deterministic offline error string is the contract the CI relocation
  smoke relies on (`scripts/ci/release-artifact-smoke.sh`); do not assert a
  looser message.
