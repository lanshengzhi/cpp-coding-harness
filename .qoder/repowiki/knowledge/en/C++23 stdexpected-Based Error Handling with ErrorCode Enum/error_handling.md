## Overview

This C++23 codebase uses `std::expected<T, Error>` as its primary error propagation mechanism, avoiding exceptions for control flow. Errors are structured with typed error codes, human-readable messages, optional details, and contextual metadata.

## Core Error System

### Error Type Definition (`include/cch/util/Error.hpp`)

The foundation is a simple `Error` struct:

```cpp
struct Error {
    ErrorCode code{ErrorCode::Unknown};
    std::string message;
    std::string detail;
    std::optional<std::string> context;
};
```

**ErrorCode enum** defines 12 categories:
- `Unknown`, `JsonParse`, `JsonSerialize` — serialization errors
- `Network`, `Timeout`, `Cancelled` — async/IO errors
- `Provider`, `Tool` — AI provider and tool execution errors
- `Session`, `Validation`, `Workspace`, `Process` — domain-specific errors

**Type aliases** simplify usage:
- `util::Expected<T>` = `std::expected<T, Error>`
- `util::ExpectedVoid` = `std::expected<void, Error>`

**Helper function**: `util::make_error(code, message, detail?, context?)` constructs errors consistently.

### Error Propagation Pattern

Functions return `util::Expected<T>` or `boost::asio::awaitable<util::Expected<T>>` for coroutines. Callers check validity with boolean conversion and access errors via `.error()`:

```cpp
auto result = some_operation();
if (!result) {
    return std::unexpected(result.error());
}
auto value = std::move(*result);
```

### Coroutine-Specific Macros (`src/util/ExpectedMacros.hpp`)

Two internal macros reduce boilerplate in coroutine contexts:

- **`CCH_TRY(var, expr)`** — unwraps an `Expected<T>`; on failure, `co_return`s the error immediately
- **`CCH_TRY_VOID(expr)`** — same for `ExpectedVoid` returns

These are **private implementation details** (not in public headers) and use `co_return`, so they only work inside coroutines returning `std::expected`.

Example usage from `AgentLoop.cpp`:
```cpp
CCH_TRY_VOID(emit(sink, ToolExecutionStartEvent{...}));
```

## Exception Handling Strategy

Exceptions are **not used for normal control flow**, but they are caught at boundary layers where external hooks or third-party code might throw:

### Hook Boundary Protection

User-provided hooks (`TransformContextHook`, `ConvertToLlmHook`, `BeforeToolCallHook`, `AfterToolCallHook`, `GetSteeringMessagesHook`, etc.) are wrapped in try/catch blocks that convert exceptions to `util::Error`:

```cpp
[[nodiscard]] util::Expected<std::vector<ai::MessageVariant>> invoke_transform_context_hook(
    TransformContextHook& hook,
    const std::vector<ai::MessageVariant>& messages) {
    try {
        return hook(messages);
    } catch (const std::exception& e) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Tool, "transformContext hook failed", e.what()));
    } catch (...) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Tool, "transformContext hook failed", "unknown exception"));
    }
}
```

This pattern appears consistently across:
- `src/agent/AgentLoop.cpp` — message transformation hooks
- `src/agent/ToolCallExecutor.cpp` — before/after tool call hooks and event sinks

### Event Sink Protection

`AgentEventSink` invocations are similarly protected, ensuring a misbehaving subscriber cannot crash the agent loop.

## Session Persistence Error Handling

### JSONL Write Failures

Session persistence uses `JsonlSessionStore` which returns `util::ExpectedVoid` for all append operations. Serialization failures (via `glz::write_json`) and journal I/O errors are propagated as `util::ErrorCode::Session`.

In `AgentSessionRuntime.cpp`, session append failures produce a specific terminal code:
```cpp
if (auto appended = session_.store->append(new_history[index]); !appended) {
    return PromptRunResult{false, "session_persist_failed", "could not persist session entry", {}};
}
```

The CLI runtime (`AsyncCliRuntime.cpp`) recognizes `"session_persist_failed"` as a non-fatal condition that warns but does not abort the session.

### Entry Serialization

`EntrySerializer` converts messages and tree entries to JSON. Serialization errors carry `ErrorCode::Session` with minimal detail (no user data leakage). Per-type methods handle any necessary redaction before DTO construction.

## Design Decisions

1. **No exceptions for control flow** — `std::expected` makes error paths explicit in type signatures
2. **Structured errors over string messages** — `ErrorCode` enum enables programmatic error classification
3. **Boundary exception catching** — external/untrusted code (hooks, sinks) is sandboxed with try/catch wrappers
4. **Coroutine-friendly macros** — `CCH_TRY`/`CCH_TRY_VOID` reduce nesting in async agent loops
5. **Best-effort persistence** — session write failures are reported but do not crash running sessions
6. **No panic/recover mechanism** — the codebase has no equivalent of Rust's `panic!` or Go's `recover`; fatal errors propagate up to the caller

## Key Files

- `include/cch/util/Error.hpp` — core error types, `ErrorCode` enum, `make_error` helper
- `src/util/ExpectedMacros.hpp` — coroutine error-propagation macros
- `src/agent/AgentLoop.cpp` — extensive use of `CCH_TRY` and hook boundary protection
- `src/agent/ToolCallExecutor.cpp` — tool execution error handling and event sink protection
- `include/cch/harness/session/JsonlSessionStore.hpp` — session persistence API returning `ExpectedVoid`
- `src/harness/session/JsonlSessionStore.cpp` — serialization and journal append error handling
- `src/coding_agent/runtime/AgentSessionRuntime.cpp` — session persist failure handling
