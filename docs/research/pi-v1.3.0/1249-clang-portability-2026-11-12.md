# #1249 Clang portability, 2026-11-12

## Scope and observation

The Clang 22 conformance job in run 43176359218 at `5a83751653138e7a709770aa4b3ee1a20aa96f43` failed the standalone interface check on deleted defaulted comparisons in `Records.hpp` and the `ToolRegistration` class/struct tag mismatch. The same checkout's `ToolTask.cpp` also reports an implicitly deleted defaulted comparison in `ToolTask.hpp`.

Using the configured GCC translation unit command for `src/durable/ToolTask.cpp`, replacing the compiler with local Clang 23.1.3, removing the output and `-c` flags and adding `-fsyntax-only -Werror -ferror-limit=0`, reproduced all four diagnostics before the change. The identical command passed afterwards. Clang 22 is not installed locally; this observation does not establish the outcome of the next Clang 22 CI run.

`TaskInspectionState` cannot compare its `support::Error`, and the containing `TaskInspection` comparison is consequently deleted. `HarnessInspection`'s vector comparison also cannot be instantiated for those tasks. `NestedCallResult` contains non-comparable values. The change removes these unusable comparisons and matches the `ToolRegistration` forward declaration to its struct definition; it makes no new equality or capability claim.

## Validation

- Default Debug incremental build and separate `cch_parity_gate_build`: passed.
- Receipt #1184's exact `issue1184` selection: 10 selected, 10 passed, zero skipped; JUnit is `deliveries/1184-chord-context-cancellation-revalidation-1249-clang.xml`. Only the changed `Records.hpp` input was rebound; the prior validation is retained in the receipt.
- Unfiltered `ctest --preset vcpkg`: 4607 selected, 4595 passed, 12 skipped, zero failed; 327.56 seconds. This includes the standalone interface, warning, and consolidation gates.
- Consolidation build and check: passed; 1464 rows, zero uncovered scenarios.
- `receipt_boundary.py`: passed, 32 receipts, 808 claims, 966 comparisons. `audit_evidence.py verify --baseline pi-v1.3.0`: passed, 84 bundles, 180 artifacts, 1012 scenarios.
- Format check against the base and `git diff --check`: passed.

## Remaining boundary

An independent review is still required by the lane exit checklist. The task explicitly forbids starting subagents, and this runtime exposes no pi-intercom tool to seek an existing peer. The local Clang 23 check does not substitute for Clang 22 CI. #1249 remains open for its other work; this record makes no claim about it.
