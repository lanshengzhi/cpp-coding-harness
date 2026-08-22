// Session replacement CLI-facts merge (issues #507 and #511, map #508): the
// Session Assembly boundary owns applying CLI-owned facts onto an
// engine-built session-creation request (D5 — the merge was absorbed from
// the deleted cli/SessionReplacementHost finalization and is exercised here
// through SessionFactory::apply_cli_facts). The engine resolves session
// trust (pi `projectTrustByCwd`: CLI override → boot prompt decision →
// boot-workspace inheritance); the merge must preserve an engine-resolved
// decision and only fill an unset one from the CLI facts. The pure CLI-owned
// resource and model facts stay host-authoritative (re-applied
// unconditionally), and the host-only capabilities (User Shell, Runtime
// target, shared Models runtime) stay outside the merge.

#include "coding_agent/runtime/SessionFactory.hpp"

#include "support/FakeModelRuntime.hpp"
#include "support/ModelsFixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

/// A replacement request as the engine delivers it: the session-stateful
/// fields resolved, the pure CLI-owned facts either mirrored or omitted.
[[nodiscard]] coding_agent::runtime::AgentSessionCreationRequest engine_request() {
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.workspace = "/workspace";
    request.session_target = coding_agent::InMemorySessionTarget{};
    return request;
}

[[nodiscard]] coding_agent::runtime::InteractiveSessionFacts empty_facts() {
    return coding_agent::runtime::InteractiveSessionFacts{};
}

} // namespace

TEST_CASE(
    "the CLI-facts merge preserves an engine-resolved trust decision over empty CLI facts",
    "[coding_agent][runtime][session-replacement][issue507][issue511]") {
    // The boot prompt's session-only "trust" answer must survive the facts
    // merge even without a CLI --approve flag (the #507 discard).
    auto trusted = engine_request();
    trusted.project_trust_override = true;
    coding_agent::runtime::SessionFactory::apply_cli_facts(
        trusted, empty_facts());
    REQUIRE(trusted.project_trust_override.has_value());
    CHECK(*trusted.project_trust_override);

    // The session-only "do not trust" answer survives the same way.
    auto untrusted = engine_request();
    untrusted.project_trust_override = false;
    coding_agent::runtime::SessionFactory::apply_cli_facts(
        untrusted, empty_facts());
    REQUIRE(untrusted.project_trust_override.has_value());
    CHECK_FALSE(*untrusted.project_trust_override);
}

TEST_CASE(
    "the CLI-facts merge lets an engine-resolved trust decision win over a differing CLI override",
    "[coding_agent][runtime][session-replacement][issue507][issue511]") {
    // The engine already merges the CLI override into its resolution (pi
    // `projectTrustByCwd` precedence), so a set request value is always the
    // authoritative one — the raw facts must never overwrite it.
    auto facts = empty_facts();
    facts.project_trust_override = false;
    auto request = engine_request();
    request.project_trust_override = true;

    coding_agent::runtime::SessionFactory::apply_cli_facts(request, facts);

    REQUIRE(request.project_trust_override.has_value());
    CHECK(*request.project_trust_override);
}

TEST_CASE(
    "the CLI-facts merge fills an unset trust decision from the CLI facts",
    "[coding_agent][runtime][session-replacement][issue507][issue511]") {
    auto facts = empty_facts();
    facts.project_trust_override = true;
    auto approved = engine_request();
    coding_agent::runtime::SessionFactory::apply_cli_facts(approved, facts);
    REQUIRE(approved.project_trust_override.has_value());
    CHECK(*approved.project_trust_override);

    facts.project_trust_override = false;
    auto rejected = engine_request();
    coding_agent::runtime::SessionFactory::apply_cli_facts(rejected, facts);
    REQUIRE(rejected.project_trust_override.has_value());
    CHECK_FALSE(*rejected.project_trust_override);
}

TEST_CASE(
    "the CLI-facts merge leaves trust unresolved when neither side sets it",
    "[coding_agent][runtime][session-replacement][issue507][issue511]") {
    // Session assembly then resolves through the trust store and the default
    // trust policy.
    auto request = engine_request();
    coding_agent::runtime::SessionFactory::apply_cli_facts(
        request, empty_facts());
    CHECK_FALSE(request.project_trust_override.has_value());
}

TEST_CASE(
    "the CLI-facts merge re-applies the CLI-owned resource prompt and model facts",
    "[coding_agent][runtime][session-replacement][issue507][issue511]") {
    // Host-authoritative pure CLI facts: load-bearing for the fields
    // `make_session_request` deliberately omits (themes, context files,
    // system prompts) and an idempotent mirror of the engine-set ones.
    auto facts = empty_facts();
    facts.no_skills = true;
    facts.no_prompt_templates = true;
    facts.prompt_template_paths = {"prompts/extra.md"};
    facts.skill_paths = {"skills/extra"};
    facts.no_themes = true;
    facts.theme_paths = {"themes/one.json"};
    facts.no_context_files = true;
    facts.system_prompt = "system text";
    facts.append_system_prompt = {"append one", "append two"};
    facts.provider = "fake";
    facts.model = "fake-model";
    facts.models = {"fake/fake-model"};
    facts.api_key = "sk-test";

    auto request = engine_request();
    coding_agent::runtime::SessionFactory::apply_cli_facts(request, facts);

    CHECK(request.session_facts.no_skills);
    CHECK(request.session_facts.no_prompt_templates);
    CHECK(request.session_facts.prompt_template_paths == std::vector<std::string>{"prompts/extra.md"});
    CHECK(request.session_facts.skill_paths == std::vector<std::string>{"skills/extra"});
    CHECK(request.session_facts.no_themes);
    CHECK(request.session_facts.theme_paths == std::vector<std::string>{"themes/one.json"});
    CHECK(request.session_facts.no_context_files);
    REQUIRE(request.session_facts.system_prompt.has_value());
    CHECK(*request.session_facts.system_prompt == "system text");
    CHECK(
        request.session_facts.append_system_prompt ==
        std::vector<std::string>{"append one", "append two"});
    REQUIRE(request.session_facts.provider.has_value());
    CHECK(*request.session_facts.provider == "fake");
    REQUIRE(request.session_facts.model.has_value());
    CHECK(*request.session_facts.model == "fake-model");
    CHECK(request.session_facts.models == std::vector<std::string>{"fake/fake-model"});
    REQUIRE(request.session_facts.api_key.has_value());
    CHECK(*request.session_facts.api_key == "sk-test");
}

TEST_CASE(
    "the CLI-facts merge leaves the host-only capabilities to the host",
    "[coding_agent][runtime][session-replacement][issue507][issue511]") {
    // Host-only capabilities (issue #507): the interactive Session's
    // independent User Shell (ADR 0026), the CLI Runtime root's target, and
    // the host-shared Models runtime (issue #466) are set by the host on the
    // request — the facts merge neither installs nor clears them.
    auto bare = engine_request();
    coding_agent::runtime::SessionFactory::apply_cli_facts(
        bare, empty_facts());
    CHECK_FALSE(bare.provide_user_shell);
    CHECK(bare.execution_runtime_target == nullptr);
    CHECK(bare.model_runtime == nullptr);

    auto installed = engine_request();
    installed.provide_user_shell = true;
    installed.execution_runtime_target = tests::detail::fixture_runtime_target();
    installed.model_runtime = std::make_shared<tests::FakeModelRuntime>();
    const auto target = installed.execution_runtime_target;
    const auto runtime = installed.model_runtime;

    coding_agent::runtime::SessionFactory::apply_cli_facts(
        installed, empty_facts());

    CHECK(installed.provide_user_shell);
    CHECK(installed.execution_runtime_target == target);
    CHECK(installed.model_runtime == runtime);
}

TEST_CASE(
    "the CLI-facts merge keeps the engine session intent and workspace",
    "[coding_agent][runtime][session-replacement][issue507][issue511]") {
    // The merge never touches the per-flow fields the engine owns: the
    // workspace, the session target, and the in-session overrides.
    auto request = engine_request();
    request.session_name = "named";
    request.resume_cwd_override = std::filesystem::path{"/elsewhere"};

    coding_agent::runtime::SessionFactory::apply_cli_facts(
        request, empty_facts());

    CHECK(request.workspace == std::filesystem::path{"/workspace"});
    CHECK(
        std::holds_alternative<coding_agent::InMemorySessionTarget>(
            request.session_target));
    REQUIRE(request.session_name.has_value());
    CHECK(*request.session_name == "named");
    REQUIRE(request.resume_cwd_override.has_value());
    CHECK(*request.resume_cwd_override == std::filesystem::path{"/elsewhere"});
}
