# Consolidated delivery receipts

A whole-row delivery pins its production and comparison test inputs, frozen comparison artifacts,
validation JUnit, and the evidence tooling used for the comparison. Its `testedRevision` must
contain every pinned input at its recorded digest; record the test command, selected count and
UTC timestamp together with the JUnit. Rerun the comparison on a clean committed revision when
any pinned input changes, then rebind all validation metadata and affected digests together.

Do not pin the entire `consolidation-plan.json`: it is shared classification bookkeeping and
unrelated families add routes and orphans. Instead each receipt pins `routingSha256`, derived
from pre-delivery rows and verified registry artifacts, without using the receipt's comparisons.
Sort claimed row IDs and each row's unique scenario names. For each scenario, select the row's
pre-delivery evidence artifact if it contains that scenario; otherwise require exactly one
registered artifact containing it. Ambiguous matches fail closed. Include row ID, scenario name, artifact path relative to the
baseline directory, and SHA-256 of its verified bytes. Serialize the list as UTF-8 JSON with
sorted object keys, no extra whitespace, and unescaped Unicode, then SHA-256 those bytes.

In-scope families are the registry's entries, including variants named
`audit-<ticket>-<variant>`; artifact paths and frontier captures accept the same form. A registered
scenario that no row routes stays an explicit orphan.

This binds the resolved delivery routing and artifact identity, while dropping byte-level protection
of unrelated plan fields and formatting. Global classification and orphan coverage remain checked
by consolidation `check`; other plan changes that do not change the delivery's resolved routing
are not receipt inputs. The comparison mapping and completeness of ordinary file inputs remain
review obligations.

## Per-claim withdrawal

A receipt that stays otherwise active can withdraw exactly the claims #1202 disputed through a
checked-in `1202-withdrawn-<receipt>-claims.json` record next to `1058-deliveries.json`. The record
names the issue, the receipt, the exact claim ids (whole rows, or single comparisons within a claim
via `withdrawnComparisons`), a reason and a date, and preserves the full withdrawn claims. `build`
and `check` validate every reference against the full receipts, fail-closed on unknown ids, validate
the full receipt as without the channel, and skip the withdrawn claims for promotion: their rows
revert to source-audit status, a lane-recorded `verified` downgrades fail-closed to `unverified`,
and the ledger embeds the effective deliveries with withdrawn claims absent and withdrawn
comparisons dropped from their claim. With no withdrawal record present the build is byte-identical
to a build without the channel.
