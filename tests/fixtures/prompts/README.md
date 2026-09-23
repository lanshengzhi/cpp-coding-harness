# System Prompt goldens

`goldens/system-prompt-default.txt`, `goldens/system-prompt-custom.txt`, and
`goldens/system-prompt-empty-tools.txt` pin the System Prompt's **identity
delta** byte-for-byte (ADR 0036 G4, re-pinned by ADR 0060): the structure is pi's
`packages/coding-agent/src/core/system-prompt.ts` at the frozen baseline
`f07218c4d4bbc12bef056a7058c3dd49dfe41abe` (tag `v0.87.1`) exactly — the sectioned
document `buildSystemPromptSections()` builds (an untagged `preamble`, then `<tools>`,
`<rules>`, `<docs>` on the default branch, plus `<addendum>`, `<project_context>`,
`<skills>`, and `<cwd>` as they apply, joined with a blank line), the conditional
file-exploration rule and guideline order in `<rules>`, the skills section's read-tool
or bash wording by the active read-capable tool, and the posix-normalized `<cwd>` —
with **only** the identity line and the documentation block swapped for the C++
binary's own ("pike") identity and docs paths. The delta was verified by running the
frozen pi `buildSystemPrompt` with the same inputs and diffing after the identity
substitution (byte-identical); the pi-truthy edge cases are pinned too (an empty custom
prompt falls through to the default branch; an explicitly empty tool selection keeps no
tools while the tool guidelines still render).

The message-level counterpart lives in `fixtures/pi-coding-agent/prompts/*-message.json`
(the differential golden, byte-compared by `SystemPromptGoldenTest`), and the transcript
record of the same sections is pinned by the session goldens
(`fixtures/pi-coding-agent/sessions/*.json`), which project the system message's ordered
section names, removal markers, and tool loadout.

Fixture rules:

- All paths inside the goldens are scrubbed dummy values (`/pike/README.md`,
  `/tmp/workspace`, `/home/user/.agents/skills/...`); no real machine paths, keys, or
  session material appear.
- The default golden uses the production session shape: the four fixed tools with
  pi's verbatim `promptSnippet`/`promptGuidelines`, one visible skill, and a clean
  cwd. The custom golden exercises the custom branch with append, project context
  files, and skills.
- The production docs paths resolve from the build-time source tree
  (`CCH_SOURCE_DIR`); the builder under test receives the scrubbed paths directly, so
  the goldens are deterministic and regenerable.

Regenerate deterministically with
`CCH_CAPTURE_GOLDENS=1 ctest --test-dir build -L system-prompt` (the `system-prompt` label
selects every `[system-prompt]` case; only the golden cases write under the capture flag).
