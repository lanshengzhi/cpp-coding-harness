// Model pattern resolution and scoping (pi `core/model-resolver.ts` subset):
// exact reference matching, `parseModelPattern` with `:level` suffixes and
// alias/dated preference, glob scoping with pi's minimatch semantics, and
// the `no-match` / `invalid-thinking-level` diagnostics that feed the
// scoped-models selector's unavailable ids. Pure value tests over hand-built
// model lists — no runtime, no network.

#include <cch/coding_agent/ModelResolver.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] ai::Model model(
    std::string id,
    std::string provider,
    std::string name = {}) {
    ai::Model result;
    result.id = std::move(id);
    result.provider = std::move(provider);
    result.name = std::move(name);
    return result;
}

/// Two providers with a plain and a dated version each, plus an alias-style
/// id, exercising the alias/dated preference rules.
[[nodiscard]] std::vector<ai::Model> catalog() {
    return {
        model("alpha-1", "alpha"),
        model("alpha-1-20250929", "alpha"),
        model("beta-1", "beta"),
        model("claude-sonnet-4-5", "anthropic", "Claude Sonnet 4.5"),
        model("claude-sonnet-4-5-20250929", "anthropic", "Claude Sonnet 4.5 (dated)"),
    };
}

} // namespace

TEST_CASE(
    "find_exact_model_reference_match resolves canonical, split, and bare references",
    "[coding_agent][model-resolver][issue407]") {
    const auto models = catalog();

    // Canonical provider/id reference.
    const auto canonical = coding_agent::find_exact_model_reference_match("alpha/alpha-1", models);
    REQUIRE(canonical.has_value());
    CHECK(canonical->provider == "alpha");
    CHECK(canonical->id == "alpha-1");

    // Case-insensitive canonical reference.
    const auto upper = coding_agent::find_exact_model_reference_match("ALPHA/Alpha-1", models);
    REQUIRE(upper.has_value());
    CHECK(upper->id == "alpha-1");

    // Bare id resolves when unambiguous.
    const auto bare = coding_agent::find_exact_model_reference_match("beta-1", models);
    REQUIRE(bare.has_value());
    CHECK(bare->provider == "beta");

    // Trimmed input.
    const auto trimmed = coding_agent::find_exact_model_reference_match("  alpha-1  ", models);
    REQUIRE(trimmed.has_value());

    // Ambiguous bare id across providers is rejected.
    auto ambiguous = catalog();
    ambiguous.push_back(model("beta-1", "gamma"));
    CHECK_FALSE(coding_agent::find_exact_model_reference_match("beta-1", ambiguous).has_value());

    // Unknown references resolve to nothing.
    CHECK_FALSE(coding_agent::find_exact_model_reference_match("nope/nothing", models).has_value());
    CHECK_FALSE(coding_agent::find_exact_model_reference_match("", models).has_value());
}

TEST_CASE(
    "parse_model_pattern prefers aliases over dated versions for partial matches",
    "[coding_agent][model-resolver][issue407]") {
    const auto models = catalog();

    // Exact match wins with no thinking level.
    const auto exact = coding_agent::parse_model_pattern("alpha-1", models);
    REQUIRE(exact.model.has_value());
    CHECK_FALSE(exact.thinking_level.has_value());
    CHECK_FALSE(exact.warning.has_value());

    // Partial id match prefers the alias over the dated version.
    const auto partial = coding_agent::parse_model_pattern("claude-sonnet-4-5", models);
    REQUIRE(partial.model.has_value());
    CHECK(partial.model->id == "claude-sonnet-4-5");
    CHECK(partial.model->provider == "anthropic");

    // Partial name match works too.
    const auto by_name = coding_agent::parse_model_pattern("sonnet 4.5", models);
    REQUIRE(by_name.model.has_value());
    CHECK(by_name.model->id == "claude-sonnet-4-5");
}

TEST_CASE(
    "parse_model_pattern handles :level suffixes with pi's warning semantics",
    "[coding_agent][model-resolver][issue407]") {
    const auto models = catalog();

    // Valid level suffix.
    const auto leveled = coding_agent::parse_model_pattern("alpha-1:high", models);
    REQUIRE(leveled.model.has_value());
    REQUIRE(leveled.thinking_level.has_value());
    CHECK(*leveled.thinking_level == "high");
    CHECK_FALSE(leveled.warning.has_value());

    // Invalid level suffix falls back to the prefix with a warning.
    const auto warned = coding_agent::parse_model_pattern("alpha-1:exacto", models);
    REQUIRE(warned.model.has_value());
    CHECK_FALSE(warned.thinking_level.has_value());
    REQUIRE(warned.warning.has_value());
    CHECK(warned.warning->find("Invalid thinking level \"exacto\" in pattern \"alpha-1:exacto\".") !=
        std::string::npos);

    // Strict mode treats the whole pattern as a model id and fails.
    const auto strict = coding_agent::parse_model_pattern(
        "alpha-1:exacto", models, /* allow_invalid_thinking_level_fallback */ false);
    CHECK_FALSE(strict.model.has_value());

    // Unknown pattern resolves to nothing.
    const auto unknown = coding_agent::parse_model_pattern("zzz", models);
    CHECK_FALSE(unknown.model.has_value());
    CHECK_FALSE(unknown.warning.has_value());
}

TEST_CASE(
    "resolve_model_scope_with_diagnostics matches globs with minimatch semantics and dedupes",
    "[coding_agent][model-resolver][issue407]") {
    const auto models = catalog();

    // Id prefix glob.
    const auto alpha_scope = coding_agent::resolve_model_scope_with_diagnostics({"alpha*"}, models);
    REQUIRE(alpha_scope.scoped_models.size() == 2);
    CHECK(alpha_scope.scoped_models[0].model.id == "alpha-1");
    CHECK(alpha_scope.scoped_models[1].model.id == "alpha-1-20250929");
    CHECK(alpha_scope.diagnostics.empty());

    // Glob with a :level suffix applies the level to every matched model.
    const auto leveled_scope = coding_agent::resolve_model_scope_with_diagnostics(
        {"alpha*:high"}, models);
    REQUIRE(leveled_scope.scoped_models.size() == 2);
    for (const auto& scoped : leveled_scope.scoped_models) {
        REQUIRE(scoped.thinking_level.has_value());
        CHECK(*scoped.thinking_level == "high");
    }

    // `*` does not cross `/`: the fullId form needs `**` or the bare id form.
    const auto no_cross = coding_agent::resolve_model_scope_with_diagnostics({"*sonnet*"}, models);
    REQUIRE(no_cross.scoped_models.size() == 2);
    CHECK(no_cross.scoped_models[0].model.id == "claude-sonnet-4-5");
    CHECK(no_cross.scoped_models[1].model.id == "claude-sonnet-4-5-20250929");

    // Provider-qualified glob.
    const auto provider_glob = coding_agent::resolve_model_scope_with_diagnostics(
        {"alpha/*"}, models);
    REQUIRE(provider_glob.scoped_models.size() == 2);

    // No-match diagnostics carry the pattern.
    const auto unmatched = coding_agent::resolve_model_scope_with_diagnostics({"gamma*"}, models);
    CHECK(unmatched.scoped_models.empty());
    REQUIRE(unmatched.diagnostics.size() == 1);
    CHECK(unmatched.diagnostics[0].code == "no-match");
    CHECK(unmatched.diagnostics[0].pattern == "gamma*");

    // Duplicates across patterns are dropped in pattern order.
    const auto deduped = coding_agent::resolve_model_scope_with_diagnostics(
        {"alpha*", "alpha-1"}, models);
    REQUIRE(deduped.scoped_models.size() == 2);
    CHECK(deduped.scoped_models[0].model.id == "alpha-1");
    CHECK(deduped.scoped_models[1].model.id == "alpha-1-20250929");

    // Plain patterns resolve through parseModelPattern.
    const auto plain = coding_agent::resolve_model_scope_with_diagnostics({"beta-1"}, models);
    REQUIRE(plain.scoped_models.size() == 1);
    CHECK(plain.scoped_models[0].model.provider == "beta");
}

TEST_CASE(
    "resolve_model_scope drops diagnostics and keeps the scoped models",
    "[coding_agent][model-resolver][issue407]") {
    const auto models = catalog();
    const auto scoped = coding_agent::resolve_model_scope({"alpha*", "missing*"}, models);
    REQUIRE(scoped.size() == 2);
    CHECK(scoped[0].model.id == "alpha-1");
}

TEST_CASE(
    "glob ** crosses slashes only as a full path segment like minimatch",
    "[coding_agent][model-resolver][issue407]") {
    auto models = catalog();
    models.push_back(model("alpha/sub/deep", "alpha"));

    // A full-segment `**` crosses `/` (minimatch globstar).
    const auto globstar = coding_agent::resolve_model_scope_with_diagnostics(
        {"alpha/**"}, models);
    REQUIRE(globstar.scoped_models.size() == 3);

    // A multi-star run mid-segment behaves like a single non-crossing `*`.
    const auto mid_segment = coding_agent::resolve_model_scope_with_diagnostics(
        {"alpha-*"}, models);
    REQUIRE(mid_segment.scoped_models.size() == 2);
    CHECK(mid_segment.scoped_models[0].model.id == "alpha-1");
    CHECK(mid_segment.scoped_models[1].model.id == "alpha-1-20250929");
}
