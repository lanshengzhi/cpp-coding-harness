# Session persistence

A session-backed run (no `--no-session`) writes the conversation transcript to
the Agent Config Directory under a workspace-keyed `sessions/` subdirectory. The
file is a JSON-lines stream of typed entries and is created on the first user or
assistant message.

## Sub-features

- `session-create` — a new session's transcript file is created after the first
  message.
- `transcript-entries` — the file records the session header, thinking-level,
  and the user and assistant/error messages.
- `workspace-key` — the file lives under `sessions/<workspace-key>/` derived
  from the working directory.

## How to get to it (user POV)

- Run `pike` with a prompt in a directory; the session persists automatically
  (no `--no-session`).

## Driving it with pike_tui.py

Preconditions:

- Doctor passes; disposable `HOME`/`XDG_CONFIG_HOME` exported.
- A workspace directory `$VERIFY_ROOT/ws` exists and is the `--cwd`.

- **Start a session-backed run.** Spawn **without** `--no-session`:
  `"$HELPER" spawn --name sess --binary "$BIN"
  --cwd "$VERIFY_ROOT/ws" --env "HOME=$HOME" --env "XDG_CONFIG_HOME=$XDG_CONFIG_HOME"`.
  The TUI opens with a normal prompt (no `--no-session` marker).
- **Send a prompt.** `"$HELPER" send --name sess --text "persist me"`, then
  `"$HELPER" send --name sess --key enter`. Expect the provider error:
  `"$HELPER" expect --name sess --text "Error: Unknown provider"`.
- **Verify the side effect.** `find "$XDG_CONFIG_HOME/pike/agent/sessions" -name
  '*.jsonl'` returns one new file under a workspace-keyed directory. Read it:
  the first line is `{"type":"session",…}`, followed by a
  `thinking_level_change` and `message` entries. The per-message fields are
  nested: each `message` entry carries a `message` object whose `role` is
  `"user"` (with the sent text) or `"assistant"` (with `stopReason:"error"` and
  `errorMessage:"Unknown provider: unknown"`). Do not read `role`/`stopReason`
  at the top level; they live one level down.
- **Proof.** The screen snapshot after submission, the path of the new `.jsonl`,
  and its parsed entries together prove persistence. Capture the file path and
  first entries into ``$EVIDENCE_DIR/session-transcript.txt``.

## Gotchas

- `--no-session` runs leave **no** transcript; do not use it when proving
  persistence.
- The session file is created lazily — only after the first user or assistant
  message, not at startup.
- The transcript contains the system preamble and tool schemas; it is a
  sensitive local file even where message content is redacted. Keep it inside
  the disposable root.
- The workspace key is the process startup cwd, captured before any in-app
  `cd`. Launching pike from one directory and `cd`-ing inside the same shell
  session does **not** re-key the session; the transcript lands under the
  original directory's key.
