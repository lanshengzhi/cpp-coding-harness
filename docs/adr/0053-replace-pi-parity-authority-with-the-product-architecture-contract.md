---
status: accepted
---

# Replace pi Parity Authority with the Product Architecture Contract

This repository was founded as "an experimental C++23 coding-agent Runtime that preserves selected pi semantics" (AGENTS.md): ADR 0024 anchored Semantic Parity to the frozen pi commit `83114817c68f5413e4d7ba6d7003ddc511cd31d2`, issue #2 (the parity map) arbitrated open parity decisions, and ADR 0039 made pi capability ownership the authority the architecture gate enforces. An external architecture review (`pike.md`, reviewed 2026) demonstrated the cost: the gate certifies that the dependency graph matches pi's attribution, which is a different question from whether the module boundaries are right — and it cannot see the couplings that live *inside* the oversized `cch_coding_agent` owner (Session core, Models Runtime, CLI, and Native TUI compiled as one static library).

The owner has decided: **the repository de-pivots from pi and becomes an independent product** (working codename *pike*). Upstream pi ceases to be the design authority and becomes one source of evidence among others.

We therefore replace the parity authority with the **Product Architecture Contract**: the same gate machinery (`cmake/parity/manifest.json`, `parity_gate.py`) now protects boundaries this product chooses for itself, starting with: the Headless core must not depend on any frontend; the execution kernel must not depend on concrete filesystem, shell, or session-file implementations; providers must not know branch navigation or UI state; product session records must not live in the AI message model. The contract evolves by ADR, exactly as the parity baseline did.

## Explicit supersessions and retentions

Partial supersession, stated clause by clause:

- **ADR 0024 — superseded.** The frozen pi baseline no longer pins Supported Capabilities; "matching pi semantics" stops being the default and becomes a per-capability product decision. Issue #2 is closed with a pointer here. Observed pi behavior is demoted from *baseline* to *evidence*.
- **ADR 0039 — retained mechanism, replaced referent.** The Owner Package graph and the gate stand; what the gate certifies changes from pi-attributed ownership to the Product Architecture Contract. `cch_ai`, `cch_agent_core`, `cch_tui`, and `cch_support` are unaffected. `cch_coding_agent`'s scope (Session + Models Runtime + CLI + TUI in one target) is the first boundary the contract rejects; its split follows in the re-gate ticket.
- **ADR 0040 — retained in full.** The serialized Runtime lifecycle is an engineering decision, not a pi concession. What changes later (per the review) is *who owns* serialization: the Runtime host, not each caller's discipline.
- **ADR 0051 / 0052 — retained, and their promise is unfinished.** The projection stream already states "every fact a projection needs to render arrives through the one stream"; today's push-only tool patches with no snapshot slice under-deliver that promise. Completing the Read Model is program work, not a direction change.

## Compat strategy

pi's session format, config format, and special data shapes move to a `compat/pi` edge layer used by a **one-time explicit import** command. There are no runtime fallback reads, no dual-format session loading, and no deprecation shims — the repository's standing "clean end state over migrations and fallback reads" principle now applies to pi itself. The default configuration directory leaves `~/.pi/agent` for the product's own namespace; branding changes are confined to user-visible surfaces (binary name, config directory, docs). `cch_` target names, include paths, and namespaces stay: renaming them is pure churn with no architectural payoff.

## Retained pi inheritances

Not everything pi-shaped is a compromise. We keep: the serialized Runtime (ADR 0040), bounded subscriber mailboxes and `AsyncResult` (ADR 0052), and the TUI's pi-compatible keybindings and slash commands as **user defaults** — muscle memory is user value, not architecture, and must not appear in the architecture contract as a constraint.

## Non-goals

- No async-framework rewrite; the existing `AsyncResult` / bounded-mailbox / serialized-Runtime mechanisms are the substrate the program builds on.
- No Owner-graph redraw beyond splitting `cch_coding_agent`; further boundary changes require their own ADRs.
- No behavior changes in this ADR; it moves authority only.

## Considered options

- **Keep pi parity as the authority and fix couplings inside it**: rejected — the review showed the gate then structurally cannot reject boundaries pi's attribution accepts (`cch_coding_agent` is legal while containing the very couplings we most need to remove); we would be fixing the product against its own specification.
- **Fork hard: delete the gate, the manifest, and all pi references at once**: rejected — an authority vacuum during migration is worse than a mispointed authority; the gate mechanism is cheap, already wired into CI, and machine-readable authority is a practice worth keeping regardless of what it protects.
- **Soft fork: keep runtime dual-read compatibility with pi config and sessions indefinitely**: rejected — a permanent compat promise keeps pi's data shapes as the de facto internal domain model, which is the coupling the program exists to remove; one-time import preserves user data without preserving the design constraint.

## Consequences

- Issue #2 (parity map) is closed as superseded; `docs/agents/pi-parity.md` is rewritten as the product architecture-contract guide in the re-gate ticket.
- The de-pi program's first tickets: re-gate the manifest and split `frontend_tui`/`frontend_cli` out of `cch_coding_agent`; complete the projection Read Model; audit the test suite into `spec` / `compat-pi` / `diverge` labels; structured inference-failure classification; own config namespace with one-time pi import.
- Every migration chain must end by deleting its old entry — dual state sources and dual execution paths are forbidden as steady states, and manifest exceptions must carry an owner and a removal ticket.
- Glossary terms recorded in CONTEXT.md: Product Architecture Contract, Compat Layer; Parity Baseline and the Parity map terms are marked historical.
