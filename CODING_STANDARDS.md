# Coding Standards

Code-level rules for this repository, written to be cited. Every rule is checkable against a diff: a reviewer can point at a hunk and name the rule it breaks (e.g. `CODING_STANDARDS.md §5.3`).

## 1. Scope and precedence

1.1. This file is the single source of truth for code-level conventions: mechanical style, naming, error handling, async, ownership, tests, CMake.

1.2. It does not restate other layers. Architecture guardrails live in `AGENTS.md`, domain language in `CONTEXT.md`, decision rationale in `docs/adr/`. Each layer wins within its own scope; this file wins on code-level questions.

1.3. In review, a documented rule here overrides generic style baselines (such as the Fowler smell baseline used by `/skill:code-review`): where this file endorses something a baseline would flag, the baseline yields.

1.4. Reviewers skip anything tooling already rejects — see §13. A finding that duplicates a compiler warning or an architecture test is noise.

## 2. Mechanical style

2.1. Braces are 1TBS: opening brace on the same line for namespaces, types, functions, and control flow.

2.2. Indent with 4 spaces. Wrapped parameter or argument lists either continue at 8 spaces or break one parameter per line.

2.3. Write `const` west of the type: `const std::string&`. East-const (`std::string const`) does not appear in this codebase.

2.4. `*` and `&` bind to the type: `const std::string& name`, `AsyncAgentTool* find(...)`.

2.5. Line length is a soft cap of ~120 columns. Break signatures one parameter per line when wrapping.

2.6. No trailing return types on named functions. Lambdas declare `-> T` when the deduced return type would not be obvious.

2.7. Every header uses `#pragma once`; no include guards.

2.8. Include order, one blank line between groups: (1) corresponding header, (2) project headers, (3) third-party (`boost/`, `glaze/`), (4) standard library.

2.9. Include spelling: public contract headers as `<cch/...>`; private `src/` headers quoted relative to `src/` (`"util/Json.hpp"`); the corresponding header quoted by basename. Never relative climbs (`"../../include/cch/..."`, `"../util/..."`). Legacy relative includes predate this rule — see §14.

2.10. Close every namespace with a comment: `} // namespace cch::agent`.

2.11. No `using namespace` in `include/` or `src/`. Test files may put `using namespace cch;` at the top.

## 3. Naming

3.1. Types are `PascalCase`; functions and variables are `snake_case`; private member variables carry a trailing underscore (`impl_`, `options_`); public struct members are plain `snake_case`.

3.2. pi-shaped seams keep pi wire vocabulary in `camelCase`: `AsyncExecutionEnv::readTextFile`, `SessionTree::getEntry`, fields like `mtimeMs`, `exitCode`, `filePath`. This is the one sanctioned camelCase region (§14); it does not extend to cch-native code.

3.3. Suffix vocabulary: `*Event` for event structs; `*Result`, `*Options`, `*Config` for passive structs; `*Sink`, `*Hook`, `*Committer` for `move_only_function` aliases; `*Variant` for variant aliases; `*Dto` confined to the Glaze layer. Awaitable capability interfaces carry an `Async` prefix (`AsyncAgentTool`, `AsyncExecutionEnv`); synchronous ones are plain nouns (`SessionStore`).

3.4. Enums are always `enum class`; enumerators are `PascalCase` unless mirroring wire vocabulary (`MaxTokensField::max_tokens`).

3.5. Constants are `kCamelCase` (`kDefaultMaxOutputLines`). Macros are `SCREAMING_SNAKE` and private to `src/`.

3.6. Files are `PascalCase`; header/source pairs share a stem (`Agent.hpp`/`Agent.cpp`); tests are `<Stem>Test.cpp`; fixtures are `*Fixture.hpp`.

3.7. Namespaces: root `cch`, one nested namespace per module (`cch::harness::session`), declared with nested syntax (`namespace cch::agent {`). File-local helpers live in an anonymous namespace inside the module namespace.

## 4. Data contracts (passive values)

4.1. Public contracts are aggregate structs with default member initializers (`bool truncated{false};`) — no user-declared constructors, no behavior beyond trivial helpers (AGENTS.md guardrail 1; ADR 0019; static-asserted for key headers, see §13).

4.2. Sum types are a `std::variant` behind a named `using` alias (`MessageVariant`, `AgentLifecycleEvent`). Dispatch with `std::visit` + `if constexpr`, or `std::get_if`.

4.3. Optional fields are `std::optional<T>` defaulting to `std::nullopt` — never sentinel values (empty path, `-1`) for "absent".

4.4. Construct multi-field contracts with designated initializers or named builders (`user_text_message(...)`), not positional brace lists — positional aggregate initialization is how fields get misbound (ADR 0013).

4.5. Unstructured JSON facts use `util::JsonValue`. Raw Glaze generic values and `boost::json` never appear in contracts (ADR 0007).

4.6. Parameters: read-only inputs by `const&`; owned values (strings, vectors, messages, callbacks) by value and moved from. Coroutine parameters that outlive a suspension point are always by value.

4.7. Fallible or easy-to-ignore return values are `[[nodiscard]]`, including awaitables.

## 5. Error handling

5.1. One error channel: `util::Error` with `util::Expected<T>` / `util::ExpectedVoid` (`include/cch/util/Error.hpp`). Build errors with `util::make_error(...)`; return them as `std::unexpected(...)`.

5.2. Exception to 5.1: the pi-shaped filesystem/shell contracts use `std::expected<T, FileError>` / `std::expected<T, ExecutionError>`; convert with `to_util_error` at the module boundary.

5.3. Inside coroutines, propagate errors with `CCH_TRY` / `CCH_TRY_VOID` (`src/util/ExpectedMacros.hpp`). The macros `co_return`, so they never appear in public headers.

5.4. Exceptions are not an error mechanism. Do not throw for control flow; wrap callback invocation in try/catch at the boundary and convert to `util::Error` (ADR 0017 — see `emit_event_impl` in `src/agent/AgentLoop.cpp`). Exceptions never unwind through a live coroutine.

5.5. Filesystem calls use the `std::error_code` overloads, not the throwing overloads.

5.6. The standard error check is if-init: `if (auto result = f(...); !result) { ... }`.

## 6. Async and events

6.1. Async operations return `[[nodiscard]] boost::asio::awaitable<util::Expected<T>>` (or `ExpectedVoid`). There is no parallel sync path.

6.2. Event sinks, committers, and policy hooks are `std::move_only_function` aliases (`AgentEventSink`, `AgentEventCommitter`) — never `std::function` (AGENTS.md guardrail 3; ADR 0018). §14 lists the one legacy exception.

6.3. Subscribers are weak observers: invoke every subscriber callback inside try/catch; a failing subscriber becomes a bounded diagnostic and is deactivated — it never vetoes agent progress or persistence (ADR 0017).

6.4. Cancellation flows through `std::stop_token`; `close()` and `abort()` are idempotent (ADR 0020).

6.5. Sync policy logic adapts into awaitable hooks through ready-awaitable adapters — never a second, sync hook path (ADR 0018).

## 7. Classes and ownership

7.1. Interfaces are abstract classes with `virtual ~X() = default;`, pure-virtual methods, and no data members.

7.2. Implementations are `final`; every overridden virtual is marked `override`.

7.3. Value types follow the rule of zero. Owning types declare, in this order: move constructor/assignment (usually `noexcept`), destructor, deleted copy constructor/assignment.

7.4. `std::unique_ptr` is the default owner (capabilities, factory results). `std::shared_ptr` is reserved for genuinely shared services (`AsyncExecutionEnv`, `StreamTransport`).

7.5. Non-owning references and raw pointers are allowed when the lifetime contract is stated in a doc comment at the declaration (`ai::StreamingChatClient& client_; // must outlive every prompt coroutine`).

7.6. Pimpl uses a nested `struct Impl;` defined in the `.cpp`, held by `unique_ptr` (or `shared_ptr` for shared-live-state owners like `Agent`).

7.7. Allocate with `std::make_unique` / `std::make_shared`; no owning raw `new`/`delete`.

7.8. POSIX file descriptors are owned by `UniqueFd` (`src/harness/UniqueFd.hpp`) — never `unique_ptr<int>` guards or manual `::close` cleanup paths.

## 8. Public header surface

8.1. `include/cch/` is the only public surface; `src/` is private via CMake `PUBLIC`/`PRIVATE` include paths. Most of this section is enforced by `tests/architecture/` (§13).

8.2. Public headers do not include Glaze, `src/` paths, provider DTOs, loaders, or the AgentLoop (AGENTS.md guardrail 4). Boost.Asio headers are allowed — `awaitable` appears in public signatures.

8.3. Prefer forward declarations in narrow headers when only a name, reference, or pointer is needed.

## 9. Modern C++ usage

9.1. Use: `std::expected`, `std::move_only_function`, `std::string_view` for read-only text, `constexpr` / `inline constexpr` for constants, `if constexpr`, structured bindings where they read better, `std::format` for new string formatting (do not churn existing `+` concatenation just to convert it).

9.2. Prefix and suffix checks are `starts_with` / `ends_with` — never `rfind(x, 0) == 0` or `compare(0, n, x) == 0`. Erasing all occurrences of a value is `std::erase` / `std::erase_if`, never the erase-remove idiom.

9.3. Number parsing is `std::from_chars` — never `std::stoi`/`std::stod`/`strtol`. When the input is already validated (e.g. a regex-constrained group), state that in a comment and skip the error check.

9.4. Template parameters are declared with `typename` (`template <typename T>`, `template <typename... Ts>`) — never `class`.

9.5. Not yet: concepts, modules, `<=>`, `std::print`, deducing this, `std::expected` monadic chains (`and_then`/`transform`/`or_else`). None appear in the codebase; the monadic chains would add a third error-handling idiom alongside if-init (§5.6) and `CCH_TRY` (§5.3). Introduce any of these only with an ADR.

9.6. Casts are `static_cast` / `reinterpret_cast`; no C-style casts.

9.7. Sizes and counts use `std::`-qualified fixed-width types (`std::size_t`, `std::int64_t`).

9.8. `auto` when the type is obvious from the right-hand side (factories, expected results); an explicit type otherwise. `const auto&` in range-for.

## 10. Security invariants

This section is the checkable form of AGENTS.md guardrail 5.

10.1. Secret redaction has exactly one implementation: `src/util/Redactor.hpp`. Never hand-roll redaction, and never expose it in public headers.

10.2. Redact before truncating. Use `bounded_redacted_text` (`src/util/BoundedText.hpp`), which bounds output without splitting the `[REDACTED]` marker.

10.3. Bounded output goes through the existing budgets: `src/util/OutputLimiter.hpp` (default 50 KiB / 2000 lines) and the presentation budgets in `src/coding_agent/runtime/BoundedText.hpp`. No ad-hoc `substr` truncation of model- or user-visible text.

10.4. Workspace containment lives in `src/harness/WorkspaceFileSystem.hpp` (absolute-path and `..` rejection, symlink-escape checks, atomic writes). File tools route through it; no parallel path-validation logic.

10.5. `bash` runs with a sanitized environment that omits API-key, token, secret, password, and OpenAI-looking variables. Extend the filter; never bypass it.

10.6. Passive values never hold credentials or secrets (ADR 0019).

## 11. Tests

11.1. Use Catch2 through the vendored fallback header, included by depth-adjusted relative path (`#include "../../third_party/catch2/catch_test_macros.hpp"`).

11.2. Flat `TEST_CASE`s only — no `SECTION`, `GENERATE`, or `SCENARIO`. Names are sentence-style ("CCH_TRY unwraps successful expected and continues").

11.3. `CHECK` for assertions; `REQUIRE` / `REQUIRE_FALSE` only for preconditions whose failure voids the rest of the case; `SUCCEED` for platform-conditional skips.

11.4. Tags are `[module][feature]`, module first, spelled like the test directory: `[ai]`, `[agent]`, `[harness]`, `[tools]`, `[util]`, `[cli]`, `[sdk]`, `[architecture]`, `[coding_agent]`. Add parity tags `[u1]`–`[u9]` and issue-traceability tags `[issueNN]` where they apply. `[coding-agent]` (with a dash) is legacy — see §14.

11.5. Layout mirrors `src/`: `tests/<module>/<Stem>Test.cpp`, including nested directories (`tests/harness/session/`). Shared helpers live in `tests/support/` under `namespace cch::tests`.

11.6. Fake providers and clients by default — scripted fakes over the public interfaces. No live API keys or network access in the default `ctest` suite (AGENTS.md Validation).

## 12. CMake

12.1. One library target per `src/` module, named `cch_<module>`. The dependency direction stays `util < ai < {agent, harness} < tools < runtime` (enforced by `CMakeDependencyTest`).

12.2. Every library target applies `cch_target_defaults()`, which publishes `include/` as `PUBLIC`, keeps `src/` `PRIVATE`, and sets `-Wall -Wextra -Wpedantic`.

12.3. Sources are listed explicitly per target — no `file(GLOB ...)`.

12.4. The project is C++23 with extensions off, set once at the top level; do not lower the standard per target.

## 13. Tool-enforced — reviewers skip these

- Compiler warnings: `-Wall -Wextra -Wpedantic` on every target.
- `tests/architecture/`: public/private include purity, Glaze-free domain headers, aggregate/abstract `static_assert`s on key contracts, move-only callback enforcement, module dependency direction, the banned legacy patterns of AGENTS.md Guardrails (`util::Result`, `boost::json`, legacy sync tool factories, removed contract types).
- Report only what compiles and passes the suite but still violates §2–§12.

## 14. Known exceptions and migrations

Sanctioned deviations — do not report them in review. Anything else the rules catch is a finding.

- **camelCase pi seams (§3.2):** `include/cch/harness/ExecutionEnv.hpp`, `include/cch/harness/session/SessionTree.hpp`, and the skill/prompt loaders under `src/coding_agent/` keep pi vocabulary deliberately (pi parity, AGENTS.md).
- **`ProviderFactory` is `std::function`** (`include/cch/ai/ProviderRegistry.hpp`) — legacy, pending migration to `move_only_function`. Do not add new `std::function` callbacks anywhere.
- **Include spelling (§2.9):** relative quoted project includes (`"../util/Error.hpp"`, `"../../include/cch/..."`) predate the rule; migrate to `<cch/...>` when touching the file.
- **Test tag `[coding-agent]`** (35 uses) predates §11.4's `[coding_agent]`; rename opportunistically.
- **Constant naming (§3.5):** snake_case constexpr locals and a few `SCREAMING_SNAKE` public constants predate the rule; new constants are `kCamelCase`.
