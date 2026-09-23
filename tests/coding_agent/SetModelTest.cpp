// Runtime `setModel` (pi `agent-session.ts` `setModel`): the in-session
// model switch validates provider auth (`No API key for
// <provider>/<model>`), swaps the live Agent model, persists the
// `model_change` session entry, and re-clamps the thinking level against the
// new model — session-only by default (pi `ModelMutationOptions`), with the
// global settings default and scope promotion under an explicit persist.
// All sessions run against a default-created ModelRuntime over a temp Agent
// Config Directory with dummy-only values — no live credentials, no network
// validation.

#include "coding_agent/AgentSession.hpp"
#include <cch/coding_agent/Settings.hpp>
#include <cch/agent/harness/session/SessionStore.hpp>
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/Json.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "support/AgentRootFixture.hpp"

using namespace cch;

namespace {

/// One isolated assembly fixture: a temp workspace for the session file and a
/// temp Agent Config Directory (`HOME`) whose models.json and
/// settings.json drive runtime creation deterministically.
struct Fixture {
    cch::tests::TempWorkspace workspace;
    std::filesystem::path agent_dir;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard kimi_guard{"KIMI_API_KEY"};
    std::filesystem::path session_file;
    tests::RuntimeFixture runtime;

    Fixture() {
        home_guard.set(workspace.path().string());
        agent_dir = tests::agent_root_under_home(workspace.path());
        std::filesystem::create_directories(agent_dir);
        kimi_guard.unset();
        session_file = workspace.path() / "session.jsonl";
    }

    void write_models(std::string_view json) {
        std::ofstream out(agent_dir / "models.json", std::ios::binary);
        out << json;
    }

    void write_settings(std::string_view json) {
        std::ofstream out(agent_dir / "settings.json", std::ios::binary);
        out << json;
    }

    [[nodiscard]] std::string read_settings() const {
        std::ifstream in(agent_dir / "settings.json", std::ios::binary);
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

/// Two keyed reasoning providers for the model-switch thinking-level
/// fallback chain.
constexpr std::string_view kTwoReasoningProviders = R"({
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
      "models": [{"id": "beta-1", "reasoning": true}]
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
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.workspace = fixture.workspace.path();
    request.session_target =
        coding_agent::ExplicitOpenOrCreateSessionTarget{fixture.session_file};
    request.execution_runtime_target = fixture.runtime.make_target();
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

[[nodiscard]] support::JsonValue settings_object(const Fixture& fixture) {
    const auto parsed = support::read_json(fixture.read_settings());
    REQUIRE(parsed.has_value());
    return *parsed;
}

} // namespace

TEST_CASE("set_model rejects a switch to a provider with no configured auth",
        "[coding_agent][set-model][issue406][spec]") {
    Fixture fixture;
    fixture.write_models(kKeyedAlphaKeylessBeta);

    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
    REQUIRE(result.has_value());
    REQUIRE(result->resolved_identity.model == "alpha-1");

    const auto target = result->session->model_runtime()->model("beta", "beta-1");
    REQUIRE(target.has_value());
    auto switched = result->session->set_model_blocking(*target);
    REQUIRE_FALSE(switched.has_value());
    CHECK(switched.error().message == "No API key for beta/beta-1");

    // The live model is unchanged and no model_change entry was persisted.
    CHECK(result->session->snapshot().agent_state.model.id == "alpha-1");
    result->session->close();

    auto loaded = harness::session::SessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    const auto* entry = find_model_change_entry(*loaded);
    REQUIRE(entry != nullptr);
    const auto& value = std::get<harness::session::ModelChangeValue>(entry->value);
    CHECK(value.provider == "alpha");
    CHECK(value.model_id == "alpha-1");
}

TEST_CASE("set_model switches the live model and persists the model_change entry session-only by default",
        "[coding_agent][set-model][issue406][spec]") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);

    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
    REQUIRE(result.has_value());
    REQUIRE(result->resolved_identity.model == "alpha-1");

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
    auto loaded = harness::session::SessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    const auto* entry = find_model_change_entry(*loaded);
    REQUIRE(entry != nullptr);
    const auto& value = std::get<harness::session::ModelChangeValue>(entry->value);
    CHECK(value.provider == "beta");
    CHECK(value.model_id == "beta-1");

    // Session-only by default (pi ModelMutationOptions): no settings default
    // write. A no-settings file stays absent.
    CHECK(fixture.read_settings().empty());
}

TEST_CASE("set_model writes the global default and promotes into the scope under an explicit persist",
        "[coding_agent][set-model][issue774][spec]") {
    // First: a session-only scope with no configured enabledModels — the
    // model joins the scope but no enabledModels field lands.
    {
        Fixture fixture;
        fixture.write_models(kTwoKeyedProviders);
        auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
        REQUIRE(result.has_value());
        REQUIRE(result->resolved_identity.model == "alpha-1");
        result->session->set_scoped_models({
                coding_agent::ScopedModel{.model = *result->session->model_runtime()->model("alpha", "alpha-1")},
        });

        const auto target = result->session->model_runtime()->model("beta", "beta-1");
        REQUIRE(target.has_value());
        auto switched =
                result->session->set_model_blocking(*target, coding_agent::ModelMutationOptions{.persist = true});
        REQUIRE(switched.has_value());

        // pi `_addPersistedDefaultToNonEmptyScope`: the model joins the
        // session scope without a thinking level.
        REQUIRE(result->session->scoped_models().size() == 2);
        CHECK(result->session->scoped_models()[1].model.id == "beta-1");
        CHECK_FALSE(result->session->scoped_models()[1].thinking_level.has_value());
        result->session->close();

        const auto settings = support::read_json(fixture.read_settings());
        REQUIRE(settings.has_value());
        const auto& object = settings->get_object();
        const auto provider = object.find("defaultProvider");
        REQUIRE(provider != object.end());
        CHECK(*provider->second.get_if<std::string>() == "beta");
        const auto model = object.find("defaultModel");
        REQUIRE(model != object.end());
        CHECK(*model->second.get_if<std::string>() == "beta-1");
        // The scope existed but configured enabledModels did not, so pi's
        // promotion tail leaves the field untouched.
        CHECK(object.find("enabledModels") == object.end());
    }

    // Second: configured enabledModels seed a session scope; a persisted
    // switch appends the `provider/id` reference.
    {
        Fixture fixture;
        fixture.write_models(kTwoKeyedProviders);
        fixture.write_settings(R"({"enabledModels": ["alpha-1"]})");
        auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
        REQUIRE(result.has_value());
        REQUIRE(result->resolved_identity.model == "alpha-1");
        REQUIRE(result->session->scoped_models().size() == 1);

        const auto target = result->session->model_runtime()->model("beta", "beta-1");
        REQUIRE(target.has_value());
        auto switched =
                result->session->set_model_blocking(*target, coding_agent::ModelMutationOptions{.persist = true});
        REQUIRE(switched.has_value());

        REQUIRE(result->session->scoped_models().size() == 2);
        CHECK(result->session->scoped_models()[1].model.id == "beta-1");
        result->session->close();

        const auto settings = support::read_json(fixture.read_settings());
        REQUIRE(settings.has_value());
        const auto& object = settings->get_object();
        const auto enabled = object.find("enabledModels");
        REQUIRE(enabled != object.end());
        const auto* patterns = enabled->second.get_if<support::JsonValue::array_t>();
        REQUIRE(patterns != nullptr);
        REQUIRE(patterns->size() == 2);
        const auto* reference = (*patterns)[1].get_if<std::string>();
        REQUIRE(reference != nullptr);
        CHECK(*reference == "beta/beta-1");
    }
}

TEST_CASE("set_model skips the scope promotion when the session scope is empty",
        "[coding_agent][set-model][issue774][spec]") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);
    // Configured enabledModels exist, but the session scope was replaced with
    // the empty set (pi setScopedModels): the persisted switch writes only
    // the global default — pi's `_addPersistedDefaultToNonEmptyScope` early
    // return keeps the configured scope untouched.
    fixture.write_settings(R"({"enabledModels": ["alpha-1"]})");

    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
    REQUIRE(result.has_value());
    REQUIRE(result->session->scoped_models().size() == 1);
    result->session->set_scoped_models({});
    REQUIRE(result->session->scoped_models().empty());

    const auto target = result->session->model_runtime()->model("beta", "beta-1");
    REQUIRE(target.has_value());
    auto switched = result->session->set_model_blocking(*target, coding_agent::ModelMutationOptions{.persist = true});
    REQUIRE(switched.has_value());
    CHECK(result->session->scoped_models().empty());
    result->session->close();

    const auto settings = support::read_json(fixture.read_settings());
    REQUIRE(settings.has_value());
    const auto& object = settings->get_object();
    const auto enabled = object.find("enabledModels");
    REQUIRE(enabled != object.end());
    const auto* patterns = enabled->second.get_if<support::JsonValue::array_t>();
    REQUIRE(patterns != nullptr);
    REQUIRE(patterns->size() == 1);
    const auto* reference = (*patterns)[0].get_if<std::string>();
    REQUIRE(reference != nullptr);
    CHECK(*reference == "alpha-1");
    const auto provider = object.find("defaultProvider");
    REQUIRE(provider != object.end());
    CHECK(*provider->second.get_if<std::string>() == "beta");
}

TEST_CASE("the model bash tool's live PI_* facts follow the session model and thinking level",
        "[coding_agent][set-model][issue414][spec]") {
    Fixture fixture;
    fixture.write_models(kReasoningAndPlainProviders);

    auto request = cli_request(fixture);
    auto bash_environment =
        std::make_shared<tools::BashSessionEnvironment>();
    request.bash_session_environment = bash_environment;
    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request)));
    REQUIRE(result.has_value());
    REQUIRE(result->resolved_identity.model == "alpha-1");

    // At session construction the holder carries the live session facts (pi
    // `resolveSpawnContext`: session id always, session file for persisted
    // sessions, model and clamped thinking level).
    CHECK(bash_environment->session_id == result->resolved_identity.session_id);
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

TEST_CASE("set_model to a non-thinking model re-clamps the level and persists the thinking_level_change entry",
        "[coding_agent][set-model][issue406][spec]") {
    Fixture fixture;
    fixture.write_models(kReasoningAndPlainProviders);

    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
    REQUIRE(result.has_value());
    REQUIRE(result->resolved_identity.model == "alpha-1");
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

    auto loaded = harness::session::SessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    const auto* entry = find_thinking_entry(*loaded);
    REQUIRE(entry != nullptr);
    const auto& value =
        std::get<harness::session::ThinkingLevelChangeValue>(entry->value);
    CHECK(value.thinking_level == "off");

    // pi's settings-write gate is gone with the mutation options: the switch
    // is session-only, so the clamped "off" lands in the transcript entry
    // only — the settings file stays untouched.
    CHECK(fixture.read_settings().empty());
}

TEST_CASE("set_thinking_level writes the requested level to the global default only under persist",
        "[coding_agent][set-model][issue774][spec]") {
    Fixture fixture;
    fixture.write_models(kReasoningAndPlainProviders);

    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
    REQUIRE(result.has_value());
    REQUIRE(result->resolved_identity.model == "alpha-1");

    // Session-only by default: a real change appends the entry and writes no
    // settings default.
    auto session_only = result->session->set_thinking_level("high");
    REQUIRE(session_only.has_value());
    CHECK(*session_only == "high");
    CHECK(fixture.read_settings().empty());

    // pi setThinkingLevel: under `options.persist` the *requested* level —
    // not the clamped state — lands in the global settings default, even
    // when the requested level re-clamps to the current live level.
    auto persisted = result->session->set_thinking_level("high", coding_agent::ModelMutationOptions{.persist = true});
    REQUIRE(persisted.has_value());
    CHECK(*persisted == "high");
    result->session->close();

    const auto settings = settings_object(fixture).get_object();
    const auto level = settings.find("defaultThinkingLevel");
    REQUIRE(level != settings.end());
    const auto* level_value = level->second.get_if<std::string>();
    REQUIRE(level_value != nullptr);
    CHECK(*level_value == "high");
}

TEST_CASE("set_model re-clamps the level and never rewrites the global thinking default",
        "[coding_agent][set-model][issue774][spec]") {
    Fixture fixture;
    fixture.write_models(kReasoningAndPlainProviders);

    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
    REQUIRE(result.has_value());
    REQUIRE(result->resolved_identity.model == "alpha-1");
    auto raised = result->session->set_thinking_level("high", coding_agent::ModelMutationOptions{.persist = true});
    REQUIRE(raised.has_value());
    REQUIRE(*raised == "high");

    const auto target = result->session->model_runtime()->model("beta", "beta-1");
    REQUIRE(target.has_value());
    auto switched = result->session->set_model_blocking(*target, coding_agent::ModelMutationOptions{.persist = true});
    REQUIRE(switched.has_value());

    // The kept level clamps against the non-reasoning model (pi setModel →
    // setThinkingLevel) and the clamped "off" entry rides the transcript —
    // but pi passes no persist here ("Model persistence does not implicitly
    // rewrite the global thinking default"), so the earlier "high" write
    // stays untouched.
    CHECK(result->session->snapshot().agent_state.thinking_level == "off");
    result->session->close();

    auto loaded = harness::session::SessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    const auto* entry = find_thinking_entry(*loaded);
    REQUIRE(entry != nullptr);
    const auto& value = std::get<harness::session::ThinkingLevelChangeValue>(entry->value);
    CHECK(value.thinking_level == "off");

    const auto settings = settings_object(fixture).get_object();
    const auto level = settings.find("defaultThinkingLevel");
    REQUIRE(level != settings.end());
    const auto* level_value = level->second.get_if<std::string>();
    REQUIRE(level_value != nullptr);
    CHECK(*level_value == "high");
    // The persisted model switch still wrote the model default.
    const auto provider = settings.find("defaultProvider");
    REQUIRE(provider != settings.end());
    CHECK(*provider->second.get_if<std::string>() == "beta");
}

TEST_CASE("set_model from a non-thinking model restores the settings default thinking level",
        "[coding_agent][set-model][issue406][spec]") {
    Fixture fixture;
    fixture.write_models(kReasoningAndPlainProviders);
    fixture.write_settings(R"({"defaultProvider": "beta", "defaultModel": "beta-1", "defaultThinkingLevel": "high"})");

    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
    REQUIRE(result.has_value());
    // The settings default resolves beta-1 (non-reasoning); the creation
    // clamp lands the level at "off".
    REQUIRE(result->resolved_identity.model == "beta-1");
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

    auto loaded = harness::session::SessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    const auto* entry = find_thinking_entry(*loaded);
    REQUIRE(entry != nullptr);
    const auto& value =
        std::get<harness::session::ThinkingLevelChangeValue>(entry->value);
    CHECK(value.thinking_level == "high");
}

TEST_CASE("set_model on an in-memory session skips the model_change entry and writes no settings default",
        "[coding_agent][set-model][issue406][spec]") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);

    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.workspace = fixture.workspace.path();
    request.session_target = coding_agent::InMemorySessionTarget{};
    request.execution_runtime_target = fixture.runtime.make_target();

    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request)));
    REQUIRE(result.has_value());
    REQUIRE(result->resolved_identity.model == "alpha-1");

    const auto target = result->session->model_runtime()->model("beta", "beta-1");
    REQUIRE(target.has_value());
    auto switched = result->session->set_model_blocking(*target);
    REQUIRE(switched.has_value());
    CHECK(result->session->snapshot().agent_state.model.id == "beta-1");
    result->session->close();

    // Session-only by default (pi ModelMutationOptions): no settings default
    // write for the in-memory session either.
    const auto settings_text = fixture.read_settings();
    CHECK(settings_text.empty());

    // No session file exists for the in-memory target.
    std::error_code exists_error;
    CHECK_FALSE(std::filesystem::exists(fixture.session_file, exists_error));
}

TEST_CASE("set_model resets the thinking level to the global default when one exists",
        "[coding_agent][set-model][issue774][spec]") {
    Fixture fixture;
    fixture.write_models(kTwoReasoningProviders);
    // pi `_getThinkingLevelForModelSwitch`: the merged settings default wins
    // over the live level whenever it exists — the live level is only the
    // fallback when no default was ever set.
    fixture.write_settings(R"({"defaultThinkingLevel": "low"})");

    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
    REQUIRE(result.has_value());
    REQUIRE(result->resolved_identity.model == "alpha-1");
    REQUIRE(result->session->snapshot().agent_state.thinking_level == "low");

    // The in-session level drifts from the default (session-only change).
    auto raised = result->session->set_thinking_level("high");
    REQUIRE(raised.has_value());
    REQUIRE(*raised == "high");

    const auto target = result->session->model_runtime()->model("beta", "beta-1");
    REQUIRE(target.has_value());
    auto switched = result->session->set_model_blocking(*target);
    REQUIRE(switched.has_value());

    // The settings default — not the drifted live level — clamps onto the
    // new reasoning model, and the effective change records the entry.
    CHECK(result->session->snapshot().agent_state.thinking_level == "low");
    result->session->close();

    auto loaded = harness::session::SessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    const auto* entry = find_thinking_entry(*loaded);
    REQUIRE(entry != nullptr);
    const auto& value = std::get<harness::session::ThinkingLevelChangeValue>(entry->value);
    CHECK(value.thinking_level == "low");
}

TEST_CASE("set_model keeps the live level and records no thinking entry when no default exists",
        "[coding_agent][set-model][issue774][spec]") {
    Fixture fixture;
    fixture.write_models(kTwoReasoningProviders);

    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
    REQUIRE(result.has_value());
    REQUIRE(result->resolved_identity.model == "alpha-1");
    REQUIRE(result->session->snapshot().agent_state.thinking_level == "medium");

    const auto target = result->session->model_runtime()->model("beta", "beta-1");
    REQUIRE(target.has_value());
    auto switched = result->session->set_model_blocking(*target);
    REQUIRE(switched.has_value());

    // No explicit default anywhere: the live level survives the switch, so
    // the effective level is unchanged and no `thinking_level_change` entry
    // lands after the `model_change` (the session-model-switch parity). The
    // creation-time initial entry stays the only thinking entry.
    CHECK(result->session->snapshot().agent_state.thinking_level == "medium");
    result->session->close();

    auto loaded = harness::session::SessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    const auto thinking_entries = std::count_if(
            loaded->entries.begin(), loaded->entries.end(), [](const harness::session::SessionEntry& entry) {
                return entry.kind == harness::session::SessionEntryKind::ThinkingLevelChange;
            });
    CHECK(thinking_entries == 1);
    const auto* model_entry = find_model_change_entry(*loaded);
    REQUIRE(model_entry != nullptr);
    const auto& model_value = std::get<harness::session::ModelChangeValue>(model_entry->value);
    CHECK(model_value.provider == "beta");
}
