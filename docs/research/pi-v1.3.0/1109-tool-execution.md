# #1109 tool execution, preparation and ordering delivery

## Scope

Baseline pi-v1.3.0, revision `71023aec1546d6d677a65669c5665c0927ce0dd0`. This delivery records the bounded behavior, discriminators, and validation for #1109.

## Delivered behavior

- **Standalone `run_tool_call`**: Expose the public Owner Interface `cch::agent::run_tool_call` in `<cch/agent/RunToolCall.hpp>`. It executes a single tool call through argument preparation, schema validation, `beforeToolCall`, `execute()`, and `afterToolCall`. It emits no lifecycle events and adds no messages. Unknown tools, validation failures, blocked calls, and thrown errors are returned as outcomes with `is_error = true`.
- **`prepareArguments` precedes schema validation**: Tools may declare `ToolPrepareArguments` (pi `AgentTool.prepareArguments`) on `cch::agent::Tool`. When present, it normalizes raw call arguments before schema compilation and validation. A tool with invalid raw arguments that are normalized to valid parameters passes validation; without the hook, the same input fails. Tested both through standalone `run_tool_call` and public `Agent::prompt`.
- **`beforeToolCall` mutation without revalidation**: The `BeforeToolCallResult` can return modified `args` (pi `BeforeToolCallResult.args`). When provided, the mutated arguments are forwarded directly to `execute()` without re-validating against the tool parameter schema. Tested both through standalone `run_tool_call` and public `Agent::prompt`.
- **Parallel batch ordering**: Parallel execution in `ToolCallExecutor` emits `ToolExecutionEndEvent`s in completion order as each tool finishes, while `ToolResultMessage`s and final batch results are returned strictly in assistant source order. Tested both through `ToolCallExecutor` and public `Agent::prompt`.
- **Duration presence discrimination**: `execute()` execution duration is measured with a monotonic clock and is present for successful calls and calls that fail during execution (`thrown`); it is absent for calls that never executed (`unknown`, `blocked`, or invalid arguments).

## Validation

- Focused selection via CTest: `ctest --preset vcpkg -L issue1109` selected and passed **11/11 tests (0.23 sec)**, exit 0:
  1. `run_tool_call executes successfully and records execute-only duration`
  2. `run_tool_call returns error outcome without running hooks for unknown tool or validation failure`
  3. `run_tool_call returns error outcome for blocked tool call without executing`
  4. `run_tool_call preserves duration on tool execution failure and after hook failure`
  5. `prepareArguments runs before validation and normalizes invalid raw arguments` (discriminating case: without hook, same input fails validation)
  6. `beforeToolCall mutation executes without revalidation` (discriminating case: mutated number replaces string without revalidation failure)
  7. `parallel tool ends follow completion order while result messages keep source order` (discriminating ordering case: 40ms vs 5ms tools)
  8. `parallel batch distinguishes duration presence across ok, thrown, blocked, and unknown calls` (discriminating duration presence across all 4 categories in one batch)
  9. `Agent prompt normalizes invalid raw arguments with prepareArguments before validation` (public caller end-to-end)
  10. `Agent prompt executes mutated beforeToolCall args without revalidation` (public caller end-to-end)
  11. `bounded parallel execution emits end events in completion order` (public caller end-to-end)
- Shard tests: `./build/cch_tests_agent` selected **206 test cases / 3,084 assertions**; 205 passed, 1 skipped (the realtime clock fixture wrapper runs under `cch_agent_realtime_jump`, which passed).
- Architecture Gate: `ctest --preset vcpkg -L architecture -j2` selected and passed **43/43**, exit 0. `cch_owner_interface_standalone` compiles `<cch/agent/RunToolCall.hpp>` cleanly.
- Full build: `cmake --build --preset vcpkg` passed cleanly; build-phase Parity Gate passed (`parity-build-gate.json`).
- Format check: `scripts/format-check.sh origin/main` clean.
- Evidence verifier: `audit_evidence.py verify --baseline pi-v1.3.0` passed 44 bundles / 100 artifacts / 561 scenarios.
- Consolidation check: `consolidate_audits.py check` passed with 1,464 rows and 0 uncovered scenarios.
- Two-axis code review: Standards (0 hard violations, 0 smells after cleanup); Spec (0 missing requirements, 0 scope creep, 0 incorrect implementations).

## Capability status

Bounded slice implementation and discriminators delivered. Whole-row capability ledger promotion and receipt re-pin are owned by #1176 (Agent verification) and #1183 (Consolidation index).

## Remaining boundary

Model-response timing remains excluded per ADR 0075. Third-party TS/JS extension execution remains excluded. Full Agent whole-row delivery receipts are consolidated in #1176. #1044 remains open.
