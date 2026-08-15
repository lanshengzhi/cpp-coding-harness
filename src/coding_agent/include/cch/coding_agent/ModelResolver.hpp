#pragma once

#include <cch/ai/Model.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent {

/// pi `ScopedModel` (`core/model-resolver.ts`): one model scoped for Ctrl+P
/// cycling, with the optional explicit thinking level parsed from a
/// `pattern:level` scope entry. `std::nullopt` means the model inherits the
/// current session preference on cycle.
struct ScopedModel {
    ai::Model model{};
    std::optional<std::string> thinking_level{std::nullopt};
};

/// pi `ModelScopeDiagnostic`: one scoping warning. `type` is always
/// `"warning"` in the pi subset; the C++ value keeps only `code`/`message`/
/// `pattern`.
struct ModelScopeDiagnostic {
    /// pi `code`: `"no-match"` or `"invalid-thinking-level"`.
    std::string code{};
    std::string message{};
    std::string pattern{};
};

/// Result of resolving `--models` / settings `enabledModels` patterns (pi
/// `ResolveModelScopeResult`).
struct ModelScopeResolution {
    std::vector<ScopedModel> scoped_models{};
    std::vector<ModelScopeDiagnostic> diagnostics{};
};

/// pi `findExactModelReferenceMatch`: match a bare model id or a canonical
/// `provider/modelId` reference against `available_models`. When matching by
/// bare id, ambiguous matches across providers are rejected (returns
/// `std::nullopt`).
[[nodiscard]] std::optional<ai::Model> find_exact_model_reference_match(
    std::string_view model_reference,
    const std::vector<ai::Model>& available_models);

/// pi `parseModelPattern`: resolve one non-glob pattern to a model with an
/// optional `:level` thinking suffix. Tries an exact match first, then
/// partial id/name matches preferring aliases over dated versions. A
/// `:suffix` that is not a valid thinking level falls back to the prefix with
/// pi's warning (unless `allow_invalid_thinking_level_fallback` is false,
/// pi's strict CLI mode).
struct ParsedModelPattern {
    std::optional<ai::Model> model{std::nullopt};
    /// Explicit thinking level from a `pattern:level` suffix; `std::nullopt`
    /// when the pattern carried none.
    std::optional<std::string> thinking_level{std::nullopt};
    std::optional<std::string> warning{std::nullopt};
};
[[nodiscard]] ParsedModelPattern parse_model_pattern(
    std::string_view pattern,
    const std::vector<ai::Model>& available_models,
    bool allow_invalid_thinking_level_fallback = true);

/// pi `resolveModelScopeWithDiagnostics`: resolve model patterns to concrete
/// `ScopedModel` values with optional thinking levels. Glob patterns
/// (`*`/`?`/`[`) match against `provider/modelId` or the bare model id
/// (nocase, pi `minimatch` semantics); non-glob patterns go through
/// `parse_model_pattern`. Unmatched patterns produce `no-match`
/// diagnostics; invalid `:level` suffixes produce `invalid-thinking-level`
/// diagnostics. Duplicates are dropped in pattern order.
[[nodiscard]] ModelScopeResolution resolve_model_scope_with_diagnostics(
    const std::vector<std::string>& patterns,
    const std::vector<ai::Model>& available_models);

/// pi `resolveModelScope`: `resolve_model_scope_with_diagnostics` with the
/// diagnostics dropped.
[[nodiscard]] std::vector<ScopedModel> resolve_model_scope(
    const std::vector<std::string>& patterns,
    const std::vector<ai::Model>& available_models);

} // namespace cch::coding_agent
