# #1115 application configuration schemas, tokens and reload

## Scope

Batch B57 / F21b, under #1044 and ADR 0075. Named Baseline: `pi-v1.3.0`,
revision `71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64/glibc.

This slice publishes the consumable `settings.json` field surface, makes the
settings failure text pi's `collectSettingsDiagnostics` text, validates the two
compaction token settings with pi's own rejection text, keeps unknown keybinding
ids in the migrated user map, and states the theme token set against a new
frozen capture. It is the schema/token/reload half of F21; prompt quoting,
resource selection and print-mode truncation already landed under #1114/#1117.

Frozen evidence: the registered
`fixtures/pi-v1.3.0/audit-1049/configuration-schema.json` scenarios
`settings schema accepts the supported shape and unknown top-level keys`,
`settings schema rejects invalid thinking level and negative compaction reserve`,
`settings manager reads package and local resource paths and reloads changed
values`, `keybindings manager migrates legacy names, keeps unknown actions,
drops invalid values and reloads` and `theme schema accepts the 56-token
built-in shape`.

The registered 1049 family had no observation of what pi *prints* when a
`settings.json` cannot be loaded, so the C++ diagnostic text had no comparison
target. Rather than derive it from C++, this slice adds the new candidate family
`fixtures/pi-v1.3.0/audit-1115/settings-diagnostics.json` with seven captured
scenarios: the malformed-file diagnostic template and its reported path, the
trust gate around the project scope, the dedupe rule, the schema error paths and
messages for every rejected shape this build publishes, the excluded
schema-versus-runtime divergence, and pi's compaction-consumer rejection text.

Reused rather than rewritten: #1104/#1105 delivered loadout and resource
loading, #1110's active tool set, #1118's codemode catalog (which is the
`codemode.mode`/`inlineBudget` consumer this slice exercises), #1112's native
extension lifecycle (untouched), and the existing `SettingsManager` two-scope
load/write path. `src/durable/*`, `src/mcp/*` and
`src/coding_agent/extensions/*` were not edited.

## Delivered behavior

- **Published schema surface.** New
  `cch::coding_agent::settings_schema_fields()` publishes exactly the 32
  `settings.json` paths a consumer in this build reads (19 top-level values, the
  `compaction`/`retry`/`codemode`/`terminal` containers and their 9 leaves),
  and `validate_settings_schema()` validates one document against that table
  using pi's `SettingsSchema.Check` vocabulary: pi's `instancePath`
  (`/defaultThinkingLevel`, `/compaction/reserveTokens`, `/enabledModels/0`) and
  pi's TypeBox message (`must be string`, `must be boolean`, `must be integer`,
  `must be >= 0`, `must be array`, `must be object`,
  `must be equal to one of the allowed values`, `must match a schema in anyOf`).
  An unknown top-level key and an unknown nested member are accepted
  (`additionalProperties: true`), matching pi and the existing lossless surgical
  write. Third-party `packages`/`extensions` markers and the resource-path lists
  `skills`/`prompts`/`themes` are not published because no consumer in this
  build reads them; a schema entry without a consumer would promise behavior the
  runtime drops. This deliberately departs from the ledger row's earlier note
  ("including the selected local resource path fields (`skills`, `prompts`,
  `themes`)"): the 2026-10-30 owner reassessment says to reuse current runtime
  consumers and add no excluded settings, and no consumer here reads those
  three, so publishing them would break the "only consumable fields" property.
- **Settings failure text.** `settings_diagnostic_text()` formats pi's
  `Invalid settings file <path>: <message>`. Session assembly warning
  diagnostics, the failed-creation error context and the interactive startup
  error all use it, replacing the harness-private
  `could not load global/project settings: <message>`. The reported path is the
  scope's file, not the scope name.
- **Compaction token validation.** `parse_compaction_settings` now rejects a
  present `reserveTokens`/`keepRecentTokens` that is not a non-negative safe
  integer with pi's own
  `Invalid compaction.<field> setting: <value>. Expected a non-negative safe
  integer.` text. pi raises it from `getCompactionTokenSetting` at resolution
  time; this build has no exception to raise from a getter, so the same check
  runs at the load boundary and fails the scope, which resolves as empty. The
  frozen capture also shows pi does *not* validate `compaction.enabled`, so a
  mistyped `enabled` still falls back to the pi default.
- **Keybinding migration and user-map retention.** The pi
  `KEYBINDING_NAME_MIGRATIONS` table (59 legacy spellings, matching the frozen
  source entry for entry) is applied on every read; the canonical id wins when the document carries both; `$schema` is
  skipped; a UTF-8 BOM is stripped before parsing; a value that is not a
  `KeyId`/`KeyId[]` or carries an unparsable key identifier is dropped with a
  diagnostic. Every valid entry is retained in the new
  `KeybindingsManagerResult::user_bindings` in pi's order (known catalog first,
  then the remaining ids sorted), which is pi's `getUserBindings`. Unknown and
  known-but-unassembled ids stay in that map and are never installed in the
  registry; the existing `unknown_action`/`unavailable_action` diagnostics are
  retained because this build assembles 85 of pi's 90 catalog actions. A reload
  replaces the map wholesale, exactly as pi's `reload` does.
- **Theme token set.** `theme_token_fallback()` exposes pi's
  `ThemeTokenDescriptor.fallback` so the count, the required count and the
  optional set can be compared with the frozen capture instead of restated. The
  set was already 56 tokens / 51 required; only the stale test comment naming
  three optionals was wrong.
- **Excluded upstream behavior.** pi's manager consumes a schema-rejected
  `defaultThinkingLevel` (`"bogus"` comes back as stored). This build keeps
  rejecting it at load and surfaces the rejection as a scope load error, per the
  ticket's reassessment. The frozen scenario records the upstream behavior so
  the divergence stays named rather than re-derived.

## Validation

- Build: `cmake --build --preset vcpkg` (incremental, default Debug preset),
  including the `PARITY-4011` include-evidence reconfigure.
- Focused Validation, `ctest --preset vcpkg -L issue1115`: 16 selected, 16
  passed, 0 failed. The selection is the ten settings-schema/diagnostic cases,
  the keybinding migration, canonical-wins, BOM/`$schema` and 90-action-catalog
  cases, the codemode runtime/reload case, the theme token-set case and the
  project-scope diagnostic case.
- Owning interaction, `ctest --preset vcpkg -R
  'settings|Settings|keybinding|Keybinding|theme|Theme'`: 229 selected, 229
  passed. `ctest --preset vcpkg -L cli`: 291 selected, 291 passed.
- Architecture, `ctest --preset vcpkg -L architecture`: 43 selected, 43 passed
  (78 s). A first run immediately after the cold build reported
  `cch_warning_gate` failing (its 900 s timeout against a cold object cache, all
  631 project compile commands otherwise recompiling cleanly); the warm rerun
  passed all 43 and the full-suite run below re-ran the same gate inside the
  unfiltered selection.
- Full Validation on the reviewed tree, `TERM_PROGRAM=herdr`,
  `cmake --build --preset vcpkg` then `ctest --preset vcpkg -j1`: 4,194
  selected, 4,183 passed, 0 failed, 11 skipped (793 s). Two earlier runs on the
  same inputs are not the recorded result: the first (4,191 selected) failed the
  two consolidation gates on the pre-revalidation digests, and the second
  (4,194 selected) was polluted by a concurrently running architecture
  selection and failed five tests that all passed on isolated rerun
  (`[issue1019]` PTY EIO, `issue990` terminal colors, the `[quarantine]` #617
  projection cost bound, `issue1089` file/watch outcomes and `[issue1170]`
  nested tool hooks). The 11 skips are the nine frozen-pi durable continuation
  cases, the realtime-clock wrapper and the Native TUI differential capture,
  none of which this slice touches.
- Formatting: `scripts/format-check.sh origin/main` clean.
- Pinned-receipt revalidation on checkout base
  `ba45599a1d9249b73992a5fa92aa45da1f9db05a`, with fresh JUnit artifacts:
  #1101/#1104 shared selection 36/36
  (`1115-revalidation-1090-1101-1104.xml`), #1118 19/19
  (`1115-revalidation-1118.xml`), #1181 16/16
  (`1115-revalidation-1181.xml`). Each record keeps its rows, scenarios and
  routing digest, gained a `revalidation1115` entry, and re-pinned only
  `cmake/tests/CodingAgentTests.cmake`, `src/coding_agent/runtime/
  SessionFactory.cpp` and `tests/coding_agent/CodemodeCatalogSessionTest.cpp`.
  The #1118 receipt's scenario mapping is unchanged: the new
  `CodemodeCatalogSessionTest` case is tagged `[issue1115]`, so the
  `issue1118` selection still counts 19.
- Consolidation: `consolidate_audits.py build` wrote 1,464 rows;
  `consolidate_audits.py check` passed (41 rows backed by pinned comparisons,
  594 scenarios, 176 explicit orphans, 0 uncovered).
  `tests/evidence/ConsolidationAuditTest.py`: 42 tests, all passed.
- Frozen evidence: `audit_evidence.py check` accepts the unregistered
  `audit-1115` manifest
  (`537b857f29b6a1f7a4af17b9f7beaa290005eb7c6da64416753a37aafd56c576`);
  `audit_evidence.py verify` passes 50 bundles / 106 artifacts / 597 scenarios
  at the full frozen revision. Registration stays with the serial merger; the
  capture command and the exact registry record are below.

## Frozen capture and registration handoff

```bash
# Capture (private checkout at the named baseline revision, clean tree)
PI_CHECKOUT=<pi-checkout> PI_BASELINE=pi-v1.3.0 \
  <pi-checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1115/capture-1115.mts
# captured audit-1115 (1 artifact, 7 scenarios) for pi-v1.3.0 (71023aec…)

# Candidate validation
python3 scripts/evidence/audit_evidence.py check \
  --manifest fixtures/pi-v1.3.0/audit-1115/manifest.json   # accepted (record below)
```

Registry record for the serial merger:

```json
{
  "audit-1115": {
    "manifest": "audit-1115/manifest.json",
    "manifestSha256": "537b857f29b6a1f7a4af17b9f7beaa290005eb7c6da64416753a37aafd56c576"
  }
}
```

The seven new scenarios need `consolidation-plan.json` orphans once the bundle
is registered, because no delivery receipt claims a whole row from them:

| # | Scenario | Related rows |
| --- | --- | --- |
| 1 | a malformed global settings file yields one warning naming the file path | `cch_coding_agent:settings-runtime` |
| 2 | a malformed project settings file reports its own path only while the project is trusted | `cch_coding_agent:settings-runtime` |
| 3 | the same settings file diagnostic from two managers deduplicates to one message | `cch_coding_agent:settings-runtime` |
| 4 | the settings schema rejects an invalid thinking level and a negative compaction reserve naming the key | `cch_coding_agent:settings-schema` |
| 5 | the settings schema names the offending path and pi message for each rejected shape | `cch_coding_agent:settings-schema` |
| 6 | a schema-rejected thinking level loads without a diagnostic and the runtime keeps the raw value | `cch_coding_agent:settings-runtime-validation` |
| 7 | the compaction consumer rejects negative and fractional token settings naming the field | `cch_coding_agent:settings-runtime` |

Reason for each: captured by #1115 against the frozen checkout after #1058
because the original 1049 audit observed the schema/runtime shapes but not the
diagnostic text; disposition: #1115 compares this slice and #1177 owns the
application receipt follow-up, so the observation does not promote a whole row.

## Comparison map

| Frozen scenario | C++ comparison |
| --- | --- |
| `audit-1049` "settings schema accepts the supported shape and unknown top-level keys" | `SettingsSchemaTest`: `settings schema accepts every published field and an unknown top-level key` |
| `audit-1049` "settings schema rejects invalid thinking level and negative compaction reserve" | `SettingsSchemaTest`: `settings schema rejects the two frozen invalid shapes with pi's path and message` |
| `audit-1049` "settings manager reads package and local resource paths and reloads changed values" | `CodemodeCatalogSessionTest`: `a settings document the schema accepts reshapes the request and reload re-reads it` for the consumed runtime half, and `SettingsManagerTest`: `SettingsManager reload re-reads both scopes and re-records errors` for the capture's `afterReload.defaultModel`. The `packages`/`skillPaths`/`promptPaths`/`themePaths` fields stay excluded, so their expectations are not compared. |
| `audit-1049` "keybindings manager migrates legacy names, keeps unknown actions, drops invalid values and reloads" | `KeybindingsManagerTest`: `Keybindings manager migrates legacy names, keeps unknown actions and reloads` (`appInterrupt`/`appClear`/`cursorUp`, `appUnknownPresent`, `userBindings`, `conflicts`) and `the assembled catalog plus the known-unassembled actions is pi's 90-action catalog` (`catalogSize: 90`) |
| `audit-1049` "theme schema accepts the 56-token built-in shape" | `ThemeTest`: `the theme token set matches the frozen pi-v1.3.0 capture` (`tokenCount`, `requiredTokenCount`, `optionalTokens`) |
| `audit-1115` 1 (diagnostic template and path) | `SessionAssemblyContractTest`: `settings load errors surface as diagnostics ahead of resource diagnostics`; `SettingsSchemaTest`: `the settings diagnostic text is pi's file-and-message template`; `CliSmokeTest`: `CLI gateway success after malformed settings keeps the warning visible` and `CLI malformed settings keep default session storage with a warning` |
| `audit-1115` 2 (project path and trust gate) | `SessionAssemblyContractTest`: `a malformed project settings file is reported only while the project is trusted` |
| `audit-1115` 3 (dedupe) | Not reproducible on this path: one `SettingsManager` per creation attempt cannot report the same file twice. `settings load errors surface as diagnostics ahead of resource diagnostics` asserts the single-diagnostic case. |
| `audit-1115` 4-5 (schema paths and messages) | `SettingsSchemaTest`: `settings schema rejects the two frozen invalid shapes with pi's path and message`, `settings schema names the offending path and pi message for every rejected shape`, `a malformed container reports one violation and no leaf violations` |
| `audit-1115` 6 (excluded divergence) | `SettingsSchemaTest`: `a schema-rejected thinking level fails the load instead of reaching the consumer` |
| `audit-1115` 7 (compaction consumer text) | `SettingsSchemaTest`: `a negative or fractional compaction token fails the scope load with pi's text` |

The `audit-1049` scenario-1 expectation (`supportedCheck: true` over pi's key set)
is not compared field for field because that set contains the intentionally
excluded `packages`/`extensions`/resource-path keys; the compared property is the
one the scenario names — the supported shape validates and an unknown top-level
key stays legal.

Discriminating cases, one per acceptance criterion:

- *Schema promises only consumable fields.* The independent expected table in
  `the published settings schema lists exactly the fields this build consumes` fails
  if a published key has no typed reader and if a loaded field is unpublished;
  `a settings document the schema accepts loads into the typed fields it
  publishes` fails if any published leaf is silently dropped.
- *A schema-accepted value reaches the real runtime and reload.* The codemode
  case is the stale-value case: if `reload()` did not re-read `codemode.mode`,
  the second request would still declare `read` directly and the catalog would
  not list it.
- *Invalid diagnostics are comparable.* The same document is compared against
  pi's `instancePath` and message text, so a respelled message separates; the
  compaction case compares pi's exception text verbatim.
- *A malformed container is one finding.*
  `a malformed container reports one violation and no leaf violations` asserts
  the exact violation count, which separates a leaf resolved through a
  non-object container (an earlier draft did exactly that and reported
  `/compaction/reserveTokens` alongside `/compaction`).
- *Keybinding migration.* The legacy-only document (migrated), the
  legacy-plus-canonical document (canonical wins) and the reload document
  (map replaced) separate a reader that ignores migration or keeps stale
  entries; the conflict case checks both claimants in pi's order, and the
  catalog case separates a silently dropped assembled action.
- *Theme token set.* The optional set is read from the fallback declarations
  rather than restated, so a renamed or dropped optional token separates.

## Two-axis review

Standards: one P1 and four P2 findings, all fixed. The P1 was
`SettingsSchemaField` lacking default member initializers in the Owner
Interface header (`CODING_STANDARDS.md` §4.1). The P2s: the keybinding migration
table was a function-static `std::vector` instead of a `constexpr` array with a
`std::span` view (§9.1); the new scans used iterator pairs instead of
`std::ranges::*` (§9.5); the compaction safe-integer bound was written twice
(now one `kMaxSafeInteger` predicate shared by the loader and the schema
validator); and `read_binding_keys` used two mutable out-parameters, replaced by
a `BindingValue` value.

Spec: two missing-or-partial items and three uncompared expectations. The
resource-path omission the ledger note recommended is kept and the record now
names the reassessment that overrides it. The project-scope observation is now
compared by a session-level case (trusted/untrusted). All three "uncompared"
items were real: `find_dotted` resolved a leaf through a non-object container
and reported a spurious second violation (fixed, with an exact-count case); the
migration table has 59 entries, not the 60 the record claimed (corrected); and
the keybinding conflict claimants, the 90-action catalog size, the manager-level
reload value and the audit-1115 project-scope assertions are now compared, with
the dedupe scenario recorded as not reproducible on this path rather than left
implicit.

## Capability status

No membership change and no row promotion. `cch_coding_agent:settings-schema`
and `cch_coding_agent:settings-runtime` stay `partial` / `upstream-captured`:
this slice supplies their first C++ comparisons and a new captured family, but
the schema row's consumer is pi's *generated* JSON Schema artifact and the
runtime row's `packages`/resource-path half stays excluded, so neither is a
whole-row verification. #1177 owns the application-wide receipt that can
promote them. `settings-runtime-validation` stays excluded.

## Remaining boundary

- **Project-scope observations are a source claim, not a capture.** The
  frontier's `settings-runtime` row already records that the project-scope
  settings half has no frozen observation; this slice adds the project-scope
  *diagnostic* (malformed file, trust gate) but no project-scope
  value-resolution capture. Do not widen the row from it.
- **No duplicate diagnostic can arise on this path.** pi dedupes because a
  startup manager and a runtime manager read the same file; this build loads
  one `SettingsManager` per creation attempt, so the dedupe scenario is
  comparable at the text level only and no dedupe pass was added.
- **Interactive startup stays fatal.** A malformed global settings file stops
  the interactive boot with pi's text instead of printing a warning and
  continuing as pi's `main.ts` does. Changing that boot behavior needs
  interactive evidence and is not part of this text-alignment slice.
- **Keybinding catalog size.** pi's catalog is 90 actions; this build assembles
  85 and keeps the other five as `unavailable_action` diagnostics. The
  `user_bindings` order therefore places those five after the assembled entries
  rather than at their pi catalog positions.
- **The theme schema artifact is still not published.** The token set matches
  the frozen capture; a distinct `validateThemeJson`-shaped artifact remains
  unbuilt, and theme rendering outcomes stay with #1182.
- **`audit-1115` is a candidate bundle.** Registration and the plan orphans
  above are the serial merger's step; until then the consolidation plan does not
  count its scenarios.
