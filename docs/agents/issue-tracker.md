# Issue tracker: Local Markdown

Specs and tickets for this repo live as Markdown files in `.scratch/`.

## Conventions

- One effort per directory: `.scratch/<feature-slug>/`.
- The spec is `.scratch/<feature-slug>/spec.md`.
- Implementation tickets are `.scratch/<feature-slug>/issues/<NN>-<slug>.md`, numbered from `01` in dependency order.
- `Category:` records exactly one Matt triage category: `bug` or `enhancement`.
- `Status:` records one Matt triage state: `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, or `wontfix`.
- Completed local work replaces its triage state with the tracker lifecycle status `implemented`. Use it only when acceptance criteria are satisfied, the implementation is committed, and validation is recorded in the ticket or spec.
- Specs produced by `/to-spec` and feature tickets produced by `/to-tickets` normally use `Category: enhancement` and `Status: ready-for-agent`; bug work uses `Category: bug`.
- Comments and conversation history append under `## Comments`.
- Implemented efforts remain as closed tracker records, but agents must not load or scan them during normal implementation unless the user references them or review requires historical acceptance evidence.

## Publishing and fetching

When a skill says "publish to the issue tracker", create the effort directory and its `spec.md` or ticket files as appropriate.

When a skill says "fetch the relevant ticket", read the referenced file. The user will normally provide its path directly.

## Working the implementation frontier

For implementation tickets, the frontier contains tickets whose `Status:` is `ready-for-agent` and whose blockers are all `implemented`. Work one frontier ticket at a time with `/implement`.

Invoking `/implement` follows Matt's native behavior: it authorizes task-scoped changes, review, and one commit on the current branch. It does not create or switch branches or worktrees, and it does not authorize push, merge, branch deletion, or inclusion of unrelated user changes.

## Wayfinding operations

Used by `/wayfinder`. A wayfinder map is distinct from an implementation spec.

- **Map**: `.scratch/<effort>/map.md` — Destination, Notes, Decisions so far, Not yet specified, and Out of scope.
- **Child decision ticket**: `.scratch/<effort>/issues/<NN>-<slug>.md`, with a `Type:` of `research`, `prototype`, `grilling`, or `task`.
- **Status**: `open` is unclaimed, `claimed` is in progress, and `resolved` is closed.
- **Blocking**: `Blocked by: NN, NN`; a ticket is unblocked when every listed ticket is `resolved`.
- **Frontier**: open, unblocked, unclaimed child tickets, ordered by number.
- **Claim**: change `Status: open` to `Status: claimed` before work.
- **Resolve**: append the answer under `## Answer`, set `Status: resolved`, and append a linked one-line gist to the map's Decisions so far.

Do not apply implementation ticket states such as `ready-for-agent` or `implemented` to wayfinder decision tickets.
