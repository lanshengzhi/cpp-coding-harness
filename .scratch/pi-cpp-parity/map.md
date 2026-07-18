# Wayfinder Map: pi C++ parity

## Destination

Reach an explicit, maintainable boundary for an idiomatic C++ implementation of pi: supported module and contract semantics are clear, intentional deferrals are named, and each approved implementation slice can move into its own spec and tickets.

## Notes

- Treat pi module and contract semantics as the reference, but do not mechanically translate TypeScript.
- Preserve the repository guardrails in `AGENTS.md` and accepted ADRs.
- Use `/grilling` with `/domain-modeling` for product and boundary decisions.
- Use current pi source or documentation for research; do not rely on deleted historical plans.
- This map resolves decisions only. Approved implementation work leaves the map and follows `/to-spec` → `/to-tickets` → `/implement`.

## Decisions so far

- The implemented baseline uses passive public values, narrow capability seams, move-only event sinks, private serialization machinery, package-style CMake targets, workspace containment, and explicit non-sandbox security boundaries. Current details live in `README.md`, `docs/agents/module-routing.md`, code, tests, and accepted ADRs.
- CLI and SDK session creation intentionally share one private assembly policy while retaining distinct product contracts; see [Centralize session assembly policy in SessionFactory](../../docs/adr/0001-centralize-session-assembly-policy.md).
- Completed and superseded implementation plans are historical Git data, not current planning authority.
- User-level state lives in a pi-mirrored agent config directory (`~/.cpp-harness/agent/`, override `CCH_CODING_AGENT_DIR`) with pi's `settings.json` vocabulary; see [ADR 0002](../../docs/adr/0002-root-user-state-in-agent-config-directory.md).

## Not yet specified

- Which extension and package capabilities belong in this C++ product, and which should remain permanently out of scope.
- Which public runtime controls are justified by real consumers: queueing, cancellation, steering, follow-up, retry, compaction, or session replacement.
- How much additional RPC and SDK parity is valuable beyond the currently supported narrow surfaces.
- Whether platform packaging, binary distribution, terminal integration, and self-update belong to this repository.
- Which additional provider/model capabilities have concrete consumers rather than parity value alone.

## Out of scope

- Mechanical TypeScript-to-C++ translation.
- Placeholder APIs for unsupported pi features.
- Weakening workspace containment, redaction, output bounds, or the explicit “not a sandbox” boundary in the name of parity.
