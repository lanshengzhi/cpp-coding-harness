# pi C++ parity

Read this when a task changes a Supported Capability, evaluates parity, interprets a `pi:` reference, or proposes a new parity ticket.

- The [pi C++ parity map](https://github.com/lanshengzhi/cpp-coding-harness/issues/2) is the planning authority for open parity decisions.
- The local pi source checkout is `../pi`; a `pi:` reference resolves from that root.
- Inspect the relevant current pi source or documentation before deciding or changing behavior. Matching supported pi semantics is the default; record an Intentional Divergence in the map or an accepted ADR.
- Preserve this repository's C++ idioms and [architecture guardrails](architecture.md) rather than mechanically translating TypeScript.
- Approved work leaves the map and follows `/to-spec` → `/to-tickets` → `/implement`.
- Prefer the clean pi-aligned end state over migrations, fallback reads, deprecation shims, or compatibility-only flags unless a current contract explicitly requires them.

The Parity Baseline and supported capability set remain authoritative until explicitly advanced (ADR 0024); current upstream behavior is evidence, not an automatic scope expansion. Semantic Parity is pinned to the frozen pi authority commit `83114817c68f5413e4d7ba6d7003ddc511cd31d2`. Architecture spec [#439](https://github.com/lanshengzhi/cpp-coding-harness/issues/439) with [ADR 0039](../adr/0039-own-the-capability-owner-package-graph-and-parity-architecture-gate.md) and [ADR 0040](../adr/0040-own-asynchronous-operations-and-the-serialized-runtime-lifecycle.md) records the change authority for the Capability Owner Package graph, the Parity Architecture Gate, the serialized Runtime, the Supported Platform and toolchain floors, and the Runtime-only release boundary; it neither advances the baseline nor adds a Supported Capability.
