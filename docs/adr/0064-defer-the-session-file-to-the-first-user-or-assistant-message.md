---
status: accepted
---

# Defer the session file to the first user or assistant message

[ADR 0003](0003-store-default-sessions-in-agent-config-directory.md) published the session file
eagerly at creation: the header, the initial `model_change` and `thinking_level_change` entries,
and the initial System Message all landed on disk before the user typed anything. Every launch —
including `/resume` browsing, accidental `exit` submissions, and abandoned starts — produced a
session file whose only message is the system prompt. Session Discovery deliberately keeps pi's
read semantics (any `message` entry counts toward the row, but the preview text comes only from
user/assistant messages), so these empty launches crowded `/resume` as `(no messages)` entries.
pi avoids this by deferring the whole file: `SessionManager._persist` buffers entries in memory
and creates the file only once the session contains a user or assistant message
(`_hasConversation`). Earlier pi releases waited for the first assistant message; pi #10000 moved
the trigger to include the user message so the prompt stays on disk when the first turn never
completes.

## Considered options

- Keep eager publication and filter empty sessions in discovery: rejected because it forks the
  discovery read semantics from pi (pike would skip files pi lists), and because it treats the
  symptom — the store would still accumulate content-free files that every future tool must
  remember to filter.
- Keep the frozen v0.87.1 baseline's trigger (first assistant message): rejected because ADR
  0003's durability rationale stands — a completed user prompt must survive a provider failure
  that prevents any assistant message from being recorded. Upstream reached the same conclusion
  in pi #10000 and moved the trigger.
- Defer the file to the first user message only: rejected because it still diverges from pi for
  a transcript whose first conversational entry is an assistant message (a restored session can
  append a reply before any new prompt); pi flushes there, and the user-or-assistant check costs
  nothing over the user-only check.
- Adopt pi v0.99.1's `_hasConversation` trigger verbatim (first user or assistant message):
  adopted. The transcript that matters begins with the first conversational turn; everything
  earlier is assembly bookkeeping.

## Decision

`SessionJournal` gains a deferred mode: `create_deferred` validates the path at
publication (the symlink-refusing parent walk, private directory creation, and the
not-already-existing check all stay at publication time) but writes nothing. The
exclusive create itself happens at the flush — `flush_new`'s single
`write_new_file_exclusive` — matching pi, which likewise only checks exclusivity at
its delayed first write (`openSync(path, "wx")`); two sessions that reserve the same
path are both told success until the first flush lets exactly one of them land. `JsonlSessionStore::create_new` buffers the
serialized header and every subsequent entry in memory; the store's live `SessionTree` records
entries exactly as before, so session behavior is unchanged in memory. The first appended **user
or assistant message** — pi v0.99.1's `_hasConversation` gate verbatim — triggers `flush_new`:
one exclusive create that writes the header and every buffered
entry in append order, then the journal appends normally. A failed flush removes the partial
file, rejects the triggering message (it is not recorded), and keeps the earlier buffered entries
pending so the next flush-triggering append retries the batch. Forks (`create_from_entries`) and resumes
(`open_existing`) keep eager, already-flushed journals. Bash messages do not trigger the flush,
matching pi, where the `bashExecution` role is neither user nor assistant and
`appendBashExecutionMessage` alone never persists a session either.

Every transcript system message — the initial prompt-and-loadout message and each later
section-diff — now carries a real epoch-millis timestamp instead of `0`, matching pi, which
mints both through the same `_preparePromptAndToolLoadout` path with `Date.now()`.

Because the file no longer exists for an unprompted session, tests and tools that need the
recorded entries observe the store's live tree; `AgentSession::session_entries()` exposes it as
the pi `SessionManager.getEntries()` equivalent.

## Consequences

- An empty session leaves no transcript: `/resume` never lists it as `(no messages)`. The
  per-launch directories are still created privately at publication; empty directories are
  invisible to discovery, which lists files only.
- This decision tracks pi v0.99.1 for the session-file lifecycle ahead of the frozen v0.87.1
  fixture baseline ([ADR 0060](0060-advance-the-cch-coding-agent-baseline-to-pi-v0-87-1-and-adopt-transcript-system-messages.md));
  the fork surface's "not saved yet" error string already carries the v0.99.1 wording for the
  same reason. The v0.87.1 difference is confined to the flush trigger (assistant-only there).
- pi parity narrows to one observable difference: pi shows an unprompted running session in its
  in-memory session list as `(no messages)`; pike's list is file-backed, so an unprompted session
  simply has no row yet. The durable end state is identical.
- pi's own "not saved yet" surfaces map onto the deferred state: an unprompted session has
  no file to fork from, exactly as in pi before its first flush, and the fork surface reports
  it with pi's verbatim "This session has not been saved yet. Send a message before cloning or
  forking it." (the earlier pike phrasing, "Wait for the first assistant response", described
  the v0.87.1 flush trigger, not pike's: here the file lands with the first user or assistant
  message).
- The failed-flush retry contract is a store-level guarantee: at runtime the session event
  commitment latches a persistence failure for the rest of the run, so the retry batch is
  exercised through the store, not the product prompt path.
- ADR 0003's consequence bullets about eager file creation and about pi's delayed first flush are
  superseded by this decision; its ownership, layout, privacy, and no-migration decisions are
  unchanged.
