---
status: accepted
---

# Align tool-execution rendering with pi v0.87.1

Pike's interactive TUI renders one model tool call in a padded box whose background transitions
pending → success/error. Today that single component is also the whole presentation policy: it
branches on the tool name, prints the raw arguments JSON it received, and applies one uniform
"first 5 logical lines" fold to both the arguments and the output. pi v0.87.1 (`f07218c4`, the
frozen baseline for this module) instead resolves a per-tool render-call / render-result pair and
gives each tool its own collapse rule, its own warning lines, and its own title shape.

This is a per-module product decision, not a restoration of pi parity as architecture authority:
[ADR 0053](0053-replace-pi-parity-authority-with-the-product-architecture-contract.md) remains the
Product Architecture Contract, and [ADR
0060](0060-advance-the-cch-coding-agent-baseline-to-pi-v0-87-1-and-adopt-transcript-system-messages.md)
remains the baseline record. pi `v0.87.1` is the comparison baseline for this module's supported
presentation, and every string below is taken verbatim from that checkout. It implements
[#822](https://github.com/lanshengzhi/cpp-coding-harness/issues/822).

## Context

Four divergences are in play.

**A raw arguments block per call.** Every tool call prints the arguments JSON exactly as it
arrived — a single unformatted line for every tool, including `read` and `write`, whose `path`,
`offset`, `limit`, and `content` arguments are all the same shape on screen. A `read` title names
the tool and the path but carries no line range, so a reader cannot tell which slice of a long
file the model actually saw. `bash` names the command but not its `timeout`.

**One collapse rule for every tool.** `ToolExecutionComponent` folds any text longer than
`kCollapsedLogicalLines` (5) or `kCollapsedPayloadBytes` (2048) down to its first five logical
lines, appends `… (<key> to expand)`, and applies that to both the arguments and the output. pi's
rules disagree per tool: `read` shows nothing when collapsed, `write` shows a 10-line content
preview with a total, `bash` keeps the **last** 5 *visual* lines (width-aware, so a wrapped line
counts for what it occupies on screen, not for what it is in the buffer), and the no-renderer
fallback shows 10 lines.

**Presentation markers baked into model-visible content.** `read` appends
`[output truncated]` followed by `[Output truncated. Use offset=N to continue.]`; `bash` prefixes
`exit_code=<N>` plus ` truncated=true` and, when truncated, prepends
`[output truncated, showing last N bytes]` or `[output capped at execution layer, showing last N bytes]`
and leaks its spill filename into the text. Every one of these is a terminal fact baked into the
string the model reads and the session file stores, which couples the model contract to a
presentation policy and makes a rendering change a model-contract change.

**A single spill location.** The complete `bash` output is spilled as
`bash-output-<timestamp>.txt` in the working directory — a file the model never asked for, in the
directory the user is working in.

## The Tool Renderer is an application-layer registry

A **Tool Renderer** (the `CONTEXT.md` entry) is the application-layer presentation of one tool's
call and result: a render-call / render-result pair keyed by tool name in the interactive TUI,
never a property of the headless `Tool` definition. The interactive TUI application layer owns a
registry mapping tool name to that pair, plus one fallback renderer. `ToolExecutionComponent`
resolves the registry once per tool execution and asks the resolved renderer for the title and the
body; the `tool_name_ == "read"` / `"write"` / `"bash"` / `"edit"` branching is deleted. A tool
name with no registered renderer takes the fallback: bold tool name, a blank line, the arguments
as pretty-printed JSON indented by 2 spaces, then the output folded at 10 lines with a
remaining-lines hint.

The render context handed to each renderer is pi's `ToolRenderContext` in substance: the arguments
JSON, `argsComplete`, `executionStarted`, `expanded`, `isPartial`, `isError`, the working
directory, the previous component for slot reuse, a per-execution state object shared by the two
halves (which is how `bash`'s call half starts the clock its result half reports), and
`invalidate`. A renderer returns text the host lays out; it does not own components.

**The registry is never a property of the `Tool` definition.** ADR 0053's first contract clause
forbids the headless core from depending on any frontend, and the manifest enforces it both as a
target edge and as an include edge. A `ToolDefinition` in the agent layer that carried
`renderCall` / `renderResult` would put frontend headers on the headless core's own contract
surface, so the registry lives entirely in the TUI application layer and nothing under `src/agent/`
references it. No new dependency and no new Owner Package ship with it: the registry is private
implementation of the existing application-layer target, and the renderers draw on
`cch_tui`'s existing public interface only.

Retained from the current component, unchanged: inline result image rendering, the
pending/error/success background transition, and the `app.tools.expand` keybinding integration.
The expand hint is now uniform in pi's format — `... (N more lines, <key> to expand)` — with `<key>`
resolved once from the keybinding registry and rendered as `Unbound` when nothing is bound.

## Model-visible contract change

`read` and `bash` results change. The renderer draws every warning from `details`; the model reads
only `content`. Three things move and two things stay, and the distinction is the whole point:
**model-actionable continuation hints stay in `content`; presentational `[Truncated: ...]` warning
lines leave it.** A `[Truncated: ...]` line is a terminal fact about a fold the model cannot see.

### `read` — head truncation

`content` is a single text block. The `offset` arithmetic is 1-indexed on input and 0-indexed on
the buffer: `start = max(0, offset - 1)`, `startLineDisplay = start + 1`. An `offset` past the end
is still an error, worded `Offset <offset> is beyond end of file (<N> lines total)`. When a `limit`
is given the slice is `min(start + limit, totalFileLines) - start` lines long, and truncation runs
on that slice. There are exactly five shapes, and they are mutually exclusive:

1. **First line alone exceeds the byte limit.** `content` is *only* the notice — no file text at
   all: `[Line <startLineDisplay> is <firstLineSize>, exceeds <maxBytesSize> limit. Use bash: sed -n
   '<startLineDisplay>p' <path> | head -c <maxBytes>]` (`<path>` is the model-supplied argument,
   not the resolved absolute path; the cap is the numeric `51200`, not a formatted size). For
   example: `[Line 1 is 60.0KB, exceeds 50.0KB limit. Use bash: sed -n '1p' /tmp/big.log | head -c 51200]`.
   `details.truncation` is present.
2. **Truncated by the line limit.** The truncated text, then
   `[Showing lines <startLineDisplay>-<endLineDisplay> of <totalFileLines>. Use offset=<nextOffset> to continue.]`
   on its own `\n\n`-separated line, where `endLineDisplay = startLineDisplay + outputLines - 1`
   and `nextOffset = endLineDisplay + 1`, and `<totalFileLines>` counts the **whole file**, not the
   selected slice. For example: `[Showing lines 1-2000 of 5231. Use offset=2001 to continue.]`.
3. **Truncated by the byte limit.** The same line with the limit named in the middle:
   `[Showing lines 1-812 of 5231 (50.0KB limit). Use offset=813 to continue.]`.
4. **A user `limit` stopped early but the file has more.** Only when truncation did *not* occur:
   `[<remaining> more lines in file. Use offset=<nextOffset> to continue.]`, where
   `remaining = totalFileLines - (start + userLimitedLines)` and `nextOffset = start +
   userLimitedLines + 1`. For example: `[119 more lines in file. Use offset=101 to continue.]`.
   This is new behavior; today a limited read says nothing about the rest of the file.
5. **Otherwise.** The text as read, with no trailing notice of any kind.

`details` is `{ "truncation": … }` in cases 1–3 and **absent** in cases 4 and 5, even though case
4's text carries a continuation hint. The `truncation` object carries `truncated`, `truncatedBy`
(`"lines"`, `"bytes"`, or `null`), `outputLines`, `totalLines`, `maxLines`, `maxBytes`,
`firstLineExceedsLimit`, and `lastLinePartial`. The renderer, not the tool, turns those into
`[Truncated: showing <outputLines> of <totalLines> lines (<maxLines> line limit)]`,
`[Truncated: <outputLines> lines shown (<maxBytes> limit)]`, or
`[First line exceeds <maxBytes> limit]` — three distinct texts, never collapsed into one.

Deleted from `read`'s `content`: the `[output truncated]` line and
`[Output truncated. Use offset=N to continue.]`.

### `bash` — tail truncation

`content` is the clean redacted output the model may see — the **tail** at most 2000 lines or
50.0KB, never a marker. Clean exit with no output is exactly `(no output)`.

When truncation happened, `details` is `{ "truncation": …, "fullOutputPath": "<path>" }` (the same
`truncation` shape as `read`), and `content` gains one `\n\n`-separated summary line, one of three
forms, with `startLine = totalLines - outputLines + 1` and `endLine = totalLines`:

- `[Showing lines 3232-5231 of 5231. Full output: /tmp/<prefix>-<id>.log]` — truncated by the line limit.
- `[Showing lines 1-812 of 5231 (50.0KB limit). Full output: /tmp/<prefix>-<id>.log]` — truncated by the byte limit.
- `[Showing last 50.0KB of line 412 (line is 78.0KB). Full output: /tmp/<prefix>-<id>.log]` — the final line itself was cut.

The path is embedded verbatim: no `~` shortening, and it is the same string `details.fullOutputPath`
carries and the same string the renderer later prints in its `Full output: …` warning. The spill
moves to the OS temporary directory, constructed as `<tmpdir>/<prefix>-<unique-id>.log`; the ADR
fixes the directory and that construction, and the `<prefix>` is pike's own rather than pi's.

Every failure is an **error** result, and the text is the output followed by `\n\n` and the status
line — or the status line alone when the output is empty. The four status strings, and the order
in which they are decided:

| Condition | Status text |
|---|---|
| aborted | `Command aborted` |
| timeout | `Command timed out after <N> seconds` |
| no exit code | `Command terminated without an exit code` |
| non-zero exit | `Command exited with code <N>` |

Abort and timeout are decided on the execution error, abort first; the remaining two on the exit
code, "no exit code" first. On the abort and timeout paths the `(no output)` default is
**suppressed**, so an aborted or timed-out command that printed nothing yields exactly
`Command aborted` or `Command timed out after <N> seconds`. On the two exit-code paths the default
applies, so a silent non-zero command reports `(no output)` followed by
`Command exited with code <N>`.

Deleted from `bash`'s `content`: the `exit_code=<N>` prefix, the ` truncated=true` suffix, the
`[output truncated, showing last N bytes]` and `[output capped at execution layer, showing last N bytes]`
markers, and the spill filename as free text.

**User Bash is not in scope here.** The `BashExecutionComponent` that renders the user's own
commands keeps its current output, exit-code, and truncation wording. The `Command exited with
code N` and `[Output truncated. Full output: …]` text that reaches the model from a User Bash
turn is a different path and stays as it is.

**`write` and `edit` keep their model contract.** `write` returns its result text unchanged; the
renderer stops re-printing a successful write's content because the call half already showed the
preview. `edit` keeps `details.diff`; the renderer drops the arguments block and keeps the diff on
the result.

### A consequence for the renderer

Because the `[Showing … Full output: <path>]` line is in `content`, a collapsed `bash` result that
was truncated shows the path twice. pi resolves this by stripping the trailing `fullOutputPath`
footer from the *displayed* text when the result has settled, leaving the path to the renderer's
own warning line. pike does the same, on the render side only: `content` keeps its summary line
unconditionally, and it is the fold that drops it.

## Session compatibility: no migration, by choice

Session files written before this change carry the old markers baked into their `content` and have
no `details.truncation`. That is tolerated, not fixed. A resumed old session renders its
`[output truncated]` and `[Output truncated. Use offset=N to continue.]` text as ordinary text
inside the folded output, and no second warning line is drawn, because the new renderers draw
warnings **only** from `details` and an old entry has none. The transcript is the record of truth
(ADR 0060), so a resumed old session also re-sends those historical tool results to the model
exactly as recorded, old markers included. Rewriting them would be a content migration dressed up
as compatibility; the standing "clean end state over migrations and fallback reads" principle
applies here as it does to the compat layer, and pi's own wording is not retro-fitted onto
history.

## Explicit deferrals

Each of these is decided out, with its reason, and none carries a placeholder surface.

- **Syntax highlighting for `read` results and `write` previews.** The repository has no
  highlighting implementation; adopting one means a new dependency. Recorded as an Intentional
  Divergence, raisable as its own proposal.
- **Live `Elapsed` ticking for `bash`.** Only the static `Took <duration>` line ships. A ticking
  `Elapsed` requires attaching a frame-ticker lifecycle to the tool-execution component, which
  this change does not take on. Duration formatting is pi's: `0.4s` under a minute, `1m 5s` under
  an hour, `1h 2m 3s` above, with the label switching on the settle.
- **Mouse-click expand.** `cch_tui` has no mouse-input seam. The expand affordance is the
  `app.tools.expand` keybinding.
- **Pre-execution diff preview for `edit`.** Computing the diff during render-call means file IO
  in the presentation layer; the layering cost exceeds the benefit. `edit` keeps its result-side
  diff.
- **The pi-docs compact classification for `read`.** pi titles a read of its *own* `README.md`,
  `docs/**`, or `examples/**` as `read docs <relative path>`. pike ships no runtime docs tree, so
  the classification would be dead code. The `SKILL.md` → `[skill] <directory name>` and
  `AGENTS.md` / `CLAUDE.md` (any case variant) → `read resource <path>` classifications ship.
- **`grep` / `find` / `ls` and their renderers.** Already Deferred by [ADR
  0034](0034-own-the-scoped-pi-agent-core-agent-and-agent-turn-capabilities.md) with no
  placeholder; this change does not resurrect them.
- **The `powershell` renderer.** Outside the Supported Platform. pi builds bash and powershell
  from one shared shell-renderer factory; pike instantiates that factory once, for the `"$"`
  prompt.
- **Old-session migration.** A tolerance decision, not a one-shot import. See above.

## Consequences

- **The architecture gate still passes.** The change touches the gated agent layer (the `read` and
  `bash` tool implementations) and adds a TUI-only module. `ctest --preset vcpkg -L architecture`
  must pass unchanged, and the ADR's registry placement is the reason the second half adds no
  frontend edge to the headless owner.
- **Two seams prove the alignment.** Seam 1 is the model contract: the tool-factory tests assert
  the exact `read` and `bash` `content` strings above and the `details` structure, each with a
  negative twin, because the strings are the contract. Seam 2 is terminal presentation: the
  component-level fixed-width rendering tests drive `ToolExecutionComponent` and assert the full
  rendered screen line for all four tools, the fallback, the compact classifications, collapse and
  expand, the warning lines, and the hyperlink gate — plus a screen-level golden showing the four
  renderers and the fallback composing in one session view.
- **Existing presentation assertions move.** The committed screen goldens, the `read saved.txt`
  interactive assertion, the `Unbound to expand` assertion, and the message-pipeline render
  snapshot all encode the old presentation and are updated against the pi source rather than
  against whatever the code happens to emit. A regenerated golden is a check, not evidence.
- **The old agent-layer truncation assertions are prior art, not a contract.** Tests that assert
  `[output truncated]`, `[Output truncated. …]`, or the `exit_code=` prefix were pinning the old
  model contract and are updated with it.

## References

- [ADR 0034](0034-own-the-scoped-pi-agent-core-agent-and-agent-turn-capabilities.md) (built-in tool
  set; `grep` / `find` / `ls` Deferred), [ADR
  0036](0036-own-the-scoped-pi-coding-agent-application-layer-capabilities-for-the-three-provider-paths.md)
  (tool-execution is a Supported application capability), [ADR
  0053](0053-replace-pi-parity-authority-with-the-product-architecture-contract.md) (Product
  Architecture Contract; the headless core reaches no frontend), [ADR
  0060](0060-advance-the-cch-coding-agent-baseline-to-pi-v0-87-1-and-adopt-transcript-system-messages.md)
  (the `v0.87.1` baseline and the transcript-as-record rule).
- Baseline `f07218c4d4bbc12bef056a7058c3dd49dfe41abe` (tag `v0.87.1`):
  `packages/coding-agent/src/core/tools/renderers/{index,render-utils,read,write,edit,bash}.ts`,
  `packages/coding-agent/src/core/tools/{read,bash,truncate,output-accumulator}.ts`,
  `packages/coding-agent/src/modes/interactive/components/{tool-execution,visual-truncate,keybinding-hints}.ts`.
