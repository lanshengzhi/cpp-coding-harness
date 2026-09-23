---
status: accepted
---

# Advance the cch_coding_agent baseline to pi v0.87.1 and adopt the sectioned System Prompt and transcript system messages

The `cch_coding_agent` application layer — the headless Agent Session owner together with the CLI
and Native TUI frontends — is re-pinned from the historical baseline
`83114817c68f5413e4d7ba6d7003ddc511cd31d2` (the `@earendil-works/pi-coding-agent@0.83.0`-correlated
commit, ADR 0036) to upstream tag `v0.87.1` = commit
`f07218c4d4bbc12bef056a7058c3dd49dfe41abe` (`@earendil-works/pi-coding-agent@0.87.1`). It
implements [#771](https://github.com/lanshengzhi/cpp-coding-harness/issues/771).

This is a per-module product decision, not a restoration of pi parity as architecture authority:
[ADR 0053](0053-replace-pi-parity-authority-with-the-product-architecture-contract.md) remains the
Product Architecture Contract, and [ADR 0036](0036-own-the-scoped-pi-coding-agent-application-layer-capabilities-for-the-three-provider-paths.md)
remains the historical record of the `83114817` (`v0.83.0`) phase. `v0.87.1` is the frozen
comparison baseline for this module's supported behavior, exactly as ADR 0024's "explicitly
advance the baseline" rule describes. Every behavior the layer currently claims whose semantics
changed is aligned; capabilities pi gained that this layer does not claim remain Deferred with no
placeholder surface; no new Intentional Divergence is recorded.

## The System Prompt is a sectioned document

`buildSystemPromptSections()` builds pi's ordered named sections and `renderSystemPromptSections()`
renders them; `buildSystemPrompt()` returns the rendered text. `preamble` is untagged; every other
section is rendered as a tag of the same name, and the renderer joins non-empty parts with a blank
line. The default branch produces `preamble`, `tools`, `rules`, `docs`; `addendum`,
`project_context`, `skills`, and `cwd` follow as they apply. The rules section follows pi's builder
exactly (the conditional file-exploration rule, guideline order, the always-lines); the skills
section selects the read-tool or bash wording by whichever read-capable tool is active; `cwd` is
posix-normalized. The identity delta stays confined to the identity line and the documentation
block — the only difference from pi's prompt — and is pinned by the differential prompt goldens
(pi structure with the identity regions swapped) plus the C++-side prompt goldens (the pike text
byte-for-byte).

## System messages are the transcript's prompt record

The AI-owned System Message value carries pi's transcript contract: an ordered set of named
`sections`, where a section is either a rendered value or an explicit removal, plus the tool
loadout as added tools and removed tool references. The value stays free of provider and
serialization types; the native session format serializes and parses it losslessly, and that
serialization stays in the session module ([ADR 0056](0056-keep-session-record-serialization-in-the-session-module.md)).

A new session records its built prompt as a **leading system message Session Entry** — `content`
empty, the structured sections, and the tool loadout — after the initial `model_change` /
`thinking_level_change` entries and before the first user message. Later prompt or tool-loadout
changes append **section-diff system messages** whose `sections` carry the changed values and an
explicit removal for each section that disappeared; an unchanged prompt appends nothing. The
native format no longer writes `active_tools_change` entries. Session Resume and context rebuild
replay the transcript's system messages to reconstruct the current prompt and the active tool
loadout, and the replayed prompt equals the prompt built for the next request.

**Request path.** The session replays the transcript into the request's leading system prompt (the
Agent's `system_prompt` context field, which the adapters emit as the provider's system/developer
message). Provider-side *mid-conversation* system messages — sending each transcript system message
in place for providers that accept them — remain **Deferred**, consistent with ADR 0059's Deferred
row; the adapters rebuild the leading prompt from the replayed state instead. The transcript is the
record of truth; the request is its projection.

## Model and thinking mutations are session-only by default

The Agent Session's set and cycle operations take a mutation-options value whose `persist` defaults
to false: an in-session change applies to the session only. Under `persist`, the global Settings
Scope default is written, and a model is additionally promoted into the scoped/enabled-model set
when one exists. Switching models computes the new thinking level as the explicit scoped level,
then the per-model override (a Deferred setting; absent), then the global default, then the current
live level, and clamps it to the model's supported set; a `thinking_level_change` entry is recorded
only when the effective level actually changes, so a model switch no longer resets the level. The
Native TUI model and thinking selectors expose save-as-default actions with pi's status wording
(`Default model: provider/id`, `Default thinking level: <level>`), while ordinary selection,
`/model <id>`, and model cycling stay session-only with `Model: <id>`.

## Resource semantics

Skill discovery distinguishes declared `SKILL.md` files from non-declared skill files (which
require a non-empty string description), prompt templates accept `argument-hint` only as a YAML
string, read and parse diagnostics are surfaced with pi's wording, and every resource read strips a
UTF-8 BOM — including the `@file` prompt text and external-editor content on the CLI/TUI surfaces.

## Evidence

The capture sidecars refuse any checkout that is not `f07218c4` with the `0.87.1` artifact, and the
committed session and prompt goldens are regenerated from a frozen `v0.87.1` checkout whose
workspace packages resolve to that checkout's own sources. The session golden projects the
system-message contract structurally — ordered section names with an explicit removal marker, plus
the added/removed tool names — because section *text* is machine- and identity-dependent (docs
paths, cwd, the identity line); the prompt text itself stays pinned by the prompt goldens. The
C++-side prompt goldens are regenerated to the sectioned shape.

## Deferred, with no placeholders

Capabilities pi gained after the baseline that this layer does not claim stay Deferred: cache-warming
usage entries, context-editing entries, per-model thinking-level overrides, default tool selection,
fullscreen/TUI-mode settings, extensions/plugins/Chord/durable/server surfaces, RPC/SDK/JSON modes,
HTML export, the new built-in tools (`grep`/`find`/`ls`/`powershell`), extension-forced prompts and
custom sections, provider-side mid-conversation system messages, managed-tool downloads, mermaid
rendering, bug-report/crash telemetry, mouse interaction, and pi's `--use-theme` flag and
progressive session-picker loading (the latter two recorded as explicit demotions on
[#771](https://github.com/lanshengzhi/cpp-coding-harness/issues/771) after the `v0.87.1` delta
audit). None of them carries a placeholder surface.

## Consequences

- The module's evidence is honest again: the committed goldens pin `v0.87.1`, and the delta audit
  ([#777](https://github.com/lanshengzhi/cpp-coding-harness/issues/777)) classified the remaining
  CLI / print / list-models / Native TUI surfaces, with every changed behavior either ticketed
  ([#786](https://github.com/lanshengzhi/cpp-coding-harness/issues/786)–[#789](https://github.com/lanshengzhi/cpp-coding-harness/issues/789))
  or explicitly demoted.
- The transcript, not a side field, becomes the prompt's record: a resumed session sends the prompt
  its transcript last recorded, and `/reload` appends only the diff.
- The session format has exactly one shape. The one-time pi import understands the `v0.87.1` system
  messages and tool loadouts; there are no dual-format reads and no migration shims.

## References

- Issue [#771](https://github.com/lanshengzhi/cpp-coding-harness/issues/771) (spec) and tickets
  [#773](https://github.com/lanshengzhi/cpp-coding-harness/issues/773)–[#780](https://github.com/lanshengzhi/cpp-coding-harness/issues/780),
  [#786](https://github.com/lanshengzhi/cpp-coding-harness/issues/786)–[#789](https://github.com/lanshengzhi/cpp-coding-harness/issues/789).
- [ADR 0036](0036-own-the-scoped-pi-coding-agent-application-layer-capabilities-for-the-three-provider-paths.md)
  (historical record of the `83114817` phase), [ADR 0053](0053-replace-pi-parity-authority-with-the-product-architecture-contract.md)
  (architecture authority), [ADR 0056](0056-keep-session-record-serialization-in-the-session-module.md)
  (serialization ownership), [ADR 0059](0059-align-the-cch-ai-capability-subset-with-pi-ai-and-diverge-on-kimi-code.md)
  (cch_ai's own baseline; mid-conversation system messages Deferred there too), [ADR 0024](0024-record-and-explicitly-advance-the-pi-parity-baseline.md)
  (baseline-advance rule).
- New baseline `f07218c4d4bbc12bef056a7058c3dd49dfe41abe` (tag `v0.87.1`), superseding
  `83114817c68f5413e4d7ba6d7003ddc511cd31d2`: `packages/coding-agent/src/core/{system-prompt.ts,agent-session.ts,session-manager.ts,skills.ts,prompt-templates.ts,resource-loader.ts}`,
  `packages/coding-agent/src/modes/interactive/*`, `packages/ai/src/types.ts` (`SystemMessage`),
  `packages/agent/src/{agent.ts,agent-loop.ts}`.
- `fixtures/pi-coding-agent/README.md` (the re-pinned evidence bundle) and
  `fixtures/pi-coding-agent/capture/*` (the capture guards and canonical projections).
