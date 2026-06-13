---
title: "refactor: Adopt std::expected monadic operations for error propagation"
type: "refactor"
status: "active"
date: "2026-06-10"
target_repo: "cpp-coding-harness"
---

# refactor: Adopt std::expected monadic operations for error propagation

**Target repo:** `cpp-coding-harness`. All implementation paths below are repo-relative.

## Summary

Replace mechanical `if (!expected) return std::unexpected(expected.error())` defensive programming with C++23 `std::expected` monadic operations (`and_then`, `transform`, `or_else`, `transform_error`) for synchronous error chains, and introduce a lightweight coroutine error-propagation macro to flatten nested guard checks in async agent/provider code. The refactor preserves all existing behavior and public API contracts; it is purely a readability and maintainability improvement.

---

## Problem Frame

The codebase already uses `std::expected<T, util::Error>` as the primary error-propagation mechanism, which aligns with the anti-fragile architecture rules. However, the current implementation relies heavily on manual `if (!result)` guard checks and explicit `std::unexpected` returns. This creates three categories of mechanical noise:

1. **Coroutine emit guards:** In `AgentLoop::continue_with` and `OpenAIChatClient::stream`, nearly every `emit()` call is followed by an identical `if (!emitted) co_return std::unexpected(emitted.error())` block. Across `src/agent/AgentLoop.cpp`, `src/ai/providers/OpenAIChatClient.cpp`, and `src/AsyncCliRuntime.cpp`, there are ~40 occurrences of this pattern.
2. **Coroutine early-return cascades:** When an awaited operation returns `Expected<T>`, the success path must be unwrapped manually before the next statement, creating nested or sequential guard blocks that obscure the happy-path logic.
3. **Synchronous branch chains:** Functions like `arguments_for_call` and the tool-call argument accumulation at the end of `OpenAIChatClient::stream()` use sequential `if` checks on `expected` results that `and_then` / `or_else` can flatten.

The goal is to let the compiler's `std::expected` monadic interface carry the mechanical propagation, so the code expresses the happy path linearly and the error path declaratively.

---

## Requirements

- **R1.** Synchronous `std::expected` result chains in provider and agent implementation code use `and_then`, `transform`, `or_else`, or `transform_error` where they reduce nesting or clarify intent.
- **R2.** Coroutine functions that repeatedly guard `Expected<T>` or `ExpectedVoid` results with identical early-return patterns use a project-local macro (`CCH_TRY` / `CCH_TRY_VOID`) to express propagation.
- **R3.** The `emit()` guard pattern is extracted into a combinator or macro call that eliminates the per-site `if (!emitted)` boilerplate.
- **R4.** All refactored code preserves existing error codes, messages, and propagation semantics; no behavior changes.
- **R5.** Public headers remain macro-free; monadic helpers live in implementation code or a single private helper header.
- **R6.** Tests verify that the monadic refactor does not alter error paths or success values.

---

## Key Technical Decisions

- **KTD1. Macro over coroutine template for `co_await expected` propagation:** C++23 `std::expected` monadic operations accept synchronous lambdas and cannot `co_await`. A lightweight macro (`CCH_TRY(var, expr)` for `Expected<T>` and `CCH_TRY_VOID(expr)` for `ExpectedVoid`) is the only way to achieve Rust-like `?` error propagation in coroutines without exceptions. The macro is confined to `.cpp` files and is never exposed in public headers.
- **KTD2. Monadic operations applied selectively, not dogmatically:** Not every `if (!expected)` block should become an `and_then` chain. When the original `if` handles multiple side effects, cross-variable dependencies, or coroutine suspension, the macro or manual guard remains clearer. The rule of thumb: if the block is exactly `if (!x) return unexpected(x.error())`, it is a candidate for monadic flattening.
- **KTD3. `transform`/`and_then` for synchronous value chains, macro for coroutine guards:** Synchronous functions that transform or chain expected values use `std::expected`'s native monadic API. Coroutine body guards (e.g., `co_await` result unwrapping, `emit()` propagation) use the macro because native monadic ops cannot suspend.
- **KTD4. No exception-based error propagation:** The refactor stays within the project's `std::expected` philosophy. The macro performs `co_return std::unexpected(error)`, not `throw`, preserving the non-throwing contract.
- **KTD5. Compiler support verified:** GCC 16.1.1 and Clang 22.1.5 are available in the build environment and fully support C++23 `std::expected` monadic operations (P2505R5). No toolchain fallback is required.

---

## Scope Boundaries

**In scope**

- Refactoring synchronous error-handling in `src/ai/providers/OpenAIChatClient.cpp`, `src/agent/AgentLoop.cpp`, and `src/AsyncCliRuntime.cpp` where monadic operations clarify intent.
- Introducing `CCH_TRY` / `CCH_TRY_VOID` macros and applying them to coroutine emit guards and `co_await` early-return patterns.
- Adding tests that exercise refactored error paths to prove behavior preservation.

### Deferred to Follow-Up Work

- JsonSchema builder-style fluent interface (discussed separately; out of scope for this refactor).
- Replacing `std::expected` with a custom monad type or `std::execution` senders/receivers.
- Adding a generic `try_co_await` operator overload for `std::expected` (would require exception-based fallback and is rejected by KTD4).

### Outside this refactor's identity

- Changes to public API signatures, data contracts, or event shapes.
- Changes to error taxonomy (`ErrorCode`, `Error` struct).
- Changes to CLI behavior, tool safety logic, or provider wire protocol.

---

## System-Wide Impact

This is a localized refactor with no external contract changes. The impact is entirely inside `.cpp` implementation files. The largest reviewer impact is learning the `CCH_TRY` macro convention. The largest implementer impact is identifying which `if (!expected)` blocks are mechanical enough to flatten and which carry enough domain logic to keep explicit.

| Surface | Impact | Plan response |
| --- | --- | --- |
| `src/ai/providers/OpenAIChatClient.cpp` | ~20 `if (!x)` guards become `CCH_TRY` or monadic chains | U2 and U3 |
| `src/agent/AgentLoop.cpp` | ~15 emit guards and tool-result branches flatten | U3 and U4 |
| `src/AsyncCliRuntime.cpp` | Session load/create/open guards and loop-result checks | U5 |
| New helper header | One new private implementation header for macros | U1 |

---

## Implementation Units

### U1. Introduce monadic helper header

- **Goal:** Create the private helper surface for coroutine error propagation macros.
- **Requirements:** R2, R5.
- **Dependencies:** None.
- **Files:**
  - `src/util/ExpectedMacros.hpp` (create)
  - `tests/util/ExpectedMacrosTest.cpp` (create)
- **Approach:** Define `CCH_TRY(var, expr)` and `CCH_TRY_VOID(expr)` in a single private header under `src/util/`. The macro expands to an expected check, early `co_return std::unexpected(error)`, and value unwrapping. Use token pasting with `__LINE__` to avoid variable-name collisions when the macro is used multiple times in the same scope.
- **Execution note:** Test-first; write macro tests before applying the macro anywhere.
- **Patterns to follow:** Rust `?` operator semantics; standard `std::expected` monadic API shape.
- **Test scenarios:**
  - `CCH_TRY` with a successful `Expected<int>` unwraps the value and continues.
  - `CCH_TRY` with a failed `Expected<int>` performs `co_return std::unexpected(error)` and the caller observes the error.
  - `CCH_TRY_VOID` with a successful `ExpectedVoid` continues execution.
  - `CCH_TRY_VOID` with a failed `ExpectedVoid` performs `co_return std::unexpected(error)`.
  - Multiple `CCH_TRY` calls in the same coroutine scope compile without name collisions.
- **Verification:** Macro tests pass and the header compiles in isolation.

### U2. Refactor synchronous expected chains in provider client

- **Goal:** Apply `std::expected` monadic operations to synchronous helper functions in the OpenAI provider.
- **Requirements:** R1, R4.
- **Dependencies:** U1.
- **Files:**
  - `src/ai/providers/OpenAIChatClient.cpp`
  - `tests/ai/providers/OpenAIChatClientTest.cpp`
- **Approach:** Identify synchronous helpers where `and_then` / `transform` / `or_else` reduce nesting. The tool-call argument accumulation at the end of `stream()` (the `parsed_args` block) can be rewritten as a `transform`/`or_else` chain on `parse_tool_arguments(block.raw_arguments)`. `resolve_api_key` is less amenable to monadic chaining because it checks config fields and environment variables in priority order; leave it explicit unless a clear pattern emerges during implementation.
- **Patterns to follow:** Standard C++23 `std::expected` monadic API documentation.
- **Test scenarios:**
  - Tool-call argument accumulation: valid arguments populate `block.arguments` and clear error fields; invalid arguments populate `block.argument_error` and clear `block.arguments`.
  - All existing OpenAIChatClient tests still pass after the refactor.
- **Verification:** Provider tests pass without behavior changes.

### U3. Flatten emit guards with `CCH_TRY_VOID`

- **Goal:** Eliminate the repetitive `if (auto emitted = emit(...); !emitted) co_return std::unexpected(emitted.error())` pattern.
- **Requirements:** R2, R3, R4.
- **Dependencies:** U1.
- **Files:**
  - `src/ai/providers/OpenAIChatClient.cpp`
  - `src/agent/AgentLoop.cpp`
  - `tests/ai/providers/OpenAIChatClientTest.cpp`
  - `tests/agent/AsyncAgentLoopTest.cpp`
- **Approach:** Replace every mechanical emit guard with `CCH_TRY_VOID(emit(sink, ...))`. Only apply the macro where the original code does exactly `if (!emitted) co_return std::unexpected(emitted.error())` with no additional side effects. Emit guards that also log, modify local state, or branch on specific error codes remain explicit.
- **Patterns to follow:** The existing `emit` helper already centralizes the empty-sink check; `CCH_TRY_VOID` sits at the call site.
- **Test scenarios:**
  - A fake provider stream with an emit sink that returns an error stops at the first failing event and propagates the error.
  - A fake agent loop with an emit sink that returns an error stops at the first failing lifecycle event and propagates the error.
  - Empty/default sinks are still accepted and do not invoke undefined behavior.
- **Verification:** Agent and provider tests pass; error propagation semantics are unchanged.

### U4. Flatten coroutine `co_await` unwrap guards in agent loop

- **Goal:** Reduce nesting in `AgentLoop::continue_with` where `co_await` results are manually unwrapped.
- **Requirements:** R2, R4.
- **Dependencies:** U1, U3.
- **Files:**
  - `src/agent/AgentLoop.cpp`
  - `tests/agent/AsyncAgentLoopTest.cpp`
- **Approach:** Apply `CCH_TRY` to the `client_.stream()` call and any other `Expected<T>` results that are immediately guarded. For the tool execution block, apply `CCH_TRY` to `arguments_for_call()` and `tool->execute()` results to flatten the nested `if (!arguments)` / `if (!executed)` pyramid. Keep the tool-not-found check explicit because it branches on `registry_.find()` returning `nullptr`, not on an `Expected`.
- **Patterns to follow:** `AgentLoop::continue_with` happy path should read as a linear sequence: start turn → stream assistant → extract calls → for each call: find tool → get args → execute → build result → append.
- **Test scenarios:**
  - A fake run with text-only response completes with the same event sequence after flattening.
  - A fake run with tool calls completes with matching tool-result messages.
  - Malformed tool arguments produce the same error tool-result message.
  - Tool execution failure produces the same error tool-result message.
  - Provider stream failure emits the same terminal agent-end event and returns the same error.
- **Verification:** All existing agent loop tests pass without modification.

### U5. Flatten session and result guards in CLI runtime

- **Goal:** Apply monadic style to the synchronous session load/create/open checks and the final loop-result handling in `run_async_cli`.
- **Requirements:** R1, R2, R4.
- **Dependencies:** U1, U4.
- **Files:**
  - `src/AsyncCliRuntime.cpp`
  - `tests/cli/CliSmokeTest.cpp`
- **Approach:** The session load/create/open checks in `run_async_cli` are synchronous `if (!loaded)` blocks that log and return an exit code. These are better expressed with `or_else` logging or left explicit because they branch to `return 2` (not `std::unexpected`). Apply `CCH_TRY` only to the inner `run_prompt` lambda's `co_await loop.continue_with()` call and the `store.append()` loop. The outer session guards remain explicit because they control process exit codes, not expected propagation.
- **Patterns to follow:** CLI error handling already maps expected errors to stderr + exit codes; preserve that mapping.
- **Test scenarios:**
  - Fake CLI smoke run completes successfully after the refactor.
  - Session append failure still prints the same error message and returns false.
  - Loop failure still prints the same error message and returns false.
- **Verification:** CLI smoke tests pass.

---

## Deferred Implementation Notes

- The exact macro expansion must be verified to compile with both GCC 16 and Clang 22. If token pasting with `__LINE__` produces warnings on either compiler, switch to `__COUNTER__` or a unique-name generator.
- If any coroutine uses `CCH_TRY` inside a lambda coroutine that is itself inside another coroutine, the `co_return` in the macro targets the innermost coroutine. This is the intended behavior, but implementers should verify scope correctness.
- `transform_error` is available in C++23 but may not see use in this refactor because the project uses a single `util::Error` type and rarely remaps error codes at boundaries. If implementation reveals a need, add it opportunistically.

---

## Sources & Research

- Current `std::expected` usage patterns: `src/ai/providers/OpenAIChatClient.cpp`, `src/agent/AgentLoop.cpp`, `src/AsyncCliRuntime.cpp`, `src/ai/providers/BoostBeastStreamTransport.cpp`.
- Project error contract: `include/cch/util/Error.hpp`.
- C++23 `std::expected` monadic operations: cppreference `std::expected` (P2505R5).
- Compiler support verification: local `g++ --version` (16.1.1), `clang++ --version` (22.1.5).
