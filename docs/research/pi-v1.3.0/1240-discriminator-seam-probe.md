# #1240 discriminator seam probe, 2026-11-06

## Findings

The #1246 correction is present at `d102853bb` on `origin/main`. The 15
claims remain bounded; this probe does not promote any of them.

The existing public Chord interfaces and frozen scenario reader provide the
candidate test seams. In `tests/chord/StrictJsonTest.cpp`, the comparison
`strict JSON rejects non-finite numbers at any depth` opens both registered
JSON scenarios and exercises `copy_strict_json` and `is_strict_json_value`.
Its nested value is the frozen `{outer:{inner:[1, NaN]}}` observation. A
separate case can compare the nested rejection and message to the typed-JSON
scenario, rejecting an implementation that checks the root alone. The
`chord JSON admission rejects a non-finite support value at any depth` case
also opens that scenario, but exercises `Json::from_support`; it does not
establish the separate `copy_strict_json` or `is_strict_json_value` public
interfaces, which have independent implementations in `src/chord/StrictJson.cpp`.

The frozen alias comparison `copy strict JSON keeps equal sibling members
independent of the source` already mutates a copy and observes source and
sibling. An independent case must retain those frozen comparisons and target
the claimed copy behavior, rather than merely reuse its name. Likewise, the
existing context test `a masked context retains its value when its parent is
cancelled later` adds an ordering not recorded in the frozen context scenario;
the owner has ruled it out as a parity discriminator.

The four common receipts #1178, #1185, #1186 and #1188 each pin
`StrictJsonTest.cpp`, `DeltaTest.cpp`, `ReplicatedStateTest.cpp`, and
`ChordContextTest.cpp`. Receipt #1184 additionally pins `ChordContextTest.cpp`.
Changes to any of these sources require review and revalidation of the
corresponding receipts, not just the receipt whose claim is promoted.

## Remaining work

Confirm the public Chord interfaces with the frozen scenario reader as test
seams, then add separate discriminating cases, verify each against its frozen
observation and named shallow implementation, and revalidate all affected
receipts. The candidate mappings in the owner comment on #1240 remain to be
checked case by case. No test, build, receipt update, or promotion was performed
in this probe.
