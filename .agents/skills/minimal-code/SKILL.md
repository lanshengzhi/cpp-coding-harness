---
name: minimal-code
description: "Shared discipline for deciding whether architecture work should exist and writing the least code that works. Use when assessing architecture refactors, writing or fixing code, choosing libraries or dependencies, or when another skill needs the minimal-change vocabulary."
---

# Minimal Code

Make the smallest change that actually works. This shared reference has two branches:

- **Architecture work under consideration:** read [ARCHITECTURE.md](ARCHITECTURE.md) and run its **existence test** before designing a module.
- **Code about to be written:** climb the ladder below, then apply the rules and the list of what never gets cut.

## The ladder

Before writing code, stop at the first rung that holds:

1. **Does this need to exist at all?** A speculative need means skip it, and say so in one line. (YAGNI)
2. **Already in this codebase?** A helper, util, type, or pattern that already lives here gets reused. Look before you write; re-implementing what sits a few files over is the most common slop.
3. **The standard library does it?** Use it.
4. **A native platform feature covers it?** `<input type="date">` over a picker library, CSS over JS, a database constraint over application code.
5. **An already-installed dependency solves it?** Use it. A few lines beat a new dependency.
6. **Can it be one line?** One line.
7. **Only then:** the minimum code that works.

The ladder is a reflex, not a research project, but it runs *after* you understand the problem, not instead of it. Read the code the change touches and trace the real flow end to end, then climb. When two rungs both hold, take the higher one and move on. The ladder shortens the solution, never the reading: a small diff built on skimming is a confident wrong fix, not a minimal one.

## Rules

- Abstractions earn their place: no polymorphic seam for one real adapter, no factory for one product, no config for a value that never changes.
- Build what the task needs. Add support for future needs when they become real.
- Deletion over addition. Boring over clever. Fewest files possible. The shortest working diff wins, once you understand the problem; the smallest change in the wrong place is a second bug.
- Complex request? Ship the minimal version and question the rest in the same response: "Did X; Y covers it. Need the full X? Say so."
- Two standard-library options of the same size? Take the one that is correct on edge cases. Minimal means less code, not the flimsier algorithm.

## Bug fixes

A bug report names a symptom. Before editing, grep every caller of the function you are about to touch. The minimal fix is the root-cause fix: one guard in the shared function is a smaller diff than a guard in every caller, and patching only the path the report names leaves every sibling caller still broken. Fix it once, where all callers route through.

## What never gets cut

The ladder shortens the solution. It never shortens:

- **Understanding.** Trace the whole flow before picking a rung.
- **Input validation at trust boundaries.**
- **Error handling that prevents data loss.**
- **Security measures.**
- **Accessibility basics.**
- **Verification.** Minimal code still gets checked the project's way: in a `tdd` loop the red test still comes first, and agreed seams still get their tests. Minimality decides what to build, never whether to check it works.
- **Calibration knobs facing the physical world.** A real clock drifts and a real sensor reads off; leave the tuning knob a minimal model cannot foresee.
- **Anything explicitly requested.** If the user insists on the full version, build it without re-arguing.

## Marking deferred shortcuts

A deliberate simplification that cuts a real corner with a known ceiling (a global lock, an O(n²) scan, a naive heuristic) gets marked with a `debt:` comment naming the ceiling and the upgrade trigger:

`# debt: global lock, per-account locks if throughput matters`

The marker keeps the deferral findable, so "later" cannot quietly become "never".

## Output

Code first. Then at most three short lines: what was skipped, and when to add it. When the explanation runs longer than the code, the explanation goes; every paragraph defending a simplification is complexity smuggled back in as prose. Explanation the user explicitly asked for is exempt and given in full.

Pattern: `[code] → skipped: [X], add when [Y].`
