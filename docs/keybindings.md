# Native TUI keybindings

The Native TUI keybinding format is compatible with pi parity baseline
`864b35c`. The harness performs one startup read of
`<Agent Config Directory>/keybindings.json` (normally
`~/.cpp-harness/agent/keybindings.json`). `CCH_CODING_AGENT_DIR` changes the
Agent Config Directory. Discovery never reads `~/.pi`, `.pi`, or any other pi
state directory.

The production CLI does not select the Native TUI yet; this configuration slice
is assembled by the Native TUI integration work.

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

`tui.input.copy` is a known baseline ID but is not assembled because the current
reusable editor has no selection/copy capability.

Application (`app.*`) actions are registered only by a frontend that assembles
the corresponding capability. A configured known-but-unassembled application
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
