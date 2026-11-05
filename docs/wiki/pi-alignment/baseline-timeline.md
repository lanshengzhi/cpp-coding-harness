# Baseline timeline

Historical record as of `lanshengzhi/cpp-coding-harness@6ca7b49` (2026-11-04). Each row is what the cited document records, not a statement about present code. Related: [ADR 0075 scope](adr-0075-scope.md), [goals](goals.md).

| Record | Baseline it names | Role |
| --- | --- | --- |
| [ADR 0024](../../adr/0024-record-and-explicitly-advance-the-pi-parity-baseline.md) | recorded pi commit rather than upstream HEAD; advancing is an explicit audit | rule for baseline advancement |
| [ADR 0053](../../adr/0053-replace-pi-parity-authority-with-the-product-architecture-contract.md) | frozen pi `83114817c68f5413e4d7ba6d7003ddc511cd31d2` is the founding anchor it supersedes as authority | pi becomes evidence, not design authority |
| [ADR 0060](../../adr/0060-advance-the-cch-coding-agent-baseline-to-pi-v0-87-1-and-adopt-transcript-system-messages.md) | pi v0.87.1, `f07218c4d4bbc12bef056a7058c3dd49dfe41abe` | application layer baseline |
| [ADR 0066](../../adr/0066-pi-capability-scope-decisions-and-open-questions.md) | per-capability membership register | scope decisions, later amended |
| [ADR 0067](../../adr/0067-align-the-linux-tui-toolkit-and-native-tui-with-pi-v1-0-4.md) | pi v1.0.4, `7c10bd4337495ee613f2224843ecdf349b80d1df` | Linux TUI target; target amended by ADR 0075 |
| [ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md) | pi-v1.3.0, `71023aec1546d6d677a65669c5665c0927ce0dd0` | selected non-AI target and Owner migration |

Notes:
- The v1.0.4 SHA is recorded by the research reports [TUI](../../research/cch-tui-pi-v1.4-semantic-alignment.md) and [MCP/codemode](../../research/mcp-codemode-pi-v1.4-parity-audit.md).
- ADR 0053 made pi parity no longer the architecture authority (see `docs/agents/pi-parity.md` at `lanshengzhi/cpp-coding-harness@6ca7b49`); later ADRs select baselines per capability. As of that commit, `docs/agents/pi-parity.md` records ADR 0075 as the accepted target and says the manifest still describes the configured graph, not the target graph.
- ADR 0075 records that it does not advance `cch_ai` overall and does not claim v1.3.0 evidence already exists. Historical bundle identities are preserved.
- This wiki records no baseline newer than pi-v1.3.0; see [open questions](open-questions.md). On 2026-11-04 the owner confirmed the target stays v1.3.0 and that periodic follow-up of later releases will be requested; no later baseline is selected.
