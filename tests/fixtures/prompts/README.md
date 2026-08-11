# System Prompt goldens

`goldens/system-prompt-default.txt`, `goldens/system-prompt-custom.txt`, and
`goldens/system-prompt-empty-tools.txt` pin the System Prompt's **identity
delta** byte-for-byte (ADR 0036 G4): the structure is pi's
`packages/coding-agent/src/core/system-prompt.ts` `buildSystemPrompt` at the frozen
baseline `83114817c68f5413e4d7ba6d7003ddc511cd31d2` exactly — default/custom branches,
`Available tools` from tool snippets, the deduped guidelines (tool guidelines + the
auto bash-exploration rule + the always-lines), the append section, `<project_context>`,
the skills section, and the trailing `Current working directory:` line — with **only**
the identity line and the documentation block swapped for the C++ binary's own
("cch") identity and docs paths. The delta was verified by running the frozen pi
`buildSystemPrompt` with the same inputs and diffing after the identity substitution
(byte-identical); the pi-truthy edge cases are pinned too (an empty custom prompt
falls through to the default branch; an explicitly empty tool selection keeps no
tools while the tool guidelines still render).

Fixture rules:

- All paths inside the goldens are scrubbed dummy values (`/cch/README.md`,
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
`CCH_CAPTURE_GOLDENS=1 scripts/run-tests.sh "[coding_agent][prompt][system-prompt]"`.
