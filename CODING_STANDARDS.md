# Coding Standards

Code-level rules for this repository, written to be cited. Every rule is checkable against a diff: a reviewer can point at a hunk and name the rule it breaks (e.g. `CODING_STANDARDS.md §5.3`).

## 1. Scope and precedence

1.1. This file is the single source of truth for code-level conventions: mechanical style, naming, error handling, async, ownership, function structure and local state, tests, CMake.

1.2. Architecture guardrails live in `docs/agents/architecture.md`, domain language in `CONTEXT.md`, and decision rationale in `docs/adr/`. This file records their checkable code-level consequences and cites the authoritative layer without duplicating its rationale or definitions. Each layer wins within its own scope; this file wins on code-level questions.

1.3. In review, a documented rule here overrides generic style baselines (such as the Fowler smell baseline used by `/skill:code-review`): where this file endorses something a baseline would flag, the baseline yields.

1.4. Reviewers skip only exact violations that required validation rejects — see §14. A non-failing compiler diagnostic is not tool-enforced and remains a valid finding.

1.5. This file stays small enough to read whole on every implementation and review: a new rule generalizes or displaces an existing one where it can, rather than only accreting. A rule that has never been cited in review and is fully validation-enforced (§14) is a pruning candidate; the removal is recorded in its issue.

## 2. Mechanical style

Rules 2.1–2.5 are the canonical `.clang-format` contract, enforced on added or modified lines by `scripts/format-check.sh` (see §14). Untouched lines stay outside that gate.

2.1. Braces are 1TBS: opening brace on the same line for namespaces, types, functions, and control flow.

2.2. Indent with 4 spaces. Wrapped parameter or argument lists either continue at 8 spaces or break one parameter per line.

2.3. Write `const` west of the type: `const std::string&`. East-const (`std::string const`) does not appear in this codebase.

2.4. `*` and `&` bind to the type: `const std::string& name`, `AsyncAgentTool* find(...)`.

2.5. The line-length limit is 120 columns on added or modified lines. Break signatures one parameter per line when wrapping.

2.6. No trailing return types on named functions. Lambdas declare `-> T` when the deduced return type would not be obvious.

2.7. Every header uses `#pragma once`; no include guards.

2.8. Include order, one blank line between groups: (1) corresponding header, (2) project headers, (3) third-party (`boost/`, `glaze/`), (4) standard library.

2.9. Include spelling: Owner Interface headers use their one canonical `<cch/...>` angle include. Private headers use the owning target's declared private root and quoted canonical spelling; the corresponding header is quoted by basename. Never use relative climbs, basenames outside the corresponding source/header pair, absolute paths, or macro-generated project includes. Legacy relative includes predate this rule—see §15.

2.10. Close every namespace with a comment: `} // namespace cch::agent`.

2.11. No `using namespace` in `include/` or `src/`. Test files may put `using namespace cch;` at the top.

## 3. Naming

3.1. Types are `PascalCase`; functions and variables are `snake_case`; private member variables carry a trailing underscore (`impl_`, `options_`); public struct members are plain `snake_case`.

3.2. pi-shaped seams keep pi wire vocabulary in `camelCase`: `AsyncExecutionEnv::readTextFile`, `SessionTree::getEntry`, fields like `mtimeMs`, `exitCode`, `filePath`. This is the one sanctioned camelCase region (§15); it does not extend to cch-native code.

3.3. Suffix vocabulary: `*Event` for event structs; `*Result`, `*Options`, `*Config` for passive structs; `*Sink`, `*Hook`, `*Committer` for `move_only_function` aliases; `*Variant` for variant aliases; `*Dto` confined to the Glaze layer. An asynchronous operation is identified by its `AsyncResult` return type rather than a mandatory `Async` prefix. Identity-bearing physical capabilities keep domain names such as `AsyncExecutionEnv` where the frozen Interface already uses them.

3.4. Enums are always `enum class`; enumerators are `PascalCase` unless mirroring wire vocabulary, which keeps its wire spelling.

3.5. Constants are `kCamelCase` (`kDefaultMaxOutputLines`). Macros are `SCREAMING_SNAKE` and private to `src/`.

3.6. Files are `PascalCase`; header/source pairs share a stem (`Agent.hpp`/`Agent.cpp`); tests are `<Stem>Test.cpp`; fixtures are `*Fixture.hpp`.

3.7. Namespaces: root `cch`, one nested namespace per module (`cch::harness::session`), declared with nested syntax (`namespace cch::agent {`). File-local helpers live in an anonymous namespace inside the module namespace.

## 4. Data contracts (passive values)

4.1. Owner Interface value contracts are aggregate structs with default member initializers (`bool truncated{false};`) — no user-declared constructors, no behavior beyond trivial helpers (`docs/agents/architecture.md` §Passive value contracts; ADR 0019; statically checked for key headers, see §14).

4.2. Sum types are a `std::variant` behind a named `using` alias (`MessageVariant`, `AgentLifecycleEvent`). Dispatch with `std::visit` + `if constexpr`, or `std::get_if`.

4.3. Optional fields are `std::optional<T>` defaulting to `std::nullopt` — never sentinel values (empty path, `-1`) for "absent".

4.4. Construct multi-field contracts with designated initializers or named builders (`user_text_message(...)`), not positional brace lists — positional aggregate initialization is how fields get misbound (ADR 0013).

4.5. Unstructured cross-Owner JSON facts use `cch::support::JsonValue`. Raw Glaze generic values and `boost::json` never appear in Owner Interfaces (ADR 0007; ADR 0039).

4.6. Parameters: cheap scalars, enums, and non-owning views such as `std::string_view` by value; non-trivial borrowed inputs by `const&`; owned values (strings, vectors, messages, callbacks) by value and moved from. A coroutine input used after suspension is an owning value in the coroutine frame or has a declaration-level lifetime contract per §7.5.

4.7. Fallible or easy-to-ignore return values are `[[nodiscard]]`, including awaitables.

## 5. Error handling

5.1. The shared default error channel is `cch::support::Error` with `cch::support::Expected<T>` / `ExpectedVoid`; return errors as `std::unexpected(...)`. An Owner may expose a more specific `std::expected<T, E>` when the domain contract requires it (ADR 0039; ADR 0040).

5.2. The pi-shaped filesystem/shell contracts use `std::expected<T, FileError>` / `std::expected<T, ExecutionError>` and convert to the consuming operation's error type at the Owner boundary.

5.3. Error-propagation macros are support implementation machinery and never appear in Owner Interfaces. Use the owning operation's return form (`return`, `co_return`, or callback completion) consistently; do not add a second error channel.

5.4. Exceptions are not an error mechanism. Do not throw for control flow; before asynchronous initiation, convert ordinary throwable setup failures to the operation's expected error channel. After initiation, producer initiation and terminal completion are `noexcept`; weak-observer exceptions are caught, boundedly diagnosed, and deactivate the observer (ADR 0017; ADR 0040).

5.5. Filesystem calls use the `std::error_code` overloads, not the throwing overloads.

5.6. The standard error check is if-init: `if (auto result = f(...); !result) { ... }`.

5.7. Project-owned production and test targets are designed for the strict no-exception configuration. The strict build uses `-fno-exceptions`; `throw`, `try`/`catch`, and exception propagation are not ordinary failure paths. Expected failures use `Expected`/`std::error_code`, while invariant violations terminate.

5.8. Owner Interfaces never expose `std::exception_ptr`, Boost.Asio completion types, or exception-based completion. The only allowed exception pointer is in the private AI completion bridge, where the exception-enabled fallback configuration maps it once to an `Expected` error, while `BOOST_ASIO_NO_EXCEPTIONS` treats a non-null pointer as a fatal Runtime invariant; it is never rethrown or forwarded across an Owner boundary. The Parity Architecture Gate's versioned exception allowlist is authoritative for this private detail.

## 6. Async and connections

6.1. Fallible asynchronous Owner operations return `[[nodiscard]] cch::support::AsyncResult<T, E>` (default `E = cch::support::Error`). Ready values complete inline without support allocation or suspension; pending operations own their post-initiation inputs. Boost.Asio awaitables, executors, schedulers, and cancellation types stay private to implementations. Bounded in-memory value work remains synchronous; do not create an asynchronous facade for it (ADR 0040).

6.2. Stored single operations, sinks, committers, and policy operations use `std::move_only_function` — never `std::function` unless independent copying is a documented contract (`docs/agents/architecture.md` §Connection strength; ADR 0040). A stored callback's copyability says nothing about referent lifetime.

6.3. Connection strength is explicit. Model-stream delivery and Agent-to-Session commitment are named strong, awaited, backpressured connections. Session-to-TUI, status, diagnostic, and ordinary Agent Session subscribers are weak observers that perform only bounded value work or mailbox sends; catch, diagnose, and deactivate a throwing observer without vetoing progress or persistence (ADR 0017; ADR 0040).

6.4. Cancellation is supplied explicitly as `std::stop_token`; each Capability Owner resolves cancellation/completion races into its honest domain outcome. Session Abort and Session Close are idempotent; Close never cancels an admitted Session Event Commitment or required persistence work (ADR 0020; ADR 0040).

6.5. `AsyncResult` is move-only and consumed once by callback start or move-only `co_await`. Duplicate/moved-from/reused consumption, duplicate completion, and controlled completion-callback contract violations terminate in every build mode. The coroutine Owner establishes quiescence before frame destruction; concurrent resume/destroy is forbidden (ADR 0040).

6.6. Agent, Models Runtime, and Agent Session state changes run only in the owning object's serialized execution domain. Cross-Owner events and worker results enter the target object's bounded FIFO mailbox; workers never mutate serialized Owner state directly, and event-loop submission never blocks or falls back to inline worker execution. Ordinary overload returns a typed `Busy` result; persistence, credential, terminal-completion, and Close control work use reserved admission. Queue capacities, batch sizes, worker counts, and timeouts are measured policy, not speculative knobs — the selected values and their regression tests are recorded in `docs/runtime-capacities.md` (ADR 0040).

## 7. Classes and ownership

7.1. Interfaces are abstract classes with `virtual ~X() = default;`, pure-virtual methods, and no data members.

7.2. Implementations are `final`; every overridden virtual is marked `override`.

7.3. Value types follow the rule of zero. Owning types declare, in this order: move constructor/assignment (usually `noexcept`), destructor, deleted copy constructor/assignment.

7.4. `std::unique_ptr` is the default owner (capabilities, factory results). `std::shared_ptr` is reserved for deliberately shared live state, such as the concrete coding-agent Models Runtime lifetime captured by the AI-owned `ModelStream`; it is not an injection or virtual-for-fake convention (ADR 0040).

7.5. A stored non-owning reference or raw pointer, and any borrowed asynchronous input used after initiation returns, states its lifetime contract in a doc comment at the declaration (`const X& value_; // must outlive every operation`). Ordinary borrowed call parameters whose use ends with the call need no lifetime comment. Prefer owning pending-operation inputs (ADR 0040).

7.6. Pimpl uses a nested `struct Impl;` defined in the `.cpp`, held by `unique_ptr` (or `shared_ptr` for shared-live-state owners like `Agent`).

7.7. Allocate with `std::make_unique` / `std::make_shared`; no owning raw `new`/`delete`.

7.8. POSIX file descriptors are owned by `UniqueFd` (`src/support/UniqueFd.hpp`) — never `unique_ptr<int>` guards or manual `::close` cleanup paths.

## 8. Owner Interface headers

8.1. Owner Interface headers live under Owner-local roots and have one canonical `<cch/...>` angle-include spelling. They are repository-internal and are not installed, exported, or ABI-promised. Private implementation roots are not visible across Owners. The Parity Architecture Gate enforces roots, ownership, visibility, canonical spelling, and legal cross-Owner relationships (ADR 0039).

8.2. Owner Interface headers include only standard-library, `cch_support`, and legal downstream Owner headers. They never include Boost.Asio, Glaze, provider/serialization DTOs, loaders/parsers/visitors, schema conversion, or private implementation paths (`docs/agents/architecture.md` §Local generic machinery; ADR 0039).

8.3. Each Owner Interface header compiles independently. Prefer forward declarations when only a name, reference, or pointer is needed; never add umbrella/forwarding headers, compatibility aliases, alternate spellings, or macro-generated project includes.

## 9. Modern C++ usage

9.1. Use: `std::expected`, `std::move_only_function`, `std::string_view` for read-only text, `constexpr` / `inline constexpr` for constants, `if constexpr`, structured bindings where they read better, `std::format` for new string formatting (do not churn existing `+` concatenation just to convert it).

9.2. Prefix and suffix checks are `starts_with` / `ends_with` — never `rfind(x, 0) == 0` or `compare(0, n, x) == 0`. Erasing all occurrences of a value is `std::erase` / `std::erase_if`, never the erase-remove idiom.

9.3. Number parsing is `std::from_chars` — never `std::stoi`/`std::stod`/`strtol`. When the input is already validated (e.g. a regex-constrained group), state that in a comment and skip the error check.

9.4. Template parameters are declared with `typename` (`template <typename T>`, `template <typename... Ts>`) — never `class`.

9.5. Constrained templates are permitted for local, non-escaping operations when they are clearer than a runtime seam (ADR 0040); keep constraints local rather than publishing generic machinery through Owner Interfaces. Not yet: modules, `<=>`, `std::print`, deducing this, and `std::expected` monadic chains (`and_then`/`transform`/`or_else`). Introduce any of those only with an ADR.

9.6. Casts are `static_cast` / `reinterpret_cast`; no C-style casts.

9.7. Container sizes and in-memory indices use `std::size_t`. Wire, persisted, or domain integer contracts whose width matters use `std::`-qualified fixed-width types (`std::int64_t`, `std::uint32_t`).

9.8. `auto` when the type is obvious from the right-hand side (factories, expected results); an explicit type otherwise. `const auto&` in range-for.

## 10. Security invariants

This section is the checkable form of `docs/agents/architecture.md` §Security and containment.

10.1. Secret redaction is Owner-private product policy. Route it through the owning package's one approved implementation; never hand-roll redaction or expose redaction machinery through `cch_support` or an Owner Interface (ADR 0039).

10.2. Redact before truncating. Use the owning package's bounded-redacted-text operation, which preserves the complete `[REDACTED]` marker.

10.3. Bounded output goes through the owning package's declared output/presentation budgets. No ad hoc `substr` truncation of model- or user-visible text, and process pipes continue draining after retained output reaches its bound (ADR 0040).

10.4. Workspace containment lives in `src/agent/harness/WorkspaceFileSystem.hpp` and its split implementation units `WorkspaceFileSystem{FdWalk,Legacy,Pi,Temp}.cpp` (absolute-path and `..` rejection, symlink-escape checks, atomic writes). File tools route through it; no parallel path-validation logic.

10.5. `bash` runs with a sanitized environment that omits API-key, token, secret, password, and OpenAI-looking variables. Extend the filter; never bypass it.

10.6. Credential values may appear in the credential-store layer (`cch::ai` `Credential`/`CredentialStore`, coding-agent `AuthStorage`), in `AuthResult` and short-lived request auth carried by trusted in-process authentication and Provider capabilities, and in transport request headers. They flow from credential-store/auth resolution through `Models` into the Provider's transport authorization (ADR 0029, ADR 0030). `Model`, the `streamSimple` request surface (`Model` argument + `ProviderStreamOptions`), message, and Session Entry contracts remain credential-free (ADR 0019); the legacy `StreamChatRequest` aggregate is removed (ADR 0034). The legacy `coding_agent::AuthEntry` remains confined to the pre-#345 loader; added or modified request paths use the new carriers.

## 11. Tests

11.1. Use formal Catch2 v3 from the pinned dependency graph through its imported target. Include Catch2's public headers directly; the local Catch-compatible imitation and compatibility headers are prohibited.

11.2. Every test name is unique and sentence-style. Use flat cases by default; shallow `SECTION`, generators, matchers, fixtures, or `SCENARIO` are allowed when they reduce real duplication without creating a combination explosion.

11.3. `CHECK` reports a failure and continues. Use `REQUIRE` / `REQUIRE_FALSE` only for preconditions whose failure makes later dereference, indexing, or execution unsafe. Use explicit skip reporting for unavailable platform prerequisites.

11.4. Tags are `[module][feature]`, module first, spelled like the owning test area: `[ai]`, `[agent]`, `[harness]`, `[tools]`, `[support]`, `[tui]`, `[cli]`, `[architecture]`, `[coding_agent]`. Add parity tags `[u1]`–`[u9]` and issue-traceability tags `[issueNN]` where they apply. Fatal-contract probes carry a `[fatal]` tag; the owning shard's `catch_discover_tests` selects `[fatal]` with a tightened CTest timeout. Tags map to CTest labels.

11.5. Layout follows Capability Owner or support ownership: `tests/<owner-or-support>/<Stem>Test.cpp`, with deeper directories for cohesive areas. Shared helpers live in `tests/support/` under `namespace cch::tests` and do not create a cross-Owner production seam. Test include roots are declared in `cmake/tests/TestsSetup.cmake` (`CCH_FORMAL_TEST_INCLUDE_DIRS`); shared helpers are included as `"support/<Header>.hpp"` against the declared `tests/` root.

11.6. CTest names and labels are the normal selection, scheduling, timeout, and reporting surface. Fast deterministic unit/contract cases are discovered individually. Process, TTY, golden, and complete Agent Session/TUI scenarios are grouped only when measured startup cost or shared setup justifies it. Fatal, cancellation, Close, and hang-prone contracts run in isolated subprocesses with hard timeouts; the old shard runner is not a second scheduling authority.

11.7. Fake Providers and deterministic local resources are the default. No live API keys or provider network access appears in the default CTest suite (`docs/agents/validation.md` §Provider tests). Each test owns unique temporary directories, ports, files, and environment state.

11.8. Hold-and-release test doubles gate on the counted, latched `tests::ReleaseGate` (`tests/support/ReleaseGate.hpp`) — never a hand-rolled max-expiry `steady_timer` cancel. A `release()` that arrives before the double arms the gate is stored and consumed by the next wait, so release/arm ordering races cannot hang a test; one permit releases one waiter, preserving per-operation re-arming. Cancellation wake-ups use `interrupt()`, which records no permit, and the wait is guarded by a `stop_requested()` check.

## 12. CMake

12.1. The authoritative Owner libraries are `cch_ai`, `cch_agent_core`, `cch_tui`, and repository-private `cch_coding_agent`; `cch_support` is the pi-neutral support library. The only direct cross-Owner project edges are `cch_agent_core -> cch_ai` and `cch_coding_agent -> {cch_agent_core, cch_ai, cch_tui}`. `cch_ai` and `cch_tui` have no Owner dependencies. Cross-Owner edges target only authoritative Owner libraries, never same-Owner private implementation targets (ADR 0039).

12.2. Use the central CMake constructors for every production target. Each declaration names exactly one role (`owner`, `implementation`, `support`, `composition`, or classified `external`), one Owner where applicable, its explicit source set, unconditional direct dependencies, and defaults. Every production source compiles once; the production graph is acyclic. Do not bypass the constructors with ad hoc target declarations.

12.3. Owner Interface roots are target-specific and repository-internal. Publish only the owning target's Interface root to legal repository dependents; keep implementation roots private. Sources are listed explicitly—no `file(GLOB ...)`. Project dependencies use classified targets, not path/linker-name shortcuts.

12.4. The project is C++23 with extensions off, set once at the top level; do not lower the standard per target. Every compiled target enables the project warning defaults.

12.5. Every supported configure, build, test, install, release, and CI path runs or depends on the latest applicable Parity Architecture Gate phase, including tests-disabled configurations. CMake File API data, compile commands, direct-include evidence, and—after build—compiler depfiles are configured evidence; CMake source formatting is not the architecture seam (ADR 0039).

12.6. `CMakeLists.txt` is pure orchestration: it brackets `project()` with supported-build validation and includes the orchestration modules. Production target declarations live in `cmake/targets/`; test registrations live in `cmake/tests/`. Each new orchestration include starts with `include_guard(GLOBAL)` and the comment `# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).`

## 13. Function structure and local state

13.1. Single Level of Abstraction (SLA): A high-level orchestrator expresses sequencing through module interfaces or cohesive private helpers. Flag it when the same function also performs line-by-line formatting, token- or regex-level parsing, or raw byte/buffer manipulation. A formatter or parser whose implementation is itself the function's purpose is not an orchestrator and is not a violation.

13.2. Pure value transformations: A transformation determined solely by its input values stays independent of physical I/O, mutable global state, and mutable object state. It accepts the values it needs explicitly and returns the owning domain's `std::expected`/`cch::support::Expected` only when it can fail; an infallible transformation returns `T` directly. Extract an embedded transformation from an orchestrator when it contains branching, iteration, parsing/validation, or multi-field formatting. A direct field projection or other single-expression transformation remains inline.

13.3. Seam discipline: Physical capability use stays behind interfaces or narrow implementation boundaries established by `docs/agents/architecture.md` §Capability seams or an accepted ADR. Value transformations move inward into private helpers or implementation modules rather than outward into new capability interfaces. Adding, moving, or removing a capability seam is an architecture decision resolved in `docs/agents/architecture.md` or an ADR, not a code-level review inference.

13.4. Locality of mutable state: Declare a mutable local in the narrowest existing lexical block containing all its uses. Flag it when the declaration can move into an existing nested block without changing behavior, or when the same variable is reused for a different meaning in a later processing phase. Accumulators, parser state, and explicit lifecycle state that intentionally span phases are not violations.

## 14. Validation-enforced — reviewers skip exact violations

- Skip a finding only when a required build or test necessarily fails on that exact violation. The Parity Architecture Gate rejects only the configured relationships and stable rule identifiers its manifest/evidence policy defines; its pass does not imply broader §2–§13 or §16 conformance (ADR 0039).
- Compiler diagnostics remain review findings: `-Wall -Wextra -Wpedantic` are enabled without warnings-as-errors.
- Formatting of added or modified lines is checked against `.clang-format` by `scripts/format-check.sh` (CI runs it as the formatting gate). Reviewers skip findings that duplicate its patch; untouched lines stay outside the gate.
- For code changes that compile and pass the required suite, report remaining §2–§13 and §16 violations. Documentation-only changes follow `docs/agents/validation.md` §Documentation-only changes instead.

## 15. Known exceptions and migrations

Sanctioned deviations are grandfathered only on untouched existing lines. Added or modified lines comply with the current rule unless an exception below explicitly permits the deviation.

- **camelCase pi vocabulary (§3.2):** untouched camelCase declarations and uses are grandfathered. A new or renamed camelCase identifier is allowed only when its issue/spec or an adjacent comment identifies the matching pi identifier; otherwise the declaration uses `snake_case`. This semantic rule covers filesystem/session/trust seams, wire fields, and skill/prompt parity code without a path allowlist.
- **Variant-alias naming (§3.3):** `Content`, `AssistantContent`, `Credential`, `AuthPromptKind`, and `AuthEventKind` predate the `*Variant` suffix and are intentionally kept to avoid public API churn (debt recorded in #372); added or renamed variant aliases use `*Variant`.

## 16. Minimal implementation

Rules 16.1–16.5 are the diff-checkable minimal-implementation discipline: reuse what exists, prefer the standard library and pinned dependencies to new code and new packages, and keep deliberate shortcuts findable. They operate within the interfaces and seams the architecture mandates (§1.2, §13.3) and never argue against a mandated seam itself.

16.1. Reuse before writing: A change reuses an existing project helper, `cch_support` utility, test support header, or established pattern rather than re-implementing it. A new helper that duplicates functionality already available in the owning package or `cch_support` is flagged.

16.2. Standard library and pinned dependencies before hand-rolled code: Where the C++ standard library or an already-pinned dependency provides the operation, no hand-rolled equivalent appears. §9.2 and §9.3 are instances of this rule, not an exhaustive list. Between two standard-library approaches of equal size, the edge-case-correct one wins.

16.3. New third-party dependencies carry justification: A new `vcpkg.json` entry cites its issue or ADR and enters only when the standard library or an already-pinned dependency does not reasonably cover the capability. A manifest baseline change runs Fresh Validation (`AGENTS.md` §Validation entry points).

16.4. `debt:` markers: A deliberate simplification with a known ceiling (a global lock, an O(n²) scan, a naive heuristic) carries a `// debt: <ceiling>, <upgrade trigger>` comment naming both. §15 tracks sanctioned legacy deviations on untouched lines; `debt:` marks new deliberate shortcuts so "later" cannot quietly become "never".

16.5. Bug fixes land at the shared seam: A fix that guards one caller when the defect lives in the shared callee its callers route through is flagged — the fix lands once in the shared code. Deliberate caller-side patching records in review why sibling callers are unaffected.
