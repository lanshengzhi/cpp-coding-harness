# #1186 Chord typed JSON ingestion comparisons

## Scope

Close the typed-JSON comparison gap [#1178](https://github.com/lanshengzhi/cpp-coding-harness/issues/1178)
left open and register whole-row receipts for the representable `cch_chord` JSON rows. Authority:
[#1178](https://github.com/lanshengzhi/cpp-coding-harness/issues/1178),
[#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044),
[#1183](https://github.com/lanshengzhi/cpp-coding-harness/issues/1183),
[ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md),
Named Baseline **pi-v1.3.0** at `71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64 with glibc.

The blocker was representational, not observational. `support::JsonValue` cannot represent JavaScript
`undefined`, functions, class instances, sparse arrays, symbols, accessors or cycles, so the frozen
`copyJson` rejection vocabulary for those classes has no C++ carrier spelling. The delivered
`copy_strict_json`/`is_strict_json_value` and the immutable `chord::Json::from_support` admission
already reject the one non-strict class the carrier can hold: a non-finite `double` at any depth. This
slice captures the representable frozen observations, compares them, and records the carrier-excluded
classes explicitly instead of inventing a C++ expectation for them.

Bounded scope: `src/chord/StrictJson.cpp`, its Owner header and `tests/chord/StrictJsonTest.cpp`, plus
one supplemental frozen capture and the receipt/ledger inputs. `src/durable`, `src/coding_agent` and
`src/mcp` are untouched; the durable text/store boundary stays with the durable packet (#1180). No
production behavior changed: the #1060 delivery already implements the typed value contract, and this
ticket is its comparison receipt.

## Carrier decision

The C++ ingestion contract is a **typed value admission**, not a JSON-text parser:

- `copy_strict_json(const support::JsonValue&)` returns an alias-free deep copy and rejects a
  non-finite number at any depth with `value contains a non-finite number and is not strict JSON`.
- `is_strict_json_value(const support::JsonValue&)` is the same finite-at-depth predicate without the
  copy.
- `chord::Json::from_support` is the immutable durable-value admission: it rejects the same
  non-finite class and returns a value that owns its containers.

`support::JsonValue` is the carrier and already excludes the JavaScript-only classes by construction.
The other candidate — a validation pass over decoded JSON **text** — would require changing the shared
Glaze-compatible parser (`support::read_json` deliberately ignores trailing content and is pinned by
other Owners' receipts). That text seam belongs to the durable storage/transaction boundary and is not
part of this typed value contract; #1180 owns its strict text admission. This slice therefore takes
the issue's candidate (b): an explicit reviewed carrier-exclusion statement plus the remaining
rejection class captured where it is still representable.

The discriminating cases that apply to this contract are therefore the alias-retention case and the
nested non-finite case. A typed value cannot accept trailing content because it is not text; the
tolerant text parser is not claimed as the strict boundary.

## Captured observation

One supplemental family, `audit-1057-typed-json`, is registered in
[baselines.json](../../../fixtures/pi-v1.3.0/baselines.json) and pinned by
[manifest.json](../../../fixtures/pi-v1.3.0/audit-1057-typed-json/manifest.json) at the same
`pi-v1.3.0` revision. It was captured by
[capture-1057-typed-json.mts](../../../fixtures/pi-v1.3.0/audit-1057-typed-json/capture-1057-typed-json.mts)
from the clean frozen checkout through the public `packages/chord/src/json.ts` exports.

| Scenario | Frozen observation | C++ carrier |
| --- | --- | --- |
| `copyJson copies strict JSON without retaining aliases` | A shared input object becomes independent `left` and `right` copies; mutating the copy leaves the source and sibling unchanged. | Fully representable; mutation separation replaces JavaScript reference identity. |
| `copyJson preserves null prototypes and own __proto__ data properties` | A null-prototype object with an own `__proto__` data property copies to an object with the same own property. | The own `__proto__` member is representable and compared; the null-prototype half is excluded by the carrier. |
| `copyJson rejects a non-finite number nested below the root` | `{outer:{inner:[1,NaN]}}` fails with `Value contains a non-finite number and is not strict JSON`. | Fully representable and compared at depth. |
| `isJsonValue checks strict JSON without normalizing it` | A pure nested value is accepted; `undefined`, `Uint8Array`, `Infinity` and a cycle are rejected. | The strict and `Infinity` cases are representable; the other classes are excluded by the carrier. |

The capture keeps the JavaScript-only cases visible in its inputs/results. They are not normalized
into C++ expectations:

| Frozen class | Why it has no C++ case |
| --- | --- |
| `undefined` object property / array entry | The `JsonValue` variant has no undefined alternative; an absent member is already absent. `omitUndefinedProperties` is vacuous at this carrier. |
| function, class instance, `Uint8Array`, symbol key | The variant has no non-plain-object alternative; the type system excludes them before a value can reach the seam. |
| sparse array, extra/non-enumerable/accessor array property | The array is a dense `std::vector`; there is no hole or property descriptor to inspect. |
| cycle | `JsonValue` is a value tree and cannot contain a back-reference; a cycle has no constructible C++ value. |
| null prototype | C++ objects have no prototype chain; only the own `__proto__` data member is observable. |

## Comparison mapping

One delivery record, `1186-chord-typed-json`, in
[1058-deliveries.json](1058-deliveries.json) pins the production, test, build, fixture and result
inputs. Every claimed row has an empty remaining boundary.

| Row | Frozen artifact and scenario | Comparison case | Discriminating case |
| --- | --- | --- | --- |
| `cch_chord:json:copy-json` | `audit-1057/json-output-support.json`: `copyJson returns a detached strict-JSON value and omits undefined object properties`; `audit-1057-typed-json/chord-typed-json.json`: alias-free copy and own `__proto__` data member | `copy strict JSON returns a detached copy of the frozen value`<br>`copy strict JSON keeps equal sibling members independent of the source`<br>`copy strict JSON preserves an own __proto__ data member` | `copy strict JSON keeps equal sibling members independent of the source` |
| `cch_chord:json:strict-validation` | `audit-1057/json-output-support.json`: `copyJson rejects functions, undefined array entries, non-finite numbers, class instances, and sparse arrays`; typed-json: nested non-finite | `strict JSON rejects non-finite numbers at any depth` | `strict JSON rejects non-finite numbers at any depth` |
| `cch_chord:json:json-value` | `audit-1057-typed-json/chord-typed-json.json`: `isJsonValue checks strict JSON without normalizing it` | `is strict JSON checks a direct JsonValue model without normalizing it` | same case |
| `cch_chord:Chord:json` | `audit-1057-typed-json/chord-typed-json.json`: `copyJson copies strict JSON without retaining aliases` | `chord JSON admission detaches the support value and preserves immutable ownership` | same case |

All cases live in `tests/chord/StrictJsonTest.cpp`, inside the pinned `-L chord` selection.

`cch_chord:json:copy-json` compares every representable frozen field. The frozen `copied`,
`copiedIsDetached` and `sourceAfterNestedPush` fields are compared by mutating the copy in the frozen
direction and checking the source and sibling copies; `copiedHasOmit` is vacuous because the carrier
has no `undefined` member to omit. The alias scenario's `copiedIsInput`, `leftIsShared`,
`rightIsShared` and `leftIsRight` reference-identity fields have no C++ spelling in a value carrier;
the compared property is mutation separation, which is the same ownership fact. The own-`__proto__`
scenario compares the own key and value; the null-prototype field is excluded by the carrier. The
discriminating case requires a copy that retains an alias to fail: mutating the copy's `left` must
leave `right` and the source at `1`.

`cch_chord:json:strict-validation` compares the one frozen rejection class the carrier can represent
and adds a deeper frozen observation. A validator that only checked the root would accept
`{outer:{inner:[1,NaN]}}`; the case requires rejection at depth with the exact frozen message. The
function/undefined/class-instance/sparse-array messages are listed in the capture but cannot be
constructed as `JsonValue`; the report records that exclusion instead of asserting an invented
message.

`cch_chord:json:json-value` gives the row its direct `JsonValue` model scenario. The strict nested
value is accepted and preserved field by field, while the non-finite value is rejected. A validator
that normalized the non-finite value to `null` would return true and fail the case. The
`undefined`/`Uint8Array`/cycle results are captured but excluded by the carrier.

`cch_chord:Chord:json` is the coarse 1054 view of the same ownership contract. The comparison case
admits a support value through `chord::Json::from_support`, mutates the source after admission, and
requires the admitted value to keep its own data; a copy of the admitted value shares the immutable
container, while an independently admitted value does not. This is the C++ spelling of the frozen
alias/detachment observation. The durable store read/write path that consumes the admitted value is
the `document:*` obligation owned by #1180, not a Chord row.

## Reviewed source-audit correction

The 1054 source row `cch_durable:Chord:json` declared the placeholder
`C++ JSON value ownership across a durable store` with `artifact: null`. Its own gaps say the
durable-required copy/ownership contract is audited with #1057, and the
`group-durable-chord-subset` plan keeps the 1054 rows as the coarse view of the 1057 detail rows.
The source row is re-paired, in
[1054-durable-session-documents-storage.json](1054-durable-session-documents-storage.json), to the
registered `audit-1057-typed-json` alias-detachment scenario, and its recorded result is the captured
detachment observation instead of the "not captured" placeholder. This is a reviewed re-pairing of
one coarse row, not a derived observation and not a membership change. The durable
`materializeDocument`/store behavior named by the row's source stays in the durable packet (#1180).

The supplemental family is also linked to the three 1057 detail rows through
`scenarioLinks` in [consolidation-plan.json](../../../fixtures/pi-v1.3.0/consolidation-1058/consolidation-plan.json).
The 1057 source audit's own statuses and captured observations are preserved as history.

## Review

Two-axis review (Standards and Spec, independent sub-agents) ran on the implementation commit.

| Finding | Disposition |
| --- | --- |
| Standards: the new test helper had a non-descriptive name and a mutable-only overload | Fixed: `object_of` overloads for mutable and const values |
| Standards: formatted `TEST_CASE` signatures exceeded the line width | Fixed: `git clang-format` |
| Spec: the alias/`__proto__` scenarios were not linked to a row | Fixed: `scenarioLinks` for all four captured scenarios |
| Spec: the 1054 coarse row still carried its placeholder scenario | Fixed: reviewed re-pair to the captured alias-detachment scenario |
| Spec: `json:json-representation` has no runtime observation | Reviewed and kept unverified: the row is a compile-time typing obligation and no frozen runtime scenario exists; no expectation was invented. |
| Spec: the text parser tolerates trailing content | Reviewed and kept out of scope: the typed value admission is not a text parser; strict text admission belongs to the durable boundary (#1180). |

## Validation

Measured on this ticket's implementation commit `4f548771c5f866c86624fb48e7e1977be84f2942`,
checkout `4f548771c`, with the pinned vcpkg toolchain:

| Command | Result |
| --- | --- |
| `cmake --preset vcpkg` | configured; build-phase Parity Architecture Gate PASS |
| `cmake --build --preset vcpkg --target cch_tests_chord` | built; build-phase Parity Architecture Gate PASS |
| `ctest --preset vcpkg -N -L issue1186` | 7 selected |
| `ctest --preset vcpkg -L issue1186` | 7/7 passed |
| `ctest --preset vcpkg -N -L chord` | 81 selected |
| `ctest --preset vcpkg -L chord --output-junit docs/research/pi-v1.3.0/deliveries/1186-chord.xml` | 81/81 passed, 0 failed, 0 skipped |
| `ctest --preset vcpkg -L architecture` | 43/43 passed (280.14 s) |
| `cmake --build --preset vcpkg` then `TERM_PROGRAM=herdr ctest --preset vcpkg -j1` | 4,228 selected, 4,218 passed, 0 failed, 10 skipped (643.32 s) |
| `python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py build` | ledger rebuilt; 54 delivery-backed rows |
| `python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py check` | PASS — 649 scenarios, 366 named by a row, 56 covered by a referenced artifact, 227 explicit orphans, 0 uncovered |
| `python3 tests/evidence/ConsolidationAuditTest.py` | 42 tests, all passed |
| `python3 scripts/evidence/audit_evidence.py check --manifest fixtures/pi-v1.3.0/audit-1057-typed-json/manifest.json` | PASS |
| `python3 scripts/evidence/audit_evidence.py verify --baseline pi-v1.3.0` | 55 bundles, 115 artifacts and 652 scenarios verified |
| `scripts/format-check.sh 5ff7a14c9` | clean |

The receipt pins the exact JUnit bytes for the `-L chord` selection, so the selected count, case names
and scenario mapping cannot drift silently. The receipt also refreshes the registry/plan digests
pinned by earlier records; those records gain a light `revalidation1186m` entry with no row,
scenario or routing changes. #1185's pinned 1054 input is refreshed with a light `revalidation1186`
entry; the shared Chord selection rerun includes every case it claims.

## Capability status

Four rows move from their audit status to `implemented` / `verified` through the validated delivery
record: `json:copy-json`, `json:strict-validation`, `json:json-value` and `Chord:json`. All other
chord rows keep their recorded status, audit history and named remaining work. Membership is
unchanged at 65 `cch_chord` rows (60 supported, 5 excluded).

`json:json-representation` remains `missing` / `unverified`: it is a compile-time representation
mapping with no runtime scenario and no delivered C++ type-mapping seam. No frozen observation was
invented for it and no receipt claims it. This is a recorded remaining boundary, not a Deferred row.

## Remaining boundary

- `json:json-representation` — compile-time typing obligation; no runtime scenario exists and no C++
  mapping seam is delivered. It stays unverified.
- Strict text admission (trailing content after a complete JSON document) — the shared
  `support::read_json` is Glaze-compatible by contract; the durable storage/transaction boundary's
  strict text admission remains with #1180.
- Durable `materializeDocument`/store behavior — owned by the durable packet (#1180), as the 1054
  coarse row's source already says.

`#1178`, `#1183`, `#1044` and `#1186` keep their own states; closing this slice accepts no adjacent
row and no umbrella obligation. Magpie-only AI access, XDG state roots, strict no-exception behavior,
the ordinary application Session, all explicit AI/platform exclusions and the
no-historical-Pike-durable-migration boundary remain unchanged. SIGKILL evidence does not establish
power-loss or multi-process guarantees.
