---
status: accepted
---

# Own Native TUI slash routing and host effects

Issue [#502](https://github.com/lanshengzhi/cpp-coding-harness/issues/502) intentionally supersedes ADR 0036 G4's Native TUI slash-command deletion and pass-through wording. The Native TUI owns a rendering-free `SlashCommandRouter` for slash tokenization, alias resolution, argument validation, unknown-command classification, and immediate-command dispatch through `SlashCommandExecutionContext`; it returns modal requests as passive values. `InteractiveMode` remains the host for rendering, session changes, and modal effects, while explicitly recognized Prompt Template, Skill, and compatible absolute-path submissions may remain Agent Prompts. Unknown slash commands and validation failures are visible routing errors, and this Native TUI contract change does not add slash dispatch to Print Mode.
