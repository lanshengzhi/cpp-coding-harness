# Worktree Discipline Reference

This file preserves the detailed branch, merge, and publishing conventions for agents working in this repository. The root [`AGENTS.md`](../../AGENTS.md) remains the entry point; read this reference only when a task asks for branch, merge, publish, PR, or worktree-cleanup behavior.

## Default posture

- Treat this repository as a single-maintainer experimental repo; pull requests are not mandatory by default.
- Prefer a feature branch for extended work so `main` does not hold unfinished changes.
- Do not commit, merge, push, delete branches, or force-push unless the user explicitly asks for that operation.
- Invoking `/implement` is explicit authorization for Matt's native task-scoped review and commit on the current branch. It does not authorize creating or switching branches/worktrees, pushing, merging, deleting branches, or including unrelated user changes.
- Treat pre-existing modified or untracked files as user-owned. Do not overwrite, reformat, delete, or clean them up unless the task explicitly targets them.

## Merge and publish flow

When the user explicitly asks to merge or publish a completed branch and the relevant validation has passed:

1. Check whether `origin/main` changed or diverged before publishing. Fetch and inspect the relationship first; never force-push to repair divergence.
2. Prefer a fast-forward merge into `main`:

   ```bash
   git switch main
   git merge --ff-only <branch>
   git push origin main
   ```

3. Delete a feature branch only after it is confirmed to be contained in `main` and branch cleanup is part of the user's requested publish/merge workflow.
4. If fast-forward merge is not possible, stop and report the divergence instead of creating an unrequested merge commit.

## Pull requests

Suggest a pull request only when the user asks for one, when CI/review history is explicitly useful, or when the change risk justifies review records. Otherwise, hand off with changed scope, validation performed, and skipped validation as required by `AGENTS.md`.
