// Runtime model cycling (pi `agent-session.ts` `cycleModel`, G3 decision 5):
// scoped models cycle within the auth-filtered scoped set (`_cycleScopedModel`),
// otherwise within the available models (`_cycleAvailableModel`); each cycle
// applies the model, persists the `model_change` entry, writes the global
// settings default, and re-clamps the thinking level. `cycleThinkingLevel`
// walks the active model's supported levels with a wrap. All sessions run
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

/// Three keyed providers (alpha, beta, gamma), each one model, all resolving
/// as configured.
constexpr std::string_view kThreeKeyedProviders = R"({
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
    },
    "gamma": {
      "baseUrl": "https://gamma.example/v1",
      "api": "openai-responses",
      "apiKey": "dummy-gamma-key",
      "models": [{"id": "gamma-1"}]
    }
  }
})";

/// `alpha` (keyed), `beta` (keyless), `gamma` (keyed).
constexpr std::string_view kKeyedAlphaKeylessBetaKeyedGamma = R"({
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
    },
    "gamma": {
      "baseUrl": "https://gamma.example/v1",
      "api": "openai-responses",
      "apiKey": "dummy-gamma-key",
      "models": [{"id": "gamma-1"}]
    }
  }
})";

/// A keyed reasoning provider with two models for thinking-cycle tests.
constexpr std::string_view kReasoningProvider = R"({
  "providers": {
    "alpha": {
      "baseUrl": "https://alpha.example/v1",
      "api": "openai-responses",
      "apiKey": "dummy-alpha-key",
      "models": [{"id": "alpha-1", "reasoning": true}, {"id": "alpha-2", "reasoning": true}]
    }
  }
})";

/// A keyed reasoning provider with a single model (for the no-cycle case).
constexpr std::string_view kSingleReasoningProvider = R"({
  "providers": {
    "alpha": {
      "baseUrl": "https://alpha.example/v1",
      "api": "openai-responses",
      "apiKey": "dummy-alpha-key",
      "models": [{"id": "alpha-1", "reasoning": true}]
    }
  }
})";

/// A keyed non-reasoning provider.
constexpr std::string_view kNonReasoningProvider = R"({
  "providers": {
    "alpha": {
      "baseUrl": "https://alpha.example/v1",
      "api": "openai-responses",
      "apiKey": "dummy-alpha-key",
      "models": [{"id": "alpha-1", "reasoning": false}]
    }
  }
})";

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

} // namespace

TEST_CASE(
    "cycle_model cycles forward and backward through the available models",
    "[coding_agent][model-cycle][issue407]") {
    Fixture fixture;
    fixture.write_models(kThreeKeyedProviders);

    auto result = coding_agent::create_agent_session(cli_request(fixture));
    REQUIRE(result.has_value());
    REQUIRE(result->model == "alpha-1");
    REQUIRE(result->session->scoped_models().empty());

    // Forward: alpha-1 → beta-1.
    auto forward = result->session->cycle_model_blocking("forward");
    REQUIRE(forward.has_value());
    REQUIRE(forward->has_value());
    CHECK(forward->value().model.provider == "beta");
    CHECK(forward->value().model.id == "beta-1");
    CHECK_FALSE(forward->value().is_scoped);
    CHECK(result->session->snapshot().agent_state.model.id == "beta-1");

    // Forward wraps: beta-1 → gamma-1.
    auto forward_again = result->session->cycle_model_blocking("forward");
    REQUIRE(forward_again.has_value());
    REQUIRE(forward_again->has_value());
    CHECK(forward_again->value().model.id == "gamma-1");

    // Backward: gamma-1 → beta-1.
    auto backward = result->session->cycle_model_blocking("backward");
    REQUIRE(backward.has_value());
    REQUIRE(backward->has_value());
    CHECK(backward->value().model.id == "beta-1");

    // The model_change entries persist each cycle (pi appendModelChange).
    result->session->close();
    auto loaded = harness::session::JsonlSessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    const auto* entry = find_model_change_entry(*loaded);
    REQUIRE(entry != nullptr);
    const auto& value = std::get<harness::session::ModelChangeValue>(entry->value);
    CHECK(value.provider == "beta");
    CHECK(value.model_id == "beta-1");

    // The global settings default follows the last cycle (pi
    // setDefaultModelAndProvider).
    const auto settings = util::read_json(fixture.read_settings());
    REQUIRE(settings.has_value());
    const auto& object = settings->get_object();
    const auto provider = object.find("defaultProvider");
    REQUIRE(provider != object.end());
    CHECK(*provider->second.get_if<std::string>() == "beta");
    const auto model = object.find("defaultModel");
    REQUIRE(model != object.end());
    CHECK(*model->second.get_if<std::string>() == "beta-1");
}

TEST_CASE(
    "cycle_model returns nullopt with a single available model",
    "[coding_agent][model-cycle][issue407]") {
    Fixture fixture;
    fixture.write_models(kSingleReasoningProvider);

    auto result = coding_agent::create_agent_session(cli_request(fixture));
    REQUIRE(result.has_value());

    const auto cycled = result->session->cycle_model_blocking("forward");
    REQUIRE(cycled.has_value());
    CHECK_FALSE(cycled->has_value());
    // The live model is unchanged.
    CHECK(result->session->snapshot().agent_state.model.id == "alpha-1");
    result->session->close();
}

TEST_CASE(
    "cycle_model cycles within the scoped set and drops unauthenticated scoped models",
    "[coding_agent][model-cycle][issue407]") {
    Fixture fixture;
    fixture.write_models(kKeyedAlphaKeylessBetaKeyedGamma);

    // --models scopes alpha* + beta*: the session carries the resolved scope
    // (pi `scopedModels` over the auth-filtered availability snapshot, so the
    // keyless beta model is never in scope).
    auto request = cli_request(fixture);
    request.models = {"alpha*", "beta*"};
    auto result = coding_agent::create_agent_session(std::move(request));
    REQUIRE(result.has_value());
    REQUIRE(result->model == "alpha-1");

    REQUIRE(result->session->scoped_models().size() == 1);
    CHECK(result->session->scoped_models()[0].model.id == "alpha-1");

    // A session-only scope that includes an unauthenticated model drops it at
    // cycle time (pi `_cycleScopedModel` auth filter) and refuses to cycle
    // when only one eligible model remains.
    result->session->set_scoped_models({
        coding_agent::ScopedModel{.model = *result->session->model_runtime()->model("alpha", "alpha-1")},
        coding_agent::ScopedModel{.model = *result->session->model_runtime()->model("beta", "beta-1")},
    });
    auto filtered = result->session->cycle_model_blocking("forward");
    REQUIRE(filtered.has_value());
    CHECK_FALSE(filtered->has_value());
    CHECK(result->session->snapshot().agent_state.model.id == "alpha-1");

    // Session-only scoped changes (pi setScopedModels): replacing the scope
    // with alpha + gamma enables cycling between them.
    result->session->set_scoped_models({
        coding_agent::ScopedModel{.model = *result->session->model_runtime()->model("alpha", "alpha-1")},
        coding_agent::ScopedModel{.model = *result->session->model_runtime()->model("gamma", "gamma-1")},
    });
    auto scoped_cycle = result->session->cycle_model_blocking("forward");
    REQUIRE(scoped_cycle.has_value());
    REQUIRE(scoped_cycle->has_value());
    CHECK(scoped_cycle->value().model.id == "gamma-1");
    CHECK(scoped_cycle->value().is_scoped);
    result->session->close();
}

TEST_CASE(
    "a scoped model's explicit thinking level overrides the session preference on cycle",
    "[coding_agent][model-cycle][issue407]") {
    Fixture fixture;
    fixture.write_models(kReasoningProvider);

    // --models "alpha-1:high" carries the explicit level into the scope.
    auto request = cli_request(fixture);
    request.models = {"alpha-2:high"};
    auto result = coding_agent::create_agent_session(std::move(request));
    REQUIRE(result.has_value());
    // The scoped initial model is alpha-2 with the explicit level.
    REQUIRE(result->session->scoped_models().size() == 1);
    REQUIRE(result->session->scoped_models()[0].thinking_level.has_value());
    CHECK(*result->session->scoped_models()[0].thinking_level == "high");
    CHECK(result->model == "alpha-2");

    // Seed a different current preference.
    auto raised = result->session->set_thinking_level("low");
    REQUIRE(raised.has_value());
    REQUIRE(*raised == "low");

    // Cycle from alpha-2 onto alpha-1, whose scope entry carries the explicit
    // level; the scoped explicit level must win over the session preference
    // (pi `_getThinkingLevelForModelSwitch`).
    result->session->set_scoped_models({
        coding_agent::ScopedModel{
            .model = *result->session->model_runtime()->model("alpha", "alpha-1"),
            .thinking_level = "high",
        },
        coding_agent::ScopedModel{
            .model = *result->session->model_runtime()->model("alpha", "alpha-2"),
        },
    });
    auto cycled = result->session->cycle_model_blocking("forward");
    REQUIRE(cycled.has_value());
    REQUIRE(cycled->has_value());
    CHECK(cycled->value().model.id == "alpha-1");
    CHECK(cycled->value().thinking_level == "high");
    CHECK(result->session->snapshot().agent_state.thinking_level == "high");
    result->session->close();
}

TEST_CASE(
    "cycle_thinking_level walks the model's supported levels and wraps",
    "[coding_agent][model-cycle][issue407]") {
    Fixture fixture;
    fixture.write_models(kReasoningProvider);

    auto result = coding_agent::create_agent_session(cli_request(fixture));
    REQUIRE(result.has_value());
    // Creation clamps the default "medium" against the reasoning model.
    REQUIRE(result->session->snapshot().agent_state.thinking_level == "medium");

    // A reasoning model without a thinkingLevelMap supports
    // off/minimal/low/medium/high (pi-ai `getSupportedThinkingLevels`).
    auto high = result->session->cycle_thinking_level();
    REQUIRE(high.has_value());
    REQUIRE(high->has_value());
    CHECK(**high == "high");

    // Wraps after the last supported level (pi
    // `(currentIndex + 1) % levels.length`).
    auto wrapped = result->session->cycle_thinking_level();
    REQUIRE(wrapped.has_value());
    REQUIRE(wrapped->has_value());
    CHECK(**wrapped == "off");

    auto minimal = result->session->cycle_thinking_level();
    REQUIRE(minimal.has_value());
    REQUIRE(minimal->has_value());
    CHECK(**minimal == "minimal");
    result->session->close();
}

TEST_CASE(
    "cycle_thinking_level returns nullopt when the model supports no thinking",
    "[coding_agent][model-cycle][issue407]") {
    Fixture fixture;
    fixture.write_models(kNonReasoningProvider);

    auto result = coding_agent::create_agent_session(cli_request(fixture));
    REQUIRE(result.has_value());
    CHECK(result->session->snapshot().agent_state.thinking_level == "off");

    const auto cycled = result->session->cycle_thinking_level();
    REQUIRE(cycled.has_value());
    CHECK_FALSE(cycled->has_value());
    result->session->close();
}

TEST_CASE(
    "settings enabledModels seed the session scope and a scoped :level seeds the initial thinking level",
    "[coding_agent][model-cycle][issue407]") {
    Fixture fixture;
    fixture.write_models(kReasoningProvider);
    // pi main.ts: `parsed.models ?? settingsManager.getEnabledModels()` — the
    // settings scope becomes the session's Ctrl+P cycling set.
    fixture.write_settings(R"({"enabledModels": ["alpha-2:high"]})");

    auto result = coding_agent::create_agent_session(cli_request(fixture));
    REQUIRE(result.has_value());
    // The scoped initial model carries the explicit :level as the initial
    // thinking level (pi main.ts `scopedModels[0].thinkingLevel`).
    REQUIRE(result->model == "alpha-2");
    REQUIRE(result->session->scoped_models().size() == 1);
    CHECK(result->session->scoped_models()[0].model.id == "alpha-2");
    CHECK(result->session->snapshot().agent_state.thinking_level == "high");

    // Cycling stays within the settings scope.
    auto cycled = result->session->cycle_model_blocking("forward");
    REQUIRE(cycled.has_value());
    CHECK_FALSE(cycled->has_value());
    CHECK(result->session->snapshot().agent_state.model.id == "alpha-2");
    result->session->close();
}
