---
title: "fix: Align SDK api_key_env and skill diagnostics with pi contracts"
type: fix
status: active
date: "2026-06-21"
target_repo: "cpp-coding-harness"
reference_repo: "pi"
origin: docs/plans/2026-06-21-001-feat-t8-embeddable-sdk-surface-plan.md
review: docs/plans/2026-06-21-001-feat-t8-embeddable-sdk-surface-plan.md (code review findings)
---

# fix: Align SDK api_key_env and skill diagnostics with pi contracts

## Summary

Fix three concrete divergences found during code review of the T8 embeddable SDK implementation:

1. `SdkProviderConfig::api_key_env` only accepts a single env var name; pi and `ConfigLoader` support an env var fallback chain (e.g., `["OPENAI_API_KEY", "AZURE_API_KEY"]`).
2. `/skill:name` expansion diagnostics are silently discarded in the SDK path; they should be surfaced to the caller.
3. `AgentSession::Impl::fanout()` is dead code — defined but never called.

## Problem Frame

The T8 SDK plan (`docs/plans/2026-06-21-001-feat-t8-embeddable-sdk-surface-plan.md`) references pi's SDK contract shape and the repository's own `ConfigLoader`, which supports env var chains. The implementation was reviewed and three gaps were identified:

- **api_key_env chain:** `ConfigLoader` stores `api_key_env` as `std::optional<std::vector<std::string>>` (chain), but `SdkProviderConfig` exposed it as `std::optional<std::string>` (single). The deferred resume construction path further truncated the chain to `[0]`. This means SDK callers cannot provide fallback env vars — if `OPENAI_API_KEY` is unset, there is no second option.

- **Skill diagnostics dropped:** `expand_skill_command_silent()` returns a `SkillExpansionResult` with a `diagnostics` field, and its doc comment says "Diagnostics are returned as values for SDK/non-interactive use." But `process_sdk_prompt()` calls it, moves out the expanded text, and discards the diagnostics vector. When a user types `/skill:unknown`, the warning is silently lost.

- **Dead code:** `AgentSession::Impl::fanout()` (~15 lines) implements subscriber fanout but is never called. The actual fanout is done by `make_combined_sink()`. This is a code-quality issue, not a behavior bug.

## Requirements

- **R1. Env var chain:** `SdkProviderConfig::api_key_env` accepts a vector of env var names. `build_chat_client()` validates that at least one is set, discovers the first-set name, and passes it to the provider factory.
- **R2. Skill diagnostics:** `PromptResult` gains a `diagnostics` field. `process_sdk_prompt()` forwards `SkillExpansionResult::diagnostics` into it. Callers can inspect diagnostics alongside the prompt result.
- **R3. Dead code removal:** Remove `AgentSession::Impl::fanout()` and any call-site references.
- **R4. No regression:** All existing SDK tests pass; new tests cover api_key_env chain validation and skill diagnostic forwarding.

## Scope Boundaries

### In Scope

- Change `SdkProviderConfig::api_key_env` type to `std::optional<std::vector<std::string>>`
- Update `build_chat_client()` to resolve the first-set env var from the chain
- Fix deferred resume construction path to pass the full chain
- Add `std::vector<std::string> diagnostics` field to `PromptResult`
- Forward `SkillExpansionResult::diagnostics` in `process_sdk_prompt()`
- Remove dead `fanout()` method
- Add tests

### Deferred to Follow-Up Work

- Upgrading `ProviderFactoryContext::api_key_env` (currently `std::string`) to also support chains — blocked by provider/client layer changes
- Auto-including skill diagnostics in `PromptResult` even when skill expansion succeeds (current fix only forwards warning/error diagnostics)
- Removing the duplicate `make_combined_sink` path — the raw-pointer capture pattern is safe under serial-prompt constraints but fragile

---

## Key Technical Decisions

| Decision | Rationale |
| --- | --- |
| Use `std::optional<std::vector<std::string>>` for `api_key_env` | Matches `ConfigLoader` type exactly, pi's env chain pattern, and allows zero-code-change serialization |
| Resolve to the first-set var name and pass it to `ProviderFactoryContext` | `ProviderFactoryContext::api_key_env` is `std::string` — we can't change it without touching provider/client layers. Passing the specific resolved name preserves the chain's intent. |
| Add diagnostics to `PromptResult`, not a separate API | Keep the prompt API simple; callers already inspect `PromptResult` |
| Remove `fanout()` entirely | It's dead code with an active duplicate; no behavioral change |

---

## Implementation Units

### U1. api_key_env chain support

**Goal:** Change `SdkProviderConfig::api_key_env` from single string to vector, update all call sites.

**Requirements:** R1, R4

**Dependencies:** None

**Files:**
- Modify: `include/cch/coding_agent/Sdk.hpp`
- Modify: `src/coding_agent/Sdk.cpp`
- Test: `tests/coding_agent/SdkSessionTest.cpp`

**Approach:**
- Change `SdkProviderConfig::api_key_env` type to `std::optional<std::vector<std::string>>`
- In `build_chat_client()`: iterate the chain with `ConfigLoader::resolve_api_key()` to validate that at least one var is set and discover the first-set name; pass that name as `ctx.api_key_env`
- In the deferred resume construction path: assign `*config->api_key_env` directly instead of `[0]`
- In the execution env construction path: pass the chain directly instead of wrapping a single string
- Update README example if it uses the field

**Patterns to follow:**
- `include/cch/coding_agent/Config.hpp` — same type signature for `api_key_env`
- `src/coding_agent/ConfigLoader.cpp` — `resolve_api_key()` usage

**Test scenarios:**
- Happy path: SDK creation with a 2-element chain where only the second var is set succeeds
- Edge case: chain with all vars unset fails with descriptive error listing the chain
- Edge case: empty chain (after optional unwrap) fails validation
- Integration: resume path with config-derived chain passes all elements through

**Verification:**
- `./build/cpp_harness_tests "[sdk]"` passes
- New test exercises the chain with `setenv`/`unsetenv`

### U2. Skill diagnostic forwarding

**Goal:** Surface `/skill:name` expansion diagnostics to SDK callers via `PromptResult`.

**Requirements:** R2, R4

**Dependencies:** U1 (same files, but logically independent)

**Files:**
- Modify: `include/cch/coding_agent/Sdk.hpp`
- Modify: `src/coding_agent/Sdk.cpp`
- Test: `tests/coding_agent/SdkSessionTest.cpp`

**Approach:**
- Add `std::vector<std::string> diagnostics` field to `PromptResult`
- In `process_sdk_prompt()`: capture `skill_expansion.diagnostics` and carry them into the result
- In `AgentSession::prompt()`: populate `result.diagnostics` from the expansion diagnostics

**Patterns to follow:**
- `src/coding_agent/PromptProcessing.cpp` — `expand_skill_command_silent()` contract and `SkillExpansionResult` shape
- `include/cch/coding_agent/Sdk.hpp` — `PromptResult` struct

**Test scenarios:**
- Happy path: prompt with `/skill:valid_skill` returns empty diagnostics
- Error path: prompt with `/skill:unknown` returns `diagnostics` containing `"[skill:warn] unknown skill: unknown"`
- Edge case: prompt with bare `/skill:` (no name) returns empty diagnostics (passthrough)

**Verification:**
- Test asserts diagnostics vector is populated for unknown skill and empty for valid skill

### U3. Remove dead fanout() code

**Goal:** Delete unused code.

**Requirements:** R3

**Dependencies:** None

**Files:**
- Modify: `src/coding_agent/Sdk.cpp`

**Approach:**
- Remove `AgentSession::Impl::fanout()` method (lines ~83-95)
- Verify no callers exist via grep

**Patterns to follow:**
- Keep `make_combined_sink()` — it is the active implementation

**Test scenarios:**
- Test expectation: none — pure removal of dead code; existing tests prove no behavioral change

**Verification:**
- Compiles cleanly; all tests pass

---

## System-Wide Impact

- **Public API:** `SdkProviderConfig::api_key_env` type changes — this is a source-level breaking change for any code that constructed `SdkProviderConfig` with designated initializers and a single string. The migration is trivial (wrap in `{}`). The SDK is experimental and not ABI-stable.
- **PromptResult:** Gains a `diagnostics` field — backward-compatible addition (new field at end of aggregate).
- **Provider layer:** Unchanged. The chain-to-single resolution happens inside `build_chat_client()`, which is SDK-private.
- **No impact on CLI/RPC modes.**

---

## Risks

| Risk | Likelihood | Impact | Mitigation |
| --- | --- | --- | --- |
| Breaking SDK callers that use `api_key_env` as single string | Low (experimental SDK, just shipped) | Low | Change is trivial to migrate; note in plan |
| `setenv`/`unsetenv` tests pollute environment | Low | Low | Use Catch2 fixtures to save/restore env |
