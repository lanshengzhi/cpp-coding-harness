# #1239 stale-field and receipt gates

## Scope and result

On 2026-11-06, 44 previously verified claims were bounded because their discriminating case reused a comparison test. Their comparisons remain in the receipts; each row reverts to its source-audit status. The measured verified count fell from 609 to 565. No membership changed.

## Bounded rows and follow-up owners

- `cch_durable:image-processor` — #1242
- `cch_durable:read-image` — #1242
- `cch_durable:image-capability-absent` — #1242
- `cch_durable:image-orientation-conversion` — #1242
- `cch_coding_agent:codemode-models-description-gate` — #1241
- `cch_chord:context:without-abort-signal` — #1240
- `cch_chord:delta:batch-application` — #1240
- `cch_chord:delta:immutable-application` — #1240
- `cch_chord:delta:mutable-application` — #1240
- `cch_chord:delta:overlap` — #1240
- `cch_chord:delta:prepare-replace` — #1240
- `cch_chord:state:attached-hydration` — #1240
- `cch_chord:state:dispose` — #1240
- `cch_chord:state:listener-error` — #1240
- `cch_chord:Chord:draft` — #1240
- `cch_chord:json:copy-json` — #1240
- `cch_chord:json:strict-validation` — #1240
- `cch_chord:json:json-value` — #1240
- `cch_chord:Chord:json` — #1240
- `cch_chord:state:delivery-order` — #1240
- `cch_codemode:codemode-declaration-identifier` — #1241
- `cch_codemode:codemode-script-globals` — #1241
- `cch_codemode:codemode-script-store-load` — #1241
- `cch_codemode:codemode-script-output-text-image-console` — #1241
- `cch_codemode:codemode-script-error-taxonomy` — #1241
- `cch_codemode:codemode-script-abort-reason` — #1241
- `cch_codemode:codemode-deterministic-start` — #1241
- `cch_codemode:codemode-deterministic-clock-seed` — #1241
- `cch_codemode:codemode-deterministic-seq` — #1241
- `cch_codemode:codemode-deterministic-during` — #1241
- `cch_codemode:codemode-deterministic-validation` — #1241
- `cch_codemode:codemode-inline-success` — #1241
- `cch_codemode:codemode-remote-exchange` — #1241
- `cch_codemode:codemode-remote-caps` — #1241
- `cch_codemode:codemode-remote-failures` — #1241
- `cch_codemode:codemode-worker-admission` — #1241
- `cch_agent_core:InputQueue:peek-clear` — #1243
- `cch_agent_core:Agent:reset` — #1243
- `cch_agent_core:Agent:prompt-busy` — #1243
- `cch_coding_agent:theme-token-set` — #1243
- `cch_codemode:codemode-execute-outcome` — #1241
- `cch_codemode:codemode-portable-entry` — #1241
- `cch_durable:Storage:jsonl:sidecar` — #1242
- `cch_durable:Storage:jsonl:ordering` — #1242

## Validation

Original implementation validation at `4f5561530`:

- Consolidation build and check passed; the focused `ConsolidationAuditTest` run selected 82, passed 82, skipped 0. `receipt_boundary.py` passed 31 receipts, 807 claims and 965 comparisons. Python compilation, format-check and `git diff --check` passed.
- The first full CTest run selected 4,557: 4,543 passed, 3 failed and 11 skipped. All three failures passed in a subsequent isolated selection (3 selected, 3 passed, 0 skipped). The Native TUI `pi-v1.3.0 InteractiveEngine preserves exact nested and clipped cursor coordinates across mode switch` case saw four restored IME events instead of two; the quarantined `Projection publishes 100 message-update chunks inside the issue cost bound over a long history` timing case measured 2.079 ms against its 2 ms bound; and `Durable edit tool reproduces the frozen BOM CRLF diff and rejection results` reported a concurrent edit diff mismatch. These C++ paths were untouched by the Python and evidence changes; the isolated passes do not establish the cause of the first and third failures.
- A two-job full CTest rerun selected 4,557: 4,546 passed, 0 failed and 11 skipped.

Review-fix validation:

- Consolidation build and check passed; `ConsolidationAuditTest` selected 91, passed 91 and skipped 0. `receipt_boundary.py` passed 31 receipts, 807 claims and 965 comparisons. `audit_evidence.py verify` verified 83 bundles, 179 artifacts and 1,010 scenarios.
- The first evidence-label selection omitted `TERM_PROGRAM=herdr`: 13 selected, 12 passed, 1 failed and 0 skipped; the real PTY test failed its environment precondition. With `TERM_PROGRAM=herdr`, the same selection passed: 13 selected, 13 passed, 0 skipped. `ReceiptBoundaryTest.py` selected 24, passed 24 and skipped 0. The incremental vcpkg build and Parity Architecture Gate passed.
- `TERM_PROGRAM=herdr ctest --preset vcpkg -j2` selected 4,557: 4,546 passed, 0 failed and 11 skipped. `scripts/format-check.sh HEAD` and `git diff --check` passed.

## Remaining boundary

The follow-up tickets own independent discriminating cases. The dated stale-field baseline records existing contradictions separately; #1180 owns durable cleanup, with #1238 retained as the historical reference, and #1044 owns the remaining baseline.

The consumer/gaps regular expression is a **check**, not acceptance that a field accurately describes its owner. It misses “The durable library is pending initial implementation in C++.” and “cch_durable has not landed in tree yet; awaiting upstream port.” It also falsely flags “verified that when optional cch_durable package configuration is missing, fallback defaults are used.” This change accepts these escapes and false positives as a recorded boundary rather than widening the pattern; field accuracy still requires review.

The ticket snapshot is offline and can drift when an issue changes state. Build and check warn once it is more than 30 days old; they do not fail solely because of age. Refresh and review the snapshot as described in [delivery reconciliation](1058-delivery-reconciliation.md).
