# pi alignment wiki

Shared historical record for aligning Pike with upstream pi. Created 2026-11-04 at `lanshengzhi/cpp-coding-harness@6ca7b49`. Pages are dated claims, not a description of the present code; see [conventions](conventions.md). Sources are cited, never copied.

## Goals and open questions
- [Goals](goals.md): the owner's immediate and long-term alignment goals and the two later decisions (2026-11-04).
- [Open questions](open-questions.md): what the owner decisions of 2026-11-04 settled and what is still undetermined.
- [Owner decisions, 2026-11-04](decisions-2026-11-04.md): the chief-of-staff rulings answering #1183 — the accepted #1104 withdrawal, the fail-closed #1202 downgrade, the symlink blocker, and the tickets filed (N1–N6 as #1203–#1208, B1–B17 as #1209–#1225).
- [Owner decisions, 2026-11-05](decisions-2026-11-05.md): the owner ruling (decision B) settling the JSONL task-sidecar symlink divergence as funded alignment with frozen pi (ADR 0076).
- [v1.3.0 residual-gap audit, 2026-11-04](residual-gap-audit-2026-11-04.md): complete, claimed-only and open work within ADR 0075's scope.
- [Reconciliation proposal for #1183, 2026-11-04](reconciliation-1183-proposal-2026-11-04.md): proposed row/ticket mapping and safe ledger, frontier and deliveries edits for the delivery-ownership index; nothing applied.
- [Build readiness for #1202 re-anchoring, 2026-11-05](build-readiness-2026-11-05.md): the `vcpkg` preset and receipt labels, the stale `build/` tree, toolchain/vcpkg/`PI_CHECKOUT` state, and what is missing to make re-anchoring cheap.

## History
- [Baseline timeline](baseline-timeline.md): which pi revision each ADR selected, from v0.83.0 to v1.3.0.
- [ADR 0075 scope](adr-0075-scope.md): what the accepted v1.3.0 target includes, excludes and supersedes.
- [Evidence and delivery history](evidence-and-delivery-history.md): the #1044 umbrella, ledger, reconciliations and evidence-integrity findings.
- [Evidence artifact growth, 2026-11-06](evidence-artifact-growth-2026-11-06.md): why every lane commits a rebuilt 5.5 MB ledger, its measured git cost, and the three representation options raised.
- [TUI route batches B1–B17](tui-route-batches.md): the #1209–#1225 row partition, which batches closed, the #1229 discriminating-case discipline, and B13's slice record at `d8182efc9`.
- [The discriminating-case rule and where it stops](discriminator-rule-limit.md): why 44 bounded claims and #1229's 182 cannot move, the `json:strict-validation` evidence, and the unresolved owner fork.
- [Ledger field staleness](ledger-field-staleness.md): why `implementation`/`consumer`/`gaps` keep audit-time values, the 88 rows still saying `cch_durable` does not exist, and candidate fail-closed detectors.

## Operations
- [Ops: lanes and concurrency](ops-lanes.md): lane types, the one-code-lane rule, handoff protocol, and the 2026-11-05 collision record.

## Sources and maintenance
- [Source map](sources.md): ADRs, research records and agent docs to read first, with their known quirks.
- [Conventions](conventions.md): how this wiki is written and linted.
