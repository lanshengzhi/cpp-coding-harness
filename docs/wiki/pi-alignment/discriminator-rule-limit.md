# The discriminating-case rule and where it stops

Recorded 2026-11-06 at `lanshengzhi/cpp-coding-harness@72b6048fc`, after four attempts at #1240 returned zero promotions. The claim it touches is `cch_chord:json:strict-validation`, verified by reading the receipt rather than the reports.

## What the rule says

The delivery contract (from #1229, the retro review of receipts #1201–#1213) requires each claim to carry a `discriminatingCase`: a passing, pinned CTest case, distinct from the claim's `comparisons[].test`, that fails for a plausible shallow implementation. #1229 found 182 claims across seven receipts naming a case whose `test` string equalled one of their own comparison tests, and disposed of them by bounding the claims — validated, but not promoted.

#1239 then made the rule mechanical: `consolidate_audits.py` rejects an unbounded claim whose `discriminatingCase.test` appears among its comparison tests. That check bounded 44 more claims across 12 receipts (`1090` 4, `1118` 1, `1176` 3, `1177` 1, `1178` 9, `1179` 16, `1180` 2, `1185` 1, `1186` 4, `1188` 1, `1192` 1, `1194` 1).

## Where it is unsatisfiable

`cch_chord:json:strict-validation` in receipt `1186-chord-typed-json` carries:

- `comparisons`: `audit-1057/json-output-support.json` scenario `copyJson rejects functions, undefined array entries, non-finite numbers, class instances, and sparse arrays`, and `audit-1057-typed-json/chord-typed-json.json` scenario `copyJson rejects a non-finite number nested below the root`; both with test `strict JSON rejects non-finite numbers at any depth`;
- `discriminatingCase`: **the same test**, reason *"A validator that checked only the root would accept the nested non-finite value; the case requires the deep rejection with the delivered message … while the top-level cases also fail."*

The test opens the frozen nested scenario and asserts the deep rejection, so a root-only validator fails it. The reason names the shallow implementation. The evidence is sound; the rule refuses it only because the test name matches a comparison name. A second, *distinct* case cannot be written without asserting something frozen pi never recorded, which would be a worse violation than the one enforced.

## Why #1229's rule was too broad

#1229's actual defect was **vacuous** discriminating cases — a name repeated with no argument about what fails. The name-equality test was a proxy for that. A proxy cannot distinguish "the comparison is the discriminator and says why" from "the comparison is cited with no argument". Four lanes and 12 bounded claim classes later, the proxy has blocked work it was meant to protect.

## Owner decision: single-row pilot

The owner approved a pilot allowing the comparison test to serve as its own `discriminatingCase` when a pinned executable negative control demonstrates that a named shallow implementation fails the same frozen scenario and acceptance judgment. Mechanical checks bind the cited passing test and its source; review must establish their semantic connection. The pilot is `cch_chord:json:strict-validation`; its result does not establish that other claims qualify. See the [owner decision](https://github.com/lanshengzhi/cpp-coding-harness/issues/1240#issuecomment-6703651162).

If refused, the alternative is explicit: every claim whose comparison test is already sharp stays unverified permanently, across the 44 rows in #1240–#1243 and #1229's 182, unless a genuinely different frozen observation is captured for each.

Related: [evidence and delivery history](evidence-and-delivery-history.md), [ledger field staleness](ledger-field-staleness.md), [index](index.md).
