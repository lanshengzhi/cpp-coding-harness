# Issue tracker: Local Markdown

Issues and PRDs for this repo live as markdown files in `.scratch/`.

## Conventions

- One feature per directory: `.scratch/<feature-slug>/`
- The PRD is `.scratch/<feature-slug>/PRD.md`
- Implementation issues are `.scratch/<feature-slug>/issues/<NN>-<slug>.md`, numbered from `01`
- Triage or lifecycle state is recorded as a `Status:` line near the top of each PRD or issue file (see `triage-labels.md` for the role strings)
- Use `ready-for-agent` when a PRD or derived implementation issue is fully specified for agent implementation, unless the user requests a different triage state
- Use `implemented` when the acceptance criteria are satisfied, implementation is committed, and validation is recorded in the issue or PRD
- Comments and conversation history append to the bottom of the file under a `## Comments` heading

## When a skill says "publish to the issue tracker"

Create a new file under `.scratch/<feature-slug>/` (creating the directory if needed).

## When a skill says "fetch the relevant ticket"

Read the file at the referenced path. The user will normally pass the path or the issue number directly.
