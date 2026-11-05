# Source map

Read these first, as of `lanshengzhi/cpp-coding-harness@6ca7b49` (2026-11-04). Cite them; do not copy or edit them from this wiki. Related: [conventions](conventions.md), [index](index.md).

## Agent instructions
- [validation](../../agents/validation.md): acceptance discipline, validation tiers, documentation-only checks.
- [pi parity](../../agents/pi-parity.md): the product contract and selected pi-v1.3.0 migration notes.
- [architecture](../../agents/architecture.md): Owner packages and seams.

## ADRs
[0024](../../adr/0024-record-and-explicitly-advance-the-pi-parity-baseline.md), [0053](../../adr/0053-replace-pi-parity-authority-with-the-product-architecture-contract.md), [0060](../../adr/0060-advance-the-cch-coding-agent-baseline-to-pi-v0-87-1-and-adopt-transcript-system-messages.md), [0066](../../adr/0066-pi-capability-scope-decisions-and-open-questions.md), [0067](../../adr/0067-align-the-linux-tui-toolkit-and-native-tui-with-pi-v1-0-4.md), [0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md). Pages: [baseline timeline](baseline-timeline.md), [ADR 0075 scope](adr-0075-scope.md).

## Research
- [TUI v1.0.4 semantic alignment](../../research/cch-tui-pi-v1.4-semantic-alignment.md) and [MCP/codemode v1.0.4 audit](../../research/mcp-codemode-pi-v1.4-parity-audit.md): file names say "v1.4"; text says pi v1.0.4 (`7c10bd43`).
- [pi-durable roadmap](../../research/pi-durable-architecture-and-migration-roadmap.md): v1.0.4 research baseline; it states it is not an owner membership ruling.
- `docs/research/pi-v1.3.0/`: [authority](../../research/pi-v1.3.0/1045-authority-record.md), [consolidation](../../research/pi-v1.3.0/1058-consolidation-frontier.md), [reconciliations](../../research/pi-v1.3.0/1044-reassessment-2026-10-30.md). Summarised in [evidence history](evidence-and-delivery-history.md). Also read for audits: the [1058 ledger](../../research/pi-v1.3.0/1058-consolidated-capability-ledger.json), [deliveries](../../research/pi-v1.3.0/1058-deliveries.json), [delivery reconciliation](../../research/pi-v1.3.0/1058-delivery-reconciliation.md) and [#1202 note](../../research/pi-v1.3.0/1202-1104-evidence-integrity.md). Quirk: the ledger and deliveries can lag closed tickets (see [audit](residual-gap-audit-2026-11-04.md)).
