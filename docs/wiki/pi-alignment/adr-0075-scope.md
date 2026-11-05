# ADR 0075 scope

Summary of [ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md) (accepted; added by commit `196f4231e`, 2026-10-19), read at `lanshengzhi/cpp-coding-harness@6ca7b49`. Read the ADR for authority; this page only orients. Spec: [#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044).

## Target
- Baseline: `pi-v1.3.0` at `71023aec1546d6d677a65669c5665c0927ce0dd0`; Linux x86-64 with glibc. Revision, not a moving `../pi` working tree, decides comparisons.
- Selected non-AI capabilities preserve availability, defaults, errors, state transitions, ordering and cancellation; C++ API shape may differ.
- Membership, implementation and verification are separate records. An accepted target cannot be reclassified Deferred to close work. Unresolved membership stays "No decision".

## Included (selected)
Complete Linux `cch_durable` (Memory, JSONL and SQLite storage with bidirectional continuation with frozen pi); complete Linux `cch_tui` (ADR 0067 advanced to v1.3.0); reusable `cch_mcp`, `cch_codemode` and a durable-required `cch_chord` subset; named MCP, codemode, tool-timing, application and extension additions.

## Excluded or retained
- Retained: Magpie-only model path (ADRs 0068, 0071 to 0074) and the XDG state root (ADR 0058). `cch_ai` gets no overall alignment; explicit AI exclusions are listed in the ADR.
- Excluded: `streamProxy` protocol, standalone pi-env SSH/daemon, telemetry and evals packages, Cloudflare adapters, macOS/Windows helpers, third-party TS/JS extension execution, upload/share services, installer/updater, SDK and stable-ABI promises.

## Rules relevant to later work
- "Future pi releases require another audit; the Owner graph does not automatically track upstream HEAD." See [open questions](open-questions.md).
- Clause-level supersessions of ADRs 0005, 0022, 0024, 0034, 0039, 0053, 0054, 0060, 0065, 0066, 0067 and 0070 are listed in the ADR.
- The ADR does not alter the manifest, create issues or register evidence by itself.

## Owner decision 2026-11-04
The owner decided that the immediate v1.3.0 scope is **exactly** this ADR's selected scope (the Included, Excluded and Retained sections above), not a wider reading of pi v1.3.0. Periodic follow-up of upstream releases is a separate, later request; the target stays v1.3.0. See [goals](goals.md) and [open questions](open-questions.md).

Related: [baseline timeline](baseline-timeline.md), [evidence and delivery history](evidence-and-delivery-history.md), [goals](goals.md).
