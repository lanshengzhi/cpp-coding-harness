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
