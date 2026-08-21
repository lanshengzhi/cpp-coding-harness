// Host-side session-replacement request finalization (issue #507): the
// production `ReplaceSessionAction` handling of the interactive CLI host,
// exercised directly so the production-path trust handling is covered (the
// interactive suites inject their own action sink and never see it). The
// engine resolves session trust (pi `projectTrustByCwd`: CLI override → boot
// prompt decision → boot-workspace inheritance); the host must preserve an
// engine-resolved decision and only fill an unset one from the CLI facts.
// The pure CLI-owned resource and model facts stay host-authoritative, and
// the host supplies the host-only capabilities.

#include "cli/SessionReplacementHost.hpp"

#include "agent/harness/RuntimeRoot.hpp"
#include "coding_agent/runtime/AgentSessionCreationRequest.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include "support/FakeModelRuntime.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/io_context.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
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
    "the replacement host preserves an engine-resolved trust decision over empty CLI facts",
    "[cli][session-replacement][issue507]") {
    // The boot prompt's session-only "trust" answer must survive host
    // finalization even without a CLI --approve flag (the #507 discard).
    auto trusted = engine_request();
    trusted.project_trust_override = true;
    auto finalized_trusted = cli::finalize_replacement_session_request(
        std::move(trusted), empty_facts(), nullptr, nullptr);
    REQUIRE(finalized_trusted.project_trust_override.has_value());
    CHECK(*finalized_trusted.project_trust_override);

    // The session-only "do not trust" answer survives the same way.
    auto untrusted = engine_request();
    untrusted.project_trust_override = false;
    auto finalized_untrusted = cli::finalize_replacement_session_request(
        std::move(untrusted), empty_facts(), nullptr, nullptr);
    REQUIRE(finalized_untrusted.project_trust_override.has_value());
    CHECK_FALSE(*finalized_untrusted.project_trust_override);
}

TEST_CASE(
    "the replacement host lets an engine-resolved trust decision win over a differing CLI override",
    "[cli][session-replacement][issue507]") {
    // The engine already merges the CLI override into its resolution (pi
    // `projectTrustByCwd` precedence), so a set request value is always the
    // authoritative one — the raw facts must never overwrite it.
    auto facts = empty_facts();
    facts.project_trust_override = false;
    auto request = engine_request();
    request.project_trust_override = true;

    const auto finalized = cli::finalize_replacement_session_request(
        std::move(request), facts, nullptr, nullptr);

    REQUIRE(finalized.project_trust_override.has_value());
    CHECK(*finalized.project_trust_override);
}

TEST_CASE(
    "the replacement host fills an unset trust decision from the CLI facts",
    "[cli][session-replacement][issue507]") {
    auto facts = empty_facts();
    facts.project_trust_override = true;
    auto approved = cli::finalize_replacement_session_request(
        engine_request(), facts, nullptr, nullptr);
    REQUIRE(approved.project_trust_override.has_value());
    CHECK(*approved.project_trust_override);

    facts.project_trust_override = false;
    auto rejected = cli::finalize_replacement_session_request(
        engine_request(), facts, nullptr, nullptr);
    REQUIRE(rejected.project_trust_override.has_value());
    CHECK_FALSE(*rejected.project_trust_override);
}

TEST_CASE(
    "the replacement host leaves trust unresolved when neither side sets it",
    "[cli][session-replacement][issue507]") {
    // SessionFactory then resolves through the trust store and the default
    // trust policy.
    const auto finalized = cli::finalize_replacement_session_request(
        engine_request(), empty_facts(), nullptr, nullptr);
    CHECK_FALSE(finalized.project_trust_override.has_value());
}

TEST_CASE(
    "the replacement host re-applies the CLI-owned resource prompt and model facts",
    "[cli][session-replacement][issue507]") {
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

    const auto finalized = cli::finalize_replacement_session_request(
        engine_request(), facts, nullptr, nullptr);

    CHECK(finalized.no_skills);
    CHECK(finalized.no_prompt_templates);
    CHECK(finalized.prompt_template_paths == std::vector<std::string>{"prompts/extra.md"});
    CHECK(finalized.skill_paths == std::vector<std::string>{"skills/extra"});
    CHECK(finalized.no_themes);
    CHECK(finalized.theme_paths == std::vector<std::string>{"themes/one.json"});
    CHECK(finalized.no_context_files);
    REQUIRE(finalized.system_prompt.has_value());
    CHECK(*finalized.system_prompt == "system text");
    CHECK(
        finalized.append_system_prompt ==
        std::vector<std::string>{"append one", "append two"});
    REQUIRE(finalized.provider.has_value());
    CHECK(*finalized.provider == "fake");
    REQUIRE(finalized.model.has_value());
    CHECK(*finalized.model == "fake-model");
    CHECK(finalized.models == std::vector<std::string>{"fake/fake-model"});
    REQUIRE(finalized.api_key.has_value());
    CHECK(*finalized.api_key == "sk-test");
}

TEST_CASE(
    "the replacement host installs the host capabilities",
    "[cli][session-replacement][issue507]") {
    auto io = std::make_shared<boost::asio::io_context>();
    harness::RuntimeRoot root{io, harness::RuntimeLimits{}};
    auto target = root.make_target();
    auto shared_runtime = std::make_shared<tests::FakeModelRuntime>();

    auto finalized = cli::finalize_replacement_session_request(
        engine_request(), empty_facts(), std::move(target), shared_runtime);

    // The interactive host always assembles the Session-owned User Shell
    // (ADR 0026) even though the engine request left it unset.
    CHECK(finalized.provide_user_shell);
    CHECK(finalized.execution_runtime_target != nullptr);
    REQUIRE(finalized.model_runtime != nullptr);
    CHECK(finalized.model_runtime == shared_runtime);

    // A null shared runtime leaves the request's model_runtime untouched so
    // the factory default-creates a Session-owned one (issue #466).
    auto preserved = engine_request();
    preserved.model_runtime = shared_runtime;
    finalized = cli::finalize_replacement_session_request(
        std::move(preserved), empty_facts(), nullptr, nullptr);
    REQUIRE(finalized.model_runtime != nullptr);
    CHECK(finalized.model_runtime == shared_runtime);

    root.close();
}

TEST_CASE(
    "the replacement host keeps the engine session intent and workspace",
    "[cli][session-replacement][issue507]") {
    // Finalization never touches the per-flow fields the engine owns: the
    // workspace, the session target, and the in-session overrides.
    auto request = engine_request();
    request.session_name = "named";
    request.resume_cwd_override = std::filesystem::path{"/elsewhere"};

    const auto finalized = cli::finalize_replacement_session_request(
        std::move(request), empty_facts(), nullptr, nullptr);

    CHECK(finalized.workspace == std::filesystem::path{"/workspace"});
    CHECK(
        std::holds_alternative<coding_agent::InMemorySessionTarget>(
            finalized.session_target));
    REQUIRE(finalized.session_name.has_value());
    CHECK(*finalized.session_name == "named");
    REQUIRE(finalized.resume_cwd_override.has_value());
    CHECK(*finalized.resume_cwd_override == std::filesystem::path{"/elsewhere"});
}

