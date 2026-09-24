---
status: accepted
---

# Own Native TUI slash routing and host effects

> **#792 amendment (pi fall-through for unrecognized slash text).** The clause
> below that makes unknown slash commands visible routing errors is superseded:
> `SlashCommandRouter::route` now returns `SlashCommandPassThrough` for a token
> that names no built-in, so the submission reaches the Agent Prompt path
> exactly as it does in pi's Native TUI (which dispatches only its built-in
> names and hands every other submission to `session.prompt`). Validation
> failures for a *known* command remain visible routing errors — that part of
> the clause stands. `parse()` keeps its parse-level `UnknownCommand`
> classification as a pure parse result. The `allow_unrecognized` predicate and
> the `is_dynamic_slash_command` helper are deleted: every unrecognized token
> passes through, so the host no longer decides which ones may. Prompt
> Templates, `/skill:<name>` resources, and compatible absolute-path
> submissions are unaffected in outcome. Consequence recorded with the change:
> the pi commands this product does not port (`/export` `/import` `/share`
> `/changelog` `/clone`, the hidden `/debug` entry, `/bug`, and the easter
> eggs) no longer produce an error
> and instead fall through as prompt text; they remain unimplemented.
> `docs/usage.md` and the ADR 0036 G4 sentence that matched the old clause are
> updated with it. Implements [#792](https://github.com/lanshengzhi/cpp-coding-harness/issues/792).

Issue [#502](https://github.com/lanshengzhi/cpp-coding-harness/issues/502) intentionally supersedes ADR 0036 G4's Native TUI slash-command deletion and pass-through wording. The Native TUI owns a rendering-free `SlashCommandRouter` for slash tokenization, alias resolution, argument validation, unknown-command classification, and immediate-command dispatch through `SlashCommandExecutionContext`; it returns modal requests as passive values. `InteractiveMode` remains the host for rendering, session changes, and modal effects, while explicitly recognized Prompt Template, Skill, and compatible absolute-path submissions may remain Agent Prompts. Unknown slash commands and validation failures are visible routing errors, and this Native TUI contract change does not add slash dispatch to Print Mode.
