# #1240 Chord discriminator boundaries

## Scope

Ticket #1240 reviews the 15 Chord delivery claims bounded by #1239 because the
claimed discriminator is also one of the claim's comparison tests. Each receipt
now records the behavioral distinction that still needs an independent
frozen-scenario case. The comparison and existing discriminator are retained.

## Capability status

No row is promoted. The following claims remain bounded:

- #1178: `cch_chord:context:without-abort-signal`,
  `cch_chord:delta:batch-application`, `cch_chord:delta:immutable-application`,
  `cch_chord:delta:mutable-application`, `cch_chord:delta:overlap`,
  `cch_chord:delta:prepare-replace`, `cch_chord:state:attached-hydration`,
  `cch_chord:state:dispose`, and `cch_chord:state:listener-error`.
- #1185: `cch_chord:Chord:draft`.
- #1186: `cch_chord:Chord:json`, `cch_chord:json:copy-json`,
  `cch_chord:json:json-value`, and `cch_chord:json:strict-validation`.
- #1188: `cch_chord:state:delivery-order`.

A passing comparison alone does not establish the independent discriminator.
Each receipt's `remainingBoundary` names the observation and shallow behavior
still requiring separation. The #1240 revalidation records the shared chord
selection and its JUnit artifact for all four receipts.

## Validation

The chord CTest selection passed 89 of 89. Consolidation build and check passed;
the receipt boundary check passed 32 receipts, 808 claims, and 966 comparisons.
The full build passed, and the unfiltered CTest suite selected 4,560 tests:
4,549 passed and 11 skipped. Format and whitespace checks passed.
