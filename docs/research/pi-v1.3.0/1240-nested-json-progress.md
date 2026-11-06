# #1240 nested JSON discriminator progress

## Scope

Ticket #1240 remains open: its owner requires at least eight promotions. None
of the 15 claims is promoted by this partial delivery.

## Observation

The registered `audit-1057-typed-json/chord-typed-json.json` scenario
`copyJson rejects a non-finite number nested below the root` records an
`outer.inner` array containing a NaN placeholder and the rejection message.
The added regression reads that input, replaces the placeholder with a
representable NaN, calls `copy_strict_json`, and compares the error to the
frozen message with the existing initial-letter normalization.

A root-only validator would fail this case, but would also fail the existing
comparison `strict JSON rejects non-finite numbers at any depth`. Therefore
the new case does not distinguish an implementation that the comparison
would tolerate, and is not cited as a parity discriminator. The strict
validation claim remains bounded.

The four receipts #1178, #1185, #1186 and #1188 pin the changed test source;
their 91-case Chord selection was rerun and the updated pin and previous
validation are recorded in `revalidation1240NestedJson`.

## Remaining work

All 15 claims remain bounded. The owner-supplied candidate scenarios and seam
map are recorded in issue #1240 and the prior seam probe. Each claim needs a
distinct artifact-backed discriminator that separates its property, with its
affected receipts revalidated. In particular, the existing masked-context
cancellation-order regression is not a frozen discriminator. For strict
validation, establish which frozen observation a root-only check would pass in
the original comparison, or obtain an owner ruling on the requested extraction
when the original comparison already tests the same rejection.

## Validation

The focused CTest passed (1/1), the Chord selection passed (91/91), and
consolidation build/check and receipt boundary passed. Frozen-evidence
verification passed 84 bundles, 180 artifacts and 1012 scenarios. The final
full build and unfiltered CTest run passed: 4562 selected, 4551 passed and 11
skipped, including the consolidation boundary and named-evidence tests.
Format and whitespace checks passed.
