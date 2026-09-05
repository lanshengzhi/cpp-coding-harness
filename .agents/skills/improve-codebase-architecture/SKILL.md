---
name: improve-codebase-architecture
description: Filter a codebase's deepening opportunities through an existence test, present the survivors as a visual HTML report, then grill through whichever one you pick.
disable-model-invocation: true
---

# Improve Codebase Architecture

Surface architectural friction and propose **deepening opportunities**: refactors that turn shallow modules into deep ones. The aim is testability and AI-navigability.

This command is _informed_ by the project's domain model, filtered by minimality, and built on a shared design vocabulary:

- Call the Skill tool with "minimal-code" and follow its architecture **existence test** before exploring. It decides whether a candidate earns a card.
- Call the Skill tool with "codebase-design" for the architecture vocabulary (**module**, **interface**, **depth**, **seam**, **adapter**, **leverage**, **locality**) and its principles (the deletion test, "the interface is the test surface", "one adapter = hypothetical seam, two = real"). Use these terms exactly to shape candidates that pass the existence test, and don't drift into "component," "service," "API," or "boundary."
- The domain language in `CONTEXT.md` gives names to good seams; ADRs in `docs/adr/` record decisions this command should not re-litigate.

## Process

### 1. Explore

**Scope before you scan: YAGNI.** Deepening a module pays off by making future changes to it easier, so put extra weight on the parts of the codebase that have recently changed. Decide *where* to look before you look:

- If the user named a direction (a module, a subsystem, a pain point), take it, and skip the inference below.
- Otherwise, walk back a good stretch of the commit history (`git log --oneline`) to find the codebase's hot spots, the files and areas that keep coming up, and let those paths pull your attention first. If the changes are scattered with no clear hot spot, widen the net.

Read the project's domain glossary (`CONTEXT.md`) and any ADRs in the area you're touching first.

Then spawn a sub-agent to walk the codebase. Don't follow rigid heuristics; explore organically and note where you experience friction:

- Where does understanding one concept require bouncing between many small modules?
- Where are modules **shallow**, with an interface nearly as complex as the implementation?
- Where have pure functions been extracted just for testability, but the real bugs hide in how they're called (no **locality**)?
- Where do tightly-coupled modules leak across their seams?
- Which parts of the codebase are untested, or hard to test through their current interface?

Apply the **deletion test** to anything you suspect is shallow: would deleting it concentrate complexity, or just move it? A "yes, concentrates" is the signal you want.

Then apply the **existence test** to every suspected candidate. Exploration is complete only when every survivor names its evidence, its earliest structural move, and what becomes smaller or disappears. If none survive, the scan has found no actionable deepening opportunities.

### 2. Present candidates as an HTML report

Write a single-file HTML report to the OS temp directory so nothing lands in the repo. Resolve the temp dir from `$TMPDIR`, falling back to `/tmp` (or `%TEMP%` on Windows), and write to `<tmpdir>/architecture-review-<timestamp>.html` so each run gets a fresh file. Open it for the user (`xdg-open <path>` on Linux, `open <path>` on macOS, `start <path>` on Windows) and tell them the absolute path.

The report uses **Tailwind via CDN** for layout and styling, and **Mermaid via CDN** for diagrams where a graph/flow/sequence reliably communicates the structure. Mix Mermaid with hand-crafted CSS/SVG visuals: use Mermaid when relationships are graph-shaped (call graphs, dependencies, sequences), and hand-built divs/SVG when you want something more editorial (mass diagrams, cross-sections). Each candidate gets a **before/after visualisation**. Be visual.

For each candidate, render a card with:

- **Files**: which files/modules are involved
- **Evidence**: the observed friction or named upcoming change
- **Problem**: why the current architecture is causing friction
- **Solution**: the earliest structural move from the existence test that resolves it
- **What disappears**: which interface, module, duplication, or spread of knowledge becomes smaller or goes away
- **Benefits**: explained in terms of locality and leverage, and how tests would improve
- **Before / After diagram**: side-by-side, custom-drawn, illustrating the shallowness and the deepening
- **Recommendation strength**: one of `Strong`, `Worth exploring`, `Speculative`, rendered as a badge; `Speculative` means the payoff is uncertain, not that evidence is absent

When candidates exist, end the report with a **Top recommendation** section: which candidate you'd tackle first and why. When none pass the existence test, render a short empty state saying **No actionable deepening opportunities** and omit the Top recommendation.

**Use CONTEXT.md vocabulary for the domain, and the `/codebase-design` vocabulary for the architecture.** If `CONTEXT.md` defines "Order," talk about "the Order intake module," not "the FooBarHandler," and not "the Order service."

**ADR conflicts**: if a candidate contradicts an existing ADR, only surface it when the friction is real enough to warrant revisiting the ADR. Mark it clearly in the card (e.g. a warning callout: _"contradicts ADR-0007, but worth reopening because…"_). Don't list every theoretical refactor an ADR forbids.

See [HTML-REPORT.md](HTML-REPORT.md) for the full HTML scaffold, diagram patterns, and styling guidance.

Keep interface design for the grilling loop. After a report with candidates is written, ask the user: "Which of these would you like to explore?" After an empty report, tell the user the scan found no actionable deepening opportunities and end the run.

### 3. Grilling loop

Once the user picks a candidate, call the Skill tool with "grilling" to walk the decision tree with them: constraints, dependencies, the shape of the deepened module, what sits behind the seam, what tests survive. Keep the existence test live: when the grilling reveals that an earlier structural move holds, reframe the candidate around it; when its evidence fails, close it with no change.

Side effects happen inline as decisions crystallize; call the Skill tool with "domain-modeling" to keep the domain model current as you go:

- **Naming a deepened module after a concept not in `CONTEXT.md`?** Add the term to `CONTEXT.md`. Create the file lazily if it doesn't exist.
- **Sharpening a fuzzy term during the conversation?** Update `CONTEXT.md` right there.
- **User rejects the candidate with a load-bearing reason?** Offer an ADR, framed as: _"Want me to record this as an ADR so future architecture reviews don't re-suggest it?"_ Only offer when the reason would actually be needed by a future explorer to avoid re-suggesting the same thing; skip ephemeral reasons ("not worth it right now") and self-evident ones.
- **Want to explore alternative interfaces for the deepened module?** Call the Skill tool with "codebase-design" and use its design-it-twice parallel sub-agent pattern.

### 4. Merge onto the main flow

The run ends when the grilling's frontier is empty and the user confirms you've reached a shared understanding. This skill never edits production code and never files implementation issues: building belongs to the main flow, and splitting work into tickets is `/to-tickets`' job. Your last act is to close out the findings and name the next command.

Summarise the handoff packet for the user:

- **Chosen candidate**: which card won, in one line.
- **Confirmed shape**: the deepened module's shape, plus the dependency and seam constraints settled during grilling.
- **Minimal move**: the earliest structural move that holds and what becomes smaller or disappears.
- **Test strategy**: what gets tested, at which seam, and which existing tests survive.
- **Validation path**: how the user will see the change working.
- **Paper trail**: paths to every `CONTEXT.md` term added or sharpened and every ADR written along the way.
- **Open questions**: anything the grilling left unresolved, so the next phase knows it still owes an answer.

Then route by size, and tell the user which command comes next. A single deepened module that lands green in one pass is a single-session task; work that needs sequencing (several modules, an expand-contract migration, several verifiable increments) is multi-session.

- **Single-session** → `/implement`, right here in this context window: the grilling is still verbatim in context, which is exactly what the implementation wants.
- **Multi-session** → `/to-spec` next, then `/to-tickets`, then `/implement` per ticket, `/clear`ing context between tickets. Keep the spec and ticket drafting in this window so they build on the grilling. Size is what decides this route: large work goes through spec and tickets even when this session could start editing immediately.

If the grilling surfaced a question conversation can't settle (one that needs a runnable answer), take the prototype detour first: `/handoff` out, `/prototype` in the fresh session, `/handoff` back with what was learned, then resume routing from here.
