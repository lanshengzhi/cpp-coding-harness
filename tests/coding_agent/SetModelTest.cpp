// Runtime `setModel` (pi `agent-session.ts` `setModel`, G3 decision 5): the
// in-session model switch validates provider auth (`No API key for
// <provider>/<model>`), swaps the live Agent model, persists the
// `model_change` session entry, writes the global settings default, and
// re-clamps the thinking level against the new model. All sessions run
// against a default-created ModelRuntime over a temp Agent Config Directory
// with dummy-only values — no live credentials, no network validation.

#include "coding_agent/AgentSession.hpp"
#include <cch/coding_agent/Settings.hpp>
#include <cch/harness/session/JsonlSessionStore.hpp>
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/TempWorkspace.hpp"
#include "util/Json.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace cch;

namespace {

/// One isolated assembly fixture: a temp workspace for the session file and a
/// temp Agent Config Directory (`PI_CODING_AGENT_DIR`) whose models.json and
/// settings.json drive runtime creation deterministically.
struct Fixture {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard dir_guard{"PI_CODING_AGENT_DIR"};
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard kimi_guard{"KIMI_API_KEY"};
    std::filesystem::path session_file;

    Fixture() {
        dir_guard.set(agent_dir.path().string());
        home_guard.set(workspace.path().string());
        kimi_guard.unset();
        session_file = workspace.path() / "session.jsonl";
    }

    void write_models(std::string_view json) {
        std::ofstream out(agent_dir.path() / "models.json", std::ios::binary);
        out << json;
    }

    void write_settings(std::string_view json) {
        std::ofstream out(agent_dir.path() / "settings.json", std::ios::binary);
        out << json;
    }

    [[nodiscard]] std::string read_settings() const {
        std::ifstream in(agent_dir.path() / "settings.json", std::ios::binary);
        return std::string{
            std::istreambuf_iterator<char>{in},
            std::istreambuf_iterator<char>{}};
    }
};

/// Two config-only providers: `alpha` (model alpha-1) and `beta` (model
/// beta-1), each with a literal dummy apiKey so both resolve as configured.
constexpr std::string_view kTwoKeyedProviders = R"({
  "providers": {
    "alpha": {
      "baseUrl": "https://alpha.example/v1",
      "api": "openai-responses",
      "apiKey": "dummy-alpha-key",
      "models": [{"id": "alpha-1"}]
    },
    "beta": {
      "baseUrl": "https://beta.example/v1",
      "api": "openai-responses",
      "apiKey": "dummy-beta-key",
      "models": [{"id": "beta-1"}]
    }
  }
})";

/// `alpha` carries a key; `beta` is keyless (never resolves as configured).
constexpr std::string_view kKeyedAlphaKeylessBeta = R"({
  "providers": {
    "alpha": {
      "baseUrl": "https://alpha.example/v1",
      "api": "openai-responses",
      "apiKey": "dummy-alpha-key",
      "models": [{"id": "alpha-1"}]
    },
    "beta": {
      "baseUrl": "https://beta.example/v1",
      "api": "openai-responses",
      "models": [{"id": "beta-1"}]
    }
  }
})";

/// A keyed reasoning provider and a keyed non-reasoning provider for the
/// thinking re-clamp scenarios.
constexpr std::string_view kReasoningAndPlainProviders = R"({
  "providers": {
    "alpha": {
      "baseUrl": "https://alpha.example/v1",
      "api": "openai-responses",
      "apiKey": "dummy-alpha-key",
      "models": [{"id": "alpha-1", "reasoning": true}]
    },
    "beta": {
      "baseUrl": "https://beta.example/v1",
      "api": "openai-responses",
      "apiKey": "dummy-beta-key",
      "models": [{"id": "beta-1", "reasoning": false}]
    }
  }
})";

[[nodiscard]] const harness::session::SessionEntry* find_thinking_entry(
    const harness::session::LoadedSession& loaded) {
    const harness::session::SessionEntry* found = nullptr;
    for (const auto& entry : loaded.entries) {
        if (entry.kind == harness::session::SessionEntryKind::ThinkingLevelChange) {
            found = &entry;
        }
    }
    return found;
}

[[nodiscard]] coding_agent::runtime::AgentSessionCreationRequest cli_request(
    const Fixture& fixture) {
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.no_skills = true;
    request.no_prompt_templates = true;
    request.workspace = fixture.workspace.path();
    request.session_target =
        coding_agent::ExplicitOpenOrCreateSessionTarget{fixture.session_file};
    return request;
}

[[nodiscard]] const harness::session::SessionEntry* find_model_change_entry(
    const harness::session::LoadedSession& loaded) {
    const harness::session::SessionEntry* found = nullptr;
    for (const auto& entry : loaded.entries) {
        if (entry.kind == harness::session::SessionEntryKind::ModelChange) {
            found = &entry;
        }
    }
    return found;
}

[[nodiscard]] util::JsonValue settings_object(const Fixture& fixture) {
    const auto parsed = util::read_json(fixture.read_settings());
    REQUIRE(parsed.has_value());
    return *parsed;
}

} // namespace

TEST_CASE(
    "set_model rejects a switch to a provider with no configured auth",
    "[coding_agent][set-model][issue406]") {
    Fixture fixture;
    fixture.write_models(kKeyedAlphaKeylessBeta);

    auto result = coding_agent::create_agent_session(cli_request(fixture));
    REQUIRE(result.has_value());
    REQUIRE(result->model == "alpha-1");

    const auto target = result->session->model_runtime()->model("beta", "beta-1");
    REQUIRE(target.has_value());
    auto switched = result->session->set_model_blocking(*target);
    REQUIRE_FALSE(switched.has_value());
    CHECK(switched.error().message == "No API key for beta/beta-1");

    // The live model is unchanged and no model_change entry was persisted.
    CHECK(result->session->snapshot().agent_state.model.id == "alpha-1");
    result->session->close();

    auto loaded = harness::session::JsonlSessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    const auto* entry = find_model_change_entry(*loaded);
    REQUIRE(entry != nullptr);
    const auto& value = std::get<harness::session::ModelChangeValue>(entry->value);
    CHECK(value.provider == "alpha");
    CHECK(value.model_id == "alpha-1");
}

TEST_CASE(
    "set_model switches the live model, persists the model_change entry, and writes the settings default",
    "[coding_agent][set-model][issue406]") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);

    auto result = coding_agent::create_agent_session(cli_request(fixture));
    REQUIRE(result.has_value());
    REQUIRE(result->model == "alpha-1");

    const auto target = result->session->model_runtime()->model("beta", "beta-1");
    REQUIRE(target.has_value());
    auto switched = result->session->set_model_blocking(*target);
    REQUIRE(switched.has_value());

    // The live Agent state carries the new model (pi `agent.state.model`).
    const auto snapshot = result->session->snapshot();
    CHECK(snapshot.agent_state.model.provider == "beta");
    CHECK(snapshot.agent_state.model.id == "beta-1");
    result->session->close();

    // The durable session file carries the new `model_change` entry.
    auto loaded = harness::session::JsonlSessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    const auto* entry = find_model_change_entry(*loaded);
    REQUIRE(entry != nullptr);
    const auto& value = std::get<harness::session::ModelChangeValue>(entry->value);
    CHECK(value.provider == "beta");
    CHECK(value.model_id == "beta-1");

    // The global settings file carries the default provider/model write (pi
    // `setDefaultModelAndProvider` writes the global scope).
    const auto settings = settings_object(fixture).get_object();
    const auto provider = settings.find("defaultProvider");
    REQUIRE(provider != settings.end());
    const auto* provider_value = provider->second.get_if<std::string>();
    REQUIRE(provider_value != nullptr);
    CHECK(*provider_value == "beta");
    const auto model = settings.find("defaultModel");
    REQUIRE(model != settings.end());
    const auto* model_value = model->second.get_if<std::string>();
    REQUIRE(model_value != nullptr);
    CHECK(*model_value == "beta-1");

    // A reloaded settings manager sees the merged default too.
    auto reloaded = coding_agent::SettingsManager::create(
        fixture.workspace.path(), fixture.agent_dir.path(), /* project_trusted */ false);
    CHECK(reloaded.settings().default_provider == "beta");
    CHECK(reloaded.settings().default_model == "beta-1");
}

TEST_CASE(
    "the model bash tool's live PI_* facts follow the session model and thinking level",
    "[coding_agent][set-model][issue414]") {
    Fixture fixture;
    fixture.write_models(kReasoningAndPlainProviders);

    auto request = cli_request(fixture);
    auto bash_environment =
        std::make_shared<tools::BashSessionEnvironment>();
    request.bash_session_environment = bash_environment;
    auto result = coding_agent::create_agent_session(std::move(request));
    REQUIRE(result.has_value());
    REQUIRE(result->model == "alpha-1");

    // At session construction the holder carries the live session facts (pi
    // `resolveSpawnContext`: session id always, session file for persisted
    // sessions, model and clamped thinking level).
    CHECK(bash_environment->session_id == result->session_id);
    REQUIRE(bash_environment->session_file.has_value());
    CHECK(*bash_environment->session_file == fixture.session_file.string());
    CHECK(bash_environment->provider == "alpha");
    CHECK(bash_environment->model == "alpha-1");
    CHECK(bash_environment->reasoning_level == "medium");

    // A direct thinking change refreshes the holder's reasoning level.
    auto raised = result->session->set_thinking_level("high");
    REQUIRE(raised.has_value());
    CHECK(*raised == "high");
    CHECK(bash_environment->reasoning_level == "high");

    // set_model refreshes the holder from the new live Agent state, and the
    // thinking re-clamp follows.
    const auto target = result->session->model_runtime()->model("beta", "beta-1");
    REQUIRE(target.has_value());
    auto switched = result->session->set_model_blocking(*target);
    REQUIRE(switched.has_value());
    CHECK(bash_environment->provider == "beta");
    CHECK(bash_environment->model == "beta-1");
    // beta-1 has no reasoning support: the level re-clamps to off.
    CHECK(bash_environment->reasoning_level == "off");
    result->session->close();
}

TEST_CASE(
    "set_model to a non-thinking model re-clamps the level and persists the thinking_level_change entry",
    "[coding_agent][set-model][issue406]") {
    Fixture fixture;
    fixture.write_models(kReasoningAndPlainProviders);

    auto result = coding_agent::create_agent_session(cli_request(fixture));
    REQUIRE(result.has_value());
    REQUIRE(result->model == "alpha-1");
    auto raised = result->session->set_thinking_level("high");
    REQUIRE(raised.has_value());
    REQUIRE(*raised == "high");

    const auto target = result->session->model_runtime()->model("beta", "beta-1");
    REQUIRE(target.has_value());
    auto switched = result->session->set_model_blocking(*target);
    REQUIRE(switched.has_value());

    // The kept level clamps against the non-reasoning model (pi setModel →
    // setThinkingLevel).
    CHECK(result->session->snapshot().agent_state.thinking_level == "off");
    result->session->close();

    auto loaded = harness::session::JsonlSessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    const auto* entry = find_thinking_entry(*loaded);
    REQUIRE(entry != nullptr);
    const auto& value =
        std::get<harness::session::ThinkingLevelChangeValue>(entry->value);
    CHECK(value.thinking_level == "off");

    // pi's settings-write gate (`supportsThinking() || effectiveLevel !==
    // "off"`): the clamped "off" on a non-reasoning model is not persisted as
    // the settings default; the earlier "high" write stays untouched.
    const auto settings = settings_object(fixture).get_object();
    const auto level = settings.find("defaultThinkingLevel");
    REQUIRE(level != settings.end());
    const auto* level_value = level->second.get_if<std::string>();
    REQUIRE(level_value != nullptr);
    CHECK(*level_value == "high");
}

TEST_CASE(
    "set_model from a non-thinking model restores the settings default thinking level",
    "[coding_agent][set-model][issue406]") {
    Fixture fixture;
    fixture.write_models(kReasoningAndPlainProviders);
    fixture.write_settings(R"({"defaultProvider": "beta", "defaultModel": "beta-1", "defaultThinkingLevel": "high"})");

    auto result = coding_agent::create_agent_session(cli_request(fixture));
    REQUIRE(result.has_value());
    // The settings default resolves beta-1 (non-reasoning); the creation
    // clamp lands the level at "off".
    REQUIRE(result->model == "beta-1");
    REQUIRE(result->session->snapshot().agent_state.thinking_level == "off");

    const auto target = result->session->model_runtime()->model("alpha", "alpha-1");
    REQUIRE(target.has_value());
    auto switched = result->session->set_model_blocking(*target);
    REQUIRE(switched.has_value());

    // pi `_getThinkingLevelForModelSwitch`: the current model supports no
    // thinking, so the merged settings default wins and clamps against the
    // new reasoning model.
    CHECK(result->session->snapshot().agent_state.thinking_level == "high");
    result->session->close();

    auto loaded = harness::session::JsonlSessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    const auto* entry = find_thinking_entry(*loaded);
    REQUIRE(entry != nullptr);
    const auto& value =
        std::get<harness::session::ThinkingLevelChangeValue>(entry->value);
    CHECK(value.thinking_level == "high");
}

TEST_CASE(
    "set_model on an in-memory session skips the model_change entry and still writes settings",
    "[coding_agent][set-model][issue406]") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);

    coding_agent::runtime::AgentSessionCreationRequest request;
    request.no_skills = true;
    request.no_prompt_templates = true;
    request.workspace = fixture.workspace.path();
    request.session_target = coding_agent::InMemorySessionTarget{};

    auto result = coding_agent::create_agent_session(std::move(request));
    REQUIRE(result.has_value());
    REQUIRE(result->model == "alpha-1");

    const auto target = result->session->model_runtime()->model("beta", "beta-1");
    REQUIRE(target.has_value());
    auto switched = result->session->set_model_blocking(*target);
    REQUIRE(switched.has_value());
    CHECK(result->session->snapshot().agent_state.model.id == "beta-1");
    result->session->close();

    const auto settings = settings_object(fixture).get_object();
    const auto provider = settings.find("defaultProvider");
    REQUIRE(provider != settings.end());
    const auto* provider_value = provider->second.get_if<std::string>();
    REQUIRE(provider_value != nullptr);
    CHECK(*provider_value == "beta");

    // No session file exists for the in-memory target.
    std::error_code exists_error;
    CHECK_FALSE(std::filesystem::exists(fixture.session_file, exists_error));
}
