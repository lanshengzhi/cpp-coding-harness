# Native TUI keybindings

The Native TUI keybinding format is compatible with pi parity baseline
`83114817` (re-pinned from `864b35c` by ADR 0035 after the [#332](https://github.com/lanshengzhi/cpp-coding-harness/issues/332)
toolkit inventory verified the `tui.*` action table and key grammar at the frozen
baseline). The harness performs one startup read of
`<Agent Config Directory>/keybindings.json` (normally
`~/.pi/agent/keybindings.json`). `PI_CODING_AGENT_DIR` changes the Agent Config
Directory. Discovery reads only that resolved user-level root and never scans a
project-local `.pi` directory.

On supported Linux/macOS, the production CLI loads this file when interactive
stdin/stdout selects the Native TUI. Print and unsupported-platform startup do
not load a Native TUI keybinding registry.

## Format

Each namespaced action accepts one key string, an array of alternative keys, or
an empty array to unbind the action:

```json
{
  "tui.editor.cursorUp": ["up", "ctrl+p"],
  "tui.input.newLine": "ctrl+j",
  "tui.input.submit": ["enter", "ctrl+enter"],
  "tui.editor.jumpForward": []
}
```

Keys use `modifier+key`. Modifiers are `ctrl`, `shift`, and `alt`, in any
combination. Keys are letters `a-z`, digits `0-9`, `escape`/`esc`,
`enter`/`return`, `tab`, `space`, `backspace`, `delete`, `insert`, `clear`,
`home`, `end`, `pageUp`, `pageDown`, arrow keys, `f1`-`f12`, and the baseline
ASCII symbols. Aliases and modifier order are canonicalized; repeated
alternatives are removed while preserving the first occurrence. macOS help
renders `alt` as `option` without changing configuration syntax.

## Effective reusable actions

These are the baseline-compatible actions currently implemented end to end in
`cch_tui`:

| Action ID | Default keys |
|---|---|
| `tui.editor.cursorUp` | `up` |
| `tui.editor.cursorDown` | `down` |
| `tui.editor.cursorLeft` | `left`, `ctrl+b` |
| `tui.editor.cursorRight` | `right`, `ctrl+f` |
| `tui.editor.cursorWordLeft` | `alt+left`, `ctrl+left`, `alt+b` |
| `tui.editor.cursorWordRight` | `alt+right`, `ctrl+right`, `alt+f` |
| `tui.editor.cursorLineStart` | `home`, `ctrl+a` |
| `tui.editor.cursorLineEnd` | `end`, `ctrl+e` |
| `tui.editor.jumpForward` | `ctrl+]` |
| `tui.editor.jumpBackward` | `ctrl+alt+]` |
| `tui.editor.pageUp` | `pageUp` |
| `tui.editor.pageDown` | `pageDown` |
| `tui.editor.deleteCharBackward` | `backspace` |
| `tui.editor.deleteCharForward` | `delete`, `ctrl+d` |
| `tui.editor.deleteWordBackward` | `ctrl+w`, `alt+backspace` |
| `tui.editor.deleteWordForward` | `alt+d`, `alt+delete` |
| `tui.editor.deleteToLineStart` | `ctrl+u` |
| `tui.editor.deleteToLineEnd` | `ctrl+k` |
| `tui.editor.yank` | `ctrl+y` |
| `tui.editor.yankPop` | `alt+y` |
| `tui.editor.undo` | `ctrl+-` |
| `tui.input.newLine` | `shift+enter`, `ctrl+j` |
| `tui.input.submit` | `enter` |
| `tui.input.tab` | `tab` |
| `tui.select.up` | `up` |
| `tui.select.down` | `down` |
| `tui.select.pageUp` | `pageUp` |
| `tui.select.pageDown` | `pageDown` |
| `tui.select.confirm` | `enter` |
| `tui.select.cancel` | `escape`, `ctrl+c` |

The baseline Settings List also treats unmodified `space` as an intrinsic
activation key. It remains available when `tui.select.confirm` is remapped and
is shown in that component's rendered hint, matching pi's Settings List
semantics rather than inventing a configurable action ID.

`tui.input.copy` and the six `tui.altScreen.*` actions (`pageUp`, `pageDown`,
`previousPrompt`, `nextPrompt`, `top`, `bottom`) are known baseline IDs but are
not assembled: their only active behavior is alt-screen viewport scrolling and
selection copy, and the alt-screen half is a Deferred Capability with no
placeholder surface (ADR 0035). Entries for them are diagnosed as
known-but-unassembled and skipped, never no-op bindings; the editor's
pass-through behavior for a copy-mapped key (pi lets the parent handle exit/clear)
holds without the action existing.

Application (`app.*`) actions are registered only by a frontend that assembles
the corresponding capability. The app layer adopts pi's full 42-action
`AppKeybindings` catalog (`pi:packages/coding-agent/src/core/keybindings.ts` at
`83114817`, ADR 0036). The Native TUI composition assembles pi's default bound
set in the main editor:

| Action ID | Default keys | Active-run behavior |
|---|---|---|
| `app.interrupt` | `escape` | Restore pending input, then request one ordinary abort lifecycle. |
| `app.clear` | `ctrl+c` | Clear the editor. |
| `app.exit` | `ctrl+d` | Exit when the editor is empty and restore the terminal. |
| `app.suspend` | `ctrl+z` (`[]` on native Windows) | Suspend to background (SIGTSTP + keep-alive). |
| `app.thinking.cycle` | `shift+tab` | Cycle thinking level. |
| `app.model.cycleForward` | `ctrl+p` | Cycle to the next model. |
| `app.model.cycleBackward` | `shift+ctrl+p` | Cycle to the previous model. |
| `app.model.select` | `ctrl+l` | Open the model selector. |
| `app.tools.expand` | `ctrl+o` | Toggle expanded tool output. |
| `app.thinking.toggle` | `ctrl+t` | Toggle expanded thinking blocks (persists the `hideThinkingBlock` user setting, pi `toggleThinkingBlockVisibility`). |
| `app.editor.external` | `ctrl+g` | Open the external editor (env-only command source). |
| `app.message.copy` | `ctrl+x` | Copy the last agent message to the clipboard. |
| `app.message.followUp` | `alt+enter` | Admit editor text to the Agent Session follow-up queue. |
| `app.message.dequeue` | `alt+up` | Restore steering, then follow-up, then unsent editor text. |

`app.clipboard.pasteImage` (`ctrl+v`, or `alt+v` on native Windows) is registered
only when the assembling host injects an asynchronous clipboard reader; the
production CLI does not advertise an unassembled clipboard action. `app.session.*`
(`new`, `tree`, `fork`, `resume`) stays recognized-but-unbound in the main editor
(pi ships `defaultKeys: []`); the session selector, tree selector, and
scoped-models selector bind their scoped action sets (`app.session.*`
filter/rename/delete, `app.tree.*`, `app.models.*`) inside those components, and
`/hotkeys` plus the header hints render the assembled subset only.

Ordinary `tui.input.submit` starts a prompt while idle and admits steering input
while a run is active. Alt+Enter acts like ordinary submit while idle. The
default queue bindings do not conflict with `shift+enter` or `ctrl+j`, which
remain `tui.input.newLine`. Accepted queued input leaves the editor and appears
in the pending display. Capacity rejection restores the submitted editor text,
leaves both Agent-owned queues and active work unchanged, and presents a
redact-before-bound diagnostic.

While autocomplete is open, a key that is also the effective
`tui.select.cancel` binding dismisses suggestions before interruption is
eligible; a distinct configured interrupt key still aborts active work. While
Agent Session work is active, repeated interrupt input coalesces into exactly
one request for that run's ordinary abort lifecycle. An active Agent run is
interrupted before an overlapping User Bash command; once the run is idle, the
next interrupt cancels User Bash, retaining its partial output as a cancelled
execution. When no work is active but the editor holds an unsubmitted Bash-mode
submission, interrupt clears the editor; otherwise it falls through to the
baseline editor behavior. A configured known-but-unassembled application
ID is diagnosed and skipped; it never creates a no-op binding or help entry.
Platform defaults are resolved at concrete registration: for example,
`app.suspend` defaults to `ctrl+z` on Linux and macOS, while native Windows has
no job-control binding and help reports `Unavailable on native Windows`.

## Deterministic resolution and diagnostics

- A valid user entry replaces all defaults for that action. An empty array
  explicitly unbinds it.
- Defaults on other actions remain intact. Editor, selector, and application
  contexts intentionally reuse keys, matching the pinned baseline.
- If multiple user actions claim one key, both contextual claims are retained,
  a `conflicting_user_key` diagnostic is emitted, and dispatch chooses the
  first action in the active component order. Autocomplete evaluates cancel,
  selection movement, then tab; the editor evaluates tab, editing/navigation,
  newline, then submit; selection/settings lists evaluate movement, paging,
  confirm, then cancel. A cancellable loader evaluates only cancel. Callers
  resolving their own context pass an explicit candidate order to the registry.
- An invalid entry is rejected atomically and that action keeps its defaults.
  Unknown IDs and known but unassembled IDs are skipped.
- A missing file silently uses defaults. An unreadable, malformed, or non-object
  automatic file also uses defaults and emits a diagnostic.
- Diagnostics are secret-redacted before truncation, limited to 1,024 bytes per
  message/path and 64 records. Stable codes distinguish unavailable files,
  malformed documents/values, invalid keys, conflicts, unknown actions,
  unavailable actions, and truncation.

Dispatch, hotkey help, and rendered key hints all consume the same immutable
effective registry. There is no startup watcher or generalized hot reload.
