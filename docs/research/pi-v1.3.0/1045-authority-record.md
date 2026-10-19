# pi-v1.3.0 authority record (#1045)

- **Baseline**: `pi-v1.3.0` at `71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64 with glibc.
- **Authority**: [ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md), recorded by commit
  `196f4231e4ebbc8bdb06d3b2152d72aa8850e956` together with the Durable Runtime glossary entry and the
  architecture/pi-parity navigation amendments.
- **Umbrella**: [spec #1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044); this record is ticket #1045.
- **Configured graph**: unchanged. The target Owner graph in ADR 0075 is not a claim that `cch_durable`,
  `cch_mcp`, `cch_codemode` or `cch_chord` already exist or that the manifest has migrated.

## Status vocabulary

Target membership, implementation and verification are separate fields on every inventory row:

| Field | Values | Meaning |
| --- | --- | --- |
| Membership | `supported`, `excluded`, `no-decision` | The capability is inside the selected target, explicitly outside it, or still awaiting an owner ruling. `no-decision` is neither support nor exclusion. |
| Implementation | `implemented`, `partial`, `missing` | What the configured C++ graph and observable behavior currently provide. A missing durable or TUI capability is implementation work, not `Deferred`. |
| Verification | `verified`, `upstream-captured`, `unverified`, `not-applicable` | `verified` requires an accepted comparison; `upstream-captured` means frozen-pi evidence exists but the C++ side is not accepted; `unverified` means no accepted evidence. |

## Clause-level supersessions

ADR 0075 supersedes only the affected clauses; the historical records keep their identity:

- ADRs 0005, 0034, 0039 and 0070: affected harness/session/tool and MCP ownership placements.
- ADR 0053: package-shape independence for this one v1.3.0 migration only; gates and frontend separation remain.
- ADR 0022: the extra Agent queue-capacity/default/rejection divergence; admission follows frozen pi.
- ADR 0054: TLS-only/no-redirect restrictions for selected MCP behavior; applicable TLS validation remains.
- ADRs 0024, 0060, 0065 and 0067: selected capability target revisions; historical evidence identities do not move.
- ADR 0066: conflicting membership/exemption rows; unrelated exclusions and `No decision` rows retain their status.
- ADRs 0038, 0040 and 0042: retained strict toolchain, serialized lifecycle and no-exception requirements.
- ADRs 0058 and 0068-0074: retained XDG and Magpie decisions; generic MCP ownership moves to `cch_mcp`.

## Retained boundaries

Magpie remains the only model path and the XDG Pike state root remains. `cch_ai` does not advance as a
package: only passive fields and serialization required by included non-AI behavior may be added.
Model-response timing, cross-model thinking conversion, the final/partial tool-argument parsing fixes and
additional AI retry matching stay excluded; classifier and image provider execution are not added.
Durable and the application Session remain separate runtimes, old Pike durable import/migration/dual reads
are omitted without deleting user data, and no installed SDK, stable ABI or cross-version source promise is
created.

## What this record does not claim

No production relocation, runtime behavior change, frozen-pi capture or capability verification landed in
this authority slice. Durable capability gaps are implementation work; capabilities outside ADR 0075's
selection and explicit additions remain `No decision`.

## Downstream audit conventions

The run-scoped shared audit extension protocol for tickets #1046-#1057 is
`/tmp/cch-1044-context/evidence-protocol.md` (not part of this commit). Audit ledgers live beside their
topic notes under `docs/research/pi-v1.3.0/`; ticket-owned evidence bundles use family `audit-<ticket>`
and may not rewrite the shared named-evidence registry. #1046 owns that registry and its verifier.

## Validation

Documentation-only slice: headings, relative links, tracker references and agent-facing English were
checked. No C++ build is required under the repository's documentation-only validation rule.
