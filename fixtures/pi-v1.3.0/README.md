# pi-v1.3.0 audit evidence

Shared named-evidence registry and verifier for the frozen pi-v1.3.0 audit families
([spec #1044], tickets #1046–#1057). This extends the named-bundle discipline of
[ADR 0065](../../docs/adr/0065-bind-pi-ai-evidence-bundles-to-a-named-baseline-registry.md) and
`scripts/tui/evidence.py` to a multi-module audit: one baseline name, one full revision, one
pinned manifest per ticket family, and one verified read path. It is evidence infrastructure,
not a capability-alignment claim: registering a family records upstream observations as
`upstream-captured`; a C++ comparison is still required before a capability row is `verified`.

The baseline is **`pi-v1.3.0`** at **`71023aec1546d6d677a65669c5665c0927ce0dd0`**, Linux x86-64
with glibc, per [ADR 0075](../../docs/adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md).
The revision is written once, in [`baselines.json`](baselines.json). A tag, a version string, a
newer working tree or a short SHA is not comparison authority, and there is no free-form revision
argument anywhere in the capture, check, register, verify or read paths.

## Layout

```text
fixtures/pi-v1.3.0/
  baselines.json                    shared registry: baseline -> full revision -> family manifest pins
  README.md                         this convention
  audit-<ticket>[-<variant>]/       ticket-owned family bundle (one directory per family)
    manifest.json                   pinned index: capture generator, environment, artifact bytes, scenarios
    <artifact>.json                 captured observation envelope (one or more per family)
    capture-<topic>.mts             executable capture that produced the artifacts
scripts/evidence/audit_evidence.py  shared check / register / verify / read CLI
tests/evidence/NamedEvidenceTest.py public boundary test for the registry and verifier
```

A family is `audit-<ticket>` or a variant `audit-<ticket>-<variant>` (for example
`audit-1085-replay`) and belongs to one ticket; the primary family and its variants may coexist in
one baseline. The ticket owns its bundle directories; only the serial merger edits `baselines.json`
through `register`, and no lane may rewrite another family's bundle or the shared verifier. Audit reports and capability ledgers live
beside the ticket documents under `docs/research/pi-v1.3.0/`.

## Registry

```json
{
  "schema": "pike/pi-v1.3.0-audit-baselines/1",
  "default": "pi-v1.3.0",
  "baselines": {
    "pi-v1.3.0": {
      "label": "v1.3.0",
      "revision": "71023aec1546d6d677a65669c5665c0927ce0dd0",
      "platform": "linux-x86_64-glibc",
      "bundles": {
        "audit-1046": {
          "manifest": "audit-1046/manifest.json",
          "manifestSha256": "<sha256 of manifest.json>"
        }
      }
    }
  }
}
```

Every baseline records a full 40-character revision and a `bundles` object. Every registered
family pin names a manifest path relative to this directory and its exact SHA-256. `verify`
requires at least one registered family; a baseline with no captured bundle fails instead of
passing as an empty registry entry.

## Manifest

Every family has one `manifest.json` next to its artifacts. Its required fields are checked by
the verifier; unknown fields are permitted.

```json
{
  "schema": "pike/pi-v1.3.0-audit-evidence/1",
  "baseline": "pi-v1.3.0",
  "revision": "71023aec1546d6d677a65669c5665c0927ce0dd0",
  "ticket": "1046",
  "family": "audit-1046",
  "capturedAt": "2026-10-19T15:30:56.095Z",
  "environment": {
    "platform": "linux",
    "arch": "x64",
    "libc": "glibc",
    "glibcVersion": "2.44",
    "node": "v26.12.0",
    "tsx": "4.22.1"
  },
  "generator": {
    "path": "fixtures/pi-v1.3.0/audit-1046/capture-extension-example.mts",
    "sha256": "<sha256 of the capture script>",
    "command": "<exact capture command>",
    "sources": [{ "path": "fixtures/pi-v1.3.0/audit-1046/capture-extension-example.mts", "sha256": "<sha256>" }]
  },
  "artifacts": [
    {
      "path": "fuzzy-match.json",
      "family": "audit-1046",
      "bytes": 1970,
      "sha256": "<sha256 of the artifact bytes>",
      "sourceEndpoints": [
        "packages/tui/src/fuzzy.ts:fuzzyMatch,fuzzyFilter",
        "packages/tui/test/fuzzy.test.ts:fuzzyMatch,fuzzyFilter"
      ],
      "scenarios": [
        {
          "name": "empty query matches everything with score zero",
          "inputs": { "query": "", "text": "anything" },
          "expected": { "matches": true, "score": 0 },
          "normalizations": []
        }
      ]
    }
  ]
}
```

- `baseline` and `revision` must resolve to the one registered baseline: the revision is the
  full 40-character SHA, never a tag or short prefix.
- `ticket` is the leading numeric suffix; `family` is `audit-<ticket>` or
  `audit-<ticket>-<variant>` and unique within the registry.
- `capturedAt` is a UTC ISO-8601 timestamp ending in `Z`.
- `environment` records the actual capture host. Linux/x64/glibc is required; record the
  Node/tsx runtime versions used. Windows/macOS observations do not satisfy a selected capability.
- `generator.path` and `generator.sha256` bind the executable capture; `generator.sources` binds
  every scenario/helper source. `generator.path` and `sources[].path` are repository-relative and
  must hash to the recorded values; a changed capture without a new digest is a different capture.
- `artifacts[].path` is relative to the family directory, is not absolute, contains no `..`, and
  stays inside the bundle. `bytes` and `sha256` bind the exact committed bytes.
- `sourceEndpoints` use `packages/<...>:<symbol>` spelling, so a capture that names no frozen
  source or upstream test is refused. They are provenance, not proof that behavior is covered.
- Every artifact carries at least one scenario with a non-empty unique `name`, non-empty
  `inputs`, a non-null `expected` observation and declared `normalizations`.

## Artifact envelope

Each artifact repeats the binding metadata and carries the observations (the worked example is
[`audit-1046/fuzzy-match.json`](audit-1046/fuzzy-match.json)):

```json
{
  "schema": "pike/pi-v1.3.0-audit-evidence/1",
  "baseline": "pi-v1.3.0",
  "revision": "71023aec1546d6d677a65669c5665c0927ce0dd0",
  "capturedAt": "<same value as the manifest>",
  "environment": { "<same value as the manifest>" },
  "sourceEndpoints": ["<same list as the manifest artifact record>"],
  "scenarios": ["<same records as the manifest artifact record>"]
}
```

The verifier requires `baseline`, `revision`, `capturedAt`, `environment`, `sourceEndpoints` and
`scenarios` to agree with the manifest and the registry. This is what separates a copied
`pi-v1.0.4` artifact with re-pinned digests from real `pi-v1.3.0` evidence: an altered artifact
fails its SHA-256, and a correctly re-hashed artifact from another source fails the
baseline/revision binding.

A scenario may carry an optional `upstreamTest` object naming the frozen test case it mirrors;
the example family uses it so the observation can be read next to its independent expectation.
The scenario record is the comparison unit: a future C++ test names the same scenario and
compares the same observation. Do not replace a missing scenario with a symbol-existence check,
a table name, a version field or a self-captured expectation.

## Extension procedure

A ticket lane adds a family without inventing a proof format:

1. Create `fixtures/pi-v1.3.0/audit-<ticket>/` with a capture script that resolves the baseline
   by name from `baselines.json`, refuses a checkout whose `HEAD` is not the registered revision,
   refuses a modified source tree, records the real environment, and writes its artifact plus
   `manifest.json` only after every guard and observation succeeded. Model the capture on
   [`audit-1046/capture-extension-example.mts`](audit-1046/capture-extension-example.mts).
2. Run the capture against a private frozen checkout, then validate the candidate manifest and
   read the exact registry record from `check`:

   ```bash
   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-<ticket>/capture-<topic>.mts
   python3 scripts/evidence/audit_evidence.py check \
     --manifest fixtures/pi-v1.3.0/audit-<ticket>/manifest.json
   ```

3. Put the `check` record in the ticket handoff as the exact registry patch. The parent or serial
   merger applies it (a lane must not commit a shared-registry edit):

   ```bash
   python3 scripts/evidence/audit_evidence.py register \
     --manifest fixtures/pi-v1.3.0/audit-<ticket>/manifest.json
   python3 scripts/evidence/audit_evidence.py verify
   python3 scripts/evidence/audit_evidence.py read \
     --family audit-<ticket> --artifact <artifact>.json
   ```

`register` validates the manifest and every artifact before writing the pin, refuses a family
already owned by another baseline, refuses to rewrite a registered family unless `--replace`
records a deliberate recapture, and never touches another family's bundle. `verify` and `read`
operate on registered families only; `read` emits only bytes that verified through the whole
chain, and prints nothing on failure.

Capture, observation and normalization rules (dummy credentials, scripted inference, local
resources, no live provider catalogs, declared normalizations only) are owned by the run-scoped
audit protocol for tickets #1046–#1057 and restated in
[the #1046 evidence report](../../docs/research/pi-v1.3.0/1046-evidence-infrastructure.md).

## Worked example: `audit-1046`

The example family captures three public calls of the frozen `@earendil-works/pi-tui` package
entry (`fuzzyMatch`/`fuzzyFilter`) with real values observed from the frozen checkout:
`fuzzyMatch("", "anything") = {matches: true, score: 0}`,
`fuzzyMatch("fb", "foo-bar") = {matches: true, score: -18.6}` against
`fuzzyMatch("fb", "afbx") = {matches: true, score: -4.7}`, and
`fuzzyFilter(["a_p_p", "app", "application"], "app") = ["app", "application", "a_p_p"]`.
It demonstrates the extension path end to end; it is not a toolkit capability claim, and the
toolkit inventory remains owned by its audit tickets.

## Historical evidence is preserved

This registry is additive. `fixtures/pi-ai/baselines.json` keeps its `pi-v0.87.1` default and its
`pi-v0.87.1`/`pi-v1.0.0`/`pi-v1.0.4` identities; `fixtures/pi-tui/baselines.json` keeps
`pi-v1.0.4`; neither historical bundle is rewritten or relabelled, and no read here falls back to
them. Adding non-AI evidence does not register `pi-v1.3.0` as a `cch_ai` baseline, advance the AI
compatibility claim, or require recapturing live provider catalogs.

## Validation

```bash
python3 tests/evidence/NamedEvidenceTest.py     # offline: committed bundle, negative cases, preservation
PI_CHECKOUT=<frozen checkout> python3 tests/evidence/NamedEvidenceTest.py  # also the real capture path
ctest --preset vcpkg -R cch_pi_v130_named_evidence
```

The CTest case is registered by `cmake/tests/EvidenceTests.cmake`. Without a frozen sibling
checkout its two capture-path cases report a skip; the committed bundle and every fail-closed
negative case still run offline.

[spec #1044]: https://github.com/lanshengzhi/cpp-coding-harness/issues/1044
