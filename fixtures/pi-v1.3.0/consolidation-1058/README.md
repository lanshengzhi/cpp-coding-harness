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

## Ticket mappings (proposal E1 of #1183)

The plan's checked-in `ticketMappings` section retargets consolidated rows whose recorded
ticket reference is null or the `#1058-draft` placeholder onto a real issue, per the #1183
reconciliation proposal (`docs/wiki/pi-alignment/reconciliation-1183-proposal-2026-11-04.md`
section 2) and the owner decisions of 2026-11-04. Each entry names the consolidated row ids
(stable ledger ids), the target issue, a `history` marker, and a note recording the proposal
group and its authority. A `history: true` entry marks rows the proposal classifies as history
references: the behavior was delivered by a closed slice and the note names that slice, while
the target issue owns the live verification obligation; the reference itself claims no
acceptance and leaves `verification`, `evidence`, `gaps` and `sourceRows` untouched.

`build` applies a mapping only after the delivery updates, and only to a row whose ticket is
still null/`#1058-draft`; `check` revalidates the section against the committed ledger — every
mapping must reference an existing row, no row may carry both a mapping and a delivery-backed
ticket (a conflict fails closed), and each mapped row's committed ticket must equal its mapped
issue. Rows that still carry no reference stay allowed: `check` counts and reports them (the
build does not fail while rows legitimately await new tickets). With no `ticketMappings`
section — or an empty `entries` list — the build is byte-identical to a build without the
channel.

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

## Receipt artifact anchoring (#1202 boundary check)

`scripts/evidence/receipt_boundary.py` closes the drift class #1202 named (issue #1202
acceptance: a boundary test fails when a receipt cites a test that never opens the artifact its
rows point at). For every active receipt in `1058-deliveries.json` it parses each claim into its
comparisons (artifact plus cited test source) and discriminating case, then verifies against the
selected `--baseline` registry that every artifact is a registered evidence artifact and that the
cited C++ source references it in code — not in a comment, a Catch2 tag or a scenario key.
Helper headers a test quoted-includes under `tests/` count as part of the cited source. The
matcher is static name + path matching (the 2026-11-04 manual heuristic, minus comment matches),
exposed as an exchangeable `SourceMatcher` layer so a stronger AST- or regex-based reader can
replace it. Unparsable claims, unregistered artifacts, missing sources and baseline mismatches
fail closed; any failing receipt exits non-zero. Whole-record withdrawals (#1104, #1189) are
inactive and unchecked; per-claim withdrawals remain in the full receipts and are checked like
every other claim. Run: `python3 scripts/evidence/receipt_boundary.py [--baseline pi-v1.3.0]
[--receipt <id-or-ticket>]`.
