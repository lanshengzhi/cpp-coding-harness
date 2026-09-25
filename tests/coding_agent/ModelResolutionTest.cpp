// pi-agent-core T04 evidence (#353): the model-resolution chain
// (CLI → scoped models → resumed session → settings default → first available
// model with configured auth → kDefaultModel) and thinking-level persistence
// (a `thinking_level_change` session entry plus the settings default, so
// resume restores the level exactly like pi). All session creation runs
// against a default-created ModelRuntime over a temp Agent Config Directory
// with dummy-only models.json/settings.json values — no live credentials, no
// network validation.
//
// The committed fixture `fixtures/pi-agent-core/thinking-persistence.json`
// pins the `thinking_level_change` entry shape and the settings default write
// (the #330 fixture strategy); every other assertion is a direct session-level
// check of one precedence level.

#include <cch/agent/Agent.hpp>
#include "coding_agent/AgentSession.hpp"
#include <cch/coding_agent/Settings.hpp>
#include <cch/agent/harness/session/SessionStore.hpp>
#include <cch/support/Error.hpp>
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ModelFixture.hpp"
#include "support/ModelsFixture.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/RuntimeLoopDriver.hpp"
#include "support/TempWorkspace.hpp"
#include "support/Json.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <format>
#include "support/AgentRootFixture.hpp"

using namespace cch;

namespace {

/// One isolated assembly fixture: a temp workspace for the session file and a
/// temp Agent Config Directory (`HOME`) whose models.json /
/// settings.json drive the resolution chain deterministically. Ambient
/// KIMI_API_KEY is unset so the built-in kimi-coding provider never resolves
/// as configured unless the test says so.
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

/// `alpha` is keyless (never resolves as configured); `beta` carries a key.
constexpr std::string_view kKeylessAlphaKeyedBeta = R"({
  "providers": {
    "alpha": {
      "baseUrl": "https://alpha.example/v1",
      "api": "openai-responses",
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

/// `beta` is keyless (never resolves as configured); `alpha` carries a key.
constexpr std::string_view kKeylessBetaKeyedAlpha = R"({
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

/// One keyed reasoning provider for thinking-persistence scenarios.
constexpr std::string_view kKeyedReasoningProvider = R"({
  "providers": {
    "alpha": {
      "baseUrl": "https://api.deepseek.example/v1",
      "api": "openai-responses",
      "apiKey": "dummy-deepseek-key",
      "models": [{"id": "deepseek-v4-flash", "reasoning": true}]
    }
  }
})";

/// One keyed reasoning provider whose map supports every pi thinking level.
constexpr std::string_view kFullThinkingProvider = R"({
  "providers": {
    "alpha": {
      "baseUrl": "https://alpha.example/v1",
      "api": "openai-responses",
      "apiKey": "dummy-alpha-key",
      "models": [{
        "id": "k3-256k",
        "reasoning": true,
        "thinkingLevelMap": {
          "off": "off",
          "minimal": "minimal",
          "low": "low",
          "medium": "medium",
          "high": "high",
          "xhigh": "xhigh",
          "max": "max"
        }
      }]
    }
  }
})";

/// One keyed non-reasoning provider.
[[nodiscard]] coding_agent::runtime::AgentSessionCreationRequest cli_request(
    const Fixture& fixture) {
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_target = coding_agent::ExplicitOpenOrCreateSessionTarget{fixture.session_file};
    request.workspace = fixture.workspace.path();
    request.execution_runtime_target = fixture.runtime.make_target();
    return request;
}

[[nodiscard]] coding_agent::runtime::AgentSessionCreationRequest cli_resume_request(
    const Fixture& fixture) {
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_target = coding_agent::ExplicitResumeSessionTarget{fixture.session_file};
    request.workspace = fixture.workspace.path();
    request.execution_runtime_target = fixture.runtime.make_target();
    return request;
}

/// The canonical `thinking_level_change` entry-shape projection: the fields
/// this ticket owns (type, thinkingLevel). Generic tree metadata (id,
/// timestamp, parentId null-vs-omitted) is pinned by the T07 session-wire
/// contract, not here.
[[nodiscard]] support::JsonValue entry_shape_to_json(
    const harness::session::SessionEntry& entry) {
    support::JsonValue object{support::JsonValue::object_t{}};
    auto& o = object.get_object();
    const auto parsed = support::read_json(entry.raw_line);
    REQUIRE(parsed.has_value());
    const auto& parsed_object = parsed->get_object();
    o.emplace("type", parsed_object.at("type"));
    const auto* thinking = std::get_if<harness::session::ThinkingLevelChangeValue>(&entry.value);
    REQUIRE(thinking != nullptr);
    o.emplace("thinkingLevel", support::JsonValue(thinking->thinking_level));
    return object;
}

/// The last `thinking_level_change` on the active path (the leaf-path entry
/// resume restores; the new-session initial entry precedes any real change).
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

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Model resolution chain — CLI profile
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("CLI model resolution: --model wins over settings defaults",
        "[coding_agent][model-resolution][issue353])[spec]") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);
    fixture.write_settings(R"({"defaultProvider":"alpha","defaultModel":"alpha-1"})");

    auto request = cli_request(fixture);
    request.session_facts.provider = "beta";
    request.session_facts.model = "beta-1";

    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request)));
    REQUIRE(result.has_value());
    CHECK(result->resolved_identity.provider == "beta");
    CHECK(result->resolved_identity.model == "beta-1");
    CHECK(result->session->provider() == "beta");
    CHECK(result->session->model() == "beta-1");
    result->session->close();
}

TEST_CASE("CLI model resolution: provider-qualified thinking suffixes select every supported level",
        "[coding_agent][model-resolution][issue798][spec]") {
    constexpr std::string_view levels[] = {"off", "minimal", "low", "medium", "high", "xhigh", "max"};

    for (const auto level : levels) {
        Fixture fixture;
        fixture.write_models(kFullThinkingProvider);
        auto request = cli_request(fixture);
        request.session_facts.model = "alpha/k3-256k:" + std::string{level};

        auto result = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request)));
        REQUIRE(result.has_value());
        CHECK(result->resolved_identity.provider == "alpha");
        CHECK(result->resolved_identity.model == "k3-256k");
        CHECK(result->session->snapshot().agent_state.thinking_level == level);
        result->session->close();
    }
}

TEST_CASE("CLI model resolution: explicit thinking overrides a model suffix",
        "[coding_agent][model-resolution][issue798][spec]") {
    Fixture fixture;
    fixture.write_models(kFullThinkingProvider);
    auto request = cli_request(fixture);
    request.session_facts.model = "alpha/k3-256k:low";
    request.session_facts.thinking = "high";

    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request)));
    REQUIRE(result.has_value());
    CHECK(result->resolved_identity.provider == "alpha");
    CHECK(result->resolved_identity.model == "k3-256k");
    CHECK(result->session->snapshot().agent_state.thinking_level == "high");
    result->session->close();
}

TEST_CASE("CLI model resolution: an invalid thinking suffix has a bounded diagnostic",
        "[coding_agent][model-resolution][issue798][spec]") {
    Fixture fixture;
    fixture.write_models(kFullThinkingProvider);
    auto request = cli_request(fixture);
    request.session_facts.model = "alpha/k3-256k:turbo";

    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request)));
    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("Invalid thinking level \"turbo\"") != std::string::npos);
    CHECK(result.error().message.find("Unknown model") == std::string::npos);
}

TEST_CASE("CLI model resolution: invalid provider and model selections keep bounded diagnostics",
        "[coding_agent][model-resolution][issue798][spec]") {
    struct InvalidSelection {
        std::optional<std::string> provider;
        std::string model;
        std::string_view expected;
    };
    const InvalidSelection selections[] = {
            {.provider = "missing", .model = "model", .expected = "Unknown provider \"missing\""},
            {.provider = std::nullopt, .model = "missing/model", .expected = "Unknown model \"missing/model\""},
    };

    for (const auto& selection : selections) {
        Fixture fixture;
        fixture.write_models(kFullThinkingProvider);
        auto request = cli_request(fixture);
        request.session_facts.provider = selection.provider;
        request.session_facts.model = selection.model;

        auto result = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request)));
        REQUIRE_FALSE(result);
        CHECK(result.error().message.find(selection.expected) != std::string::npos);
    }
}

TEST_CASE("CLI model resolution: settings default wins with configured auth",
        "[coding_agent][model-resolution][issue353])[spec]") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);
    fixture.write_settings(R"({"defaultProvider":"beta","defaultModel":"beta-1"})");

    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
    REQUIRE(result.has_value());
    CHECK(result->resolved_identity.provider == "beta");
    CHECK(result->resolved_identity.model == "beta-1");
    result->session->close();
}

TEST_CASE("CLI model resolution: unauthenticated settings default falls through to first available with auth",
        "[coding_agent][model-resolution][issue353])[spec]") {
    Fixture fixture;
    fixture.write_models(kKeylessAlphaKeyedBeta);
    // The saved default (alpha-1) has no configured auth; pi findInitialModel
    // skips it and the chain resolves the first available model whose provider
    // has configured auth (beta-1), never the keyless alpha-1.
    fixture.write_settings(R"({"defaultProvider":"alpha","defaultModel":"alpha-1"})");

    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
    REQUIRE(result.has_value());
    CHECK(result->resolved_identity.provider == "beta");
    CHECK(result->resolved_identity.model == "beta-1");
    result->session->close();
}

TEST_CASE("CLI model resolution: scoped models select the first scoped model for new sessions",
        "[coding_agent][model-resolution][issue353])[spec]") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);

    auto request = cli_request(fixture);
    request.session_facts.models = {"alpha*"};

    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request)));
    REQUIRE(result.has_value());
    CHECK(result->resolved_identity.provider == "alpha");
    CHECK(result->resolved_identity.model == "alpha-1");
    result->session->close();
}

TEST_CASE("CLI model resolution: the saved default in scope wins over the first scoped model",
        "[coding_agent][model-resolution][issue353])[spec]") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);
    fixture.write_settings(R"({"defaultProvider":"beta","defaultModel":"beta-1"})");

    auto request = cli_request(fixture);
    request.session_facts.models = {"alpha*", "beta*"};

    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request)));
    REQUIRE(result.has_value());
    // beta-1 is in scope and is the saved default, so it wins over the first
    // scoped model (alpha-1).
    CHECK(result->resolved_identity.provider == "beta");
    CHECK(result->resolved_identity.model == "beta-1");
    result->session->close();
}

TEST_CASE("CLI model resolution: resume re-resolves the stored model identity",
        "[coding_agent][model-resolution][resume][issue353])[spec]") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);

    {
        auto request = cli_request(fixture);
        request.session_facts.provider = "beta";
        request.session_facts.model = "beta-1";
        auto created = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request)));
        REQUIRE(created.has_value());
        created->session->close();
    }

    // Resume with no CLI model flags: the stored `model_change {beta, beta-1}`
    // re-resolves against the live runtime catalog.
    auto resumed = fixture.runtime.run(coding_agent::create_agent_session_async(cli_resume_request(fixture)));
    REQUIRE(resumed.has_value());
    CHECK(resumed->resolved_identity.provider == "beta");
    CHECK(resumed->resolved_identity.model == "beta-1");
    CHECK_FALSE(resumed->model_fallback_message.has_value());
    resumed->session->close();
}

TEST_CASE("CLI model resolution: equivalent explicit resume metadata emits no override diagnostic",
        "[coding_agent][model-resolution][resume][issue799][spec]") {
    Fixture fixture;
    fixture.write_models(kFullThinkingProvider);

    {
        auto request = cli_request(fixture);
        auto created = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request)));
        REQUIRE(created.has_value());
        REQUIRE(created->resolved_identity.provider == "alpha");
        REQUIRE(created->resolved_identity.model == "k3-256k");
        created->session->close();
    }

    auto request = cli_resume_request(fixture);
    request.session_facts.provider = "ALPHA";
    request.session_facts.model = "K3-256k:high";
    auto resumed = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request)));

    REQUIRE(resumed.has_value());
    CHECK(std::none_of(resumed->diagnostics.begin(), resumed->diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "resume_provider_override";
    }));
    CHECK(resumed->resolved_identity.provider == "alpha");
    CHECK(resumed->resolved_identity.model == "k3-256k");
    CHECK(resumed->session->snapshot().agent_state.model.provider == "alpha");
    CHECK(resumed->session->snapshot().agent_state.model.id == "k3-256k");
    CHECK(resumed->session->snapshot().agent_state.thinking_level == "high");
    resumed->session->close();

    auto loaded = harness::session::SessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    const harness::session::SessionEntry* model_change = nullptr;
    for (const auto& entry : loaded->entries) {
        if (entry.kind == harness::session::SessionEntryKind::ModelChange) {
            model_change = &entry;
        }
    }
    REQUIRE(model_change != nullptr);
    const auto& model = std::get<harness::session::ModelChangeValue>(model_change->value);
    CHECK(model.provider == "alpha");
    CHECK(model.model_id == "k3-256k");
}

TEST_CASE("CLI model resolution: an actual resume model change emits one accurate diagnostic",
        "[coding_agent][model-resolution][resume][issue799][spec]") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);

    {
        auto request = cli_request(fixture);
        request.session_facts.provider = "alpha";
        request.session_facts.model = "alpha-1";
        auto created = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request)));
        REQUIRE(created.has_value());
        created->session->close();
    }

    auto request = cli_resume_request(fixture);
    request.session_facts.provider = "beta";
    request.session_facts.model = "beta-1";
    auto resumed = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request)));

    REQUIRE(resumed.has_value());
    const auto override_count = std::count_if(resumed->diagnostics.begin(),
            resumed->diagnostics.end(),
            [](const auto& diagnostic) { return diagnostic.code == "resume_provider_override"; });
    CHECK(override_count == 1);
    const auto diagnostic = std::find_if(resumed->diagnostics.begin(),
            resumed->diagnostics.end(),
            [](const auto& value) { return value.code == "resume_provider_override"; });
    REQUIRE(diagnostic != resumed->diagnostics.end());
    CHECK(diagnostic->message == "Resumed session provider/model metadata overridden by explicit request; "
                                 "was (alpha/alpha-1) now (beta/beta-1)");
    CHECK(resumed->resolved_identity.provider == "beta");
    CHECK(resumed->resolved_identity.model == "beta-1");
    CHECK(resumed->session->snapshot().agent_state.model.provider == "beta");
    CHECK(resumed->session->snapshot().agent_state.model.id == "beta-1");
    resumed->session->close();

    auto loaded = harness::session::SessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    const harness::session::SessionEntry* model_change = nullptr;
    for (const auto& entry : loaded->entries) {
        if (entry.kind == harness::session::SessionEntryKind::ModelChange) {
            model_change = &entry;
        }
    }
    REQUIRE(model_change != nullptr);
    const auto& model = std::get<harness::session::ModelChangeValue>(model_change->value);
    CHECK(model.provider == "beta");
    CHECK(model.model_id == "beta-1");

    auto re_resumed = fixture.runtime.run(coding_agent::create_agent_session_async(cli_resume_request(fixture)));
    REQUIRE(re_resumed.has_value());
    CHECK(re_resumed->resolved_identity.provider == "beta");
    CHECK(re_resumed->resolved_identity.model == "beta-1");
    CHECK(std::none_of(re_resumed->diagnostics.begin(), re_resumed->diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "resume_provider_override";
    }));
    re_resumed->session->close();
}

TEST_CASE("CLI model resolution: resume without legacy model metadata emits no override diagnostic",
        "[coding_agent][model-resolution][resume][issue799][spec]") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);
    auto store = harness::session::SessionStore::create_new(fixture.session_file,
            harness::session::SessionMetadata{
                    .session_id = "legacy-session",
                    .created_at = "2026-09-25T00:00:00Z",
                    .workspace = fixture.workspace.path(),
                    .provider = "legacy",
                    .model = "legacy-model",
            });
    REQUIRE(store.has_value());
    REQUIRE(store->append(ai::user_text_message("hello")).has_value());

    auto request = cli_resume_request(fixture);
    request.session_facts.provider = "beta";
    request.session_facts.model = "beta-1";
    auto resumed = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request)));

    REQUIRE(resumed.has_value());
    CHECK(std::none_of(resumed->diagnostics.begin(), resumed->diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "resume_provider_override";
    }));
    CHECK(resumed->resolved_identity.provider == "beta");
    CHECK(resumed->resolved_identity.model == "beta-1");
    CHECK(resumed->session->snapshot().agent_state.model.provider == "beta");
    CHECK(resumed->session->snapshot().agent_state.model.id == "beta-1");
    CHECK(resumed->session->session_stats().user_messages == 1);
    resumed->session->close();
}

TEST_CASE("CLI model resolution: resume without configured auth falls back with pi's message",
        "[coding_agent][model-resolution][resume][issue357])[spec]") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);

    {
        auto request = cli_request(fixture);
        request.session_facts.provider = "beta";
        request.session_facts.model = "beta-1";
        auto created = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request)));
        REQUIRE(created.has_value());
        created->session->close();
    }

    // The stored identity's provider loses its auth between create and resume:
    // pi sdk.ts `createAgentSession` requires `restoredModel &&
    // hasConfiguredAuth`, so the chain falls back to the first available model
    // with configured auth. The `modelFallbackMessage` carries pi's exact
    // text (no reason parenthetical at this baseline — `restoreModelFromSession`
    // is uncalled) plus the resolved fallback identity.
    fixture.write_models(kKeylessBetaKeyedAlpha);

    auto resumed = fixture.runtime.run(coding_agent::create_agent_session_async(cli_resume_request(fixture)));
    REQUIRE(resumed.has_value());
    CHECK(resumed->resolved_identity.provider == "alpha");
    CHECK(resumed->resolved_identity.model == "alpha-1");
    REQUIRE(resumed->model_fallback_message.has_value());
    CHECK(*resumed->model_fallback_message ==
          "Could not restore model beta/beta-1. Using alpha/alpha-1");
    resumed->session->close();
}

TEST_CASE("resume restore failure with nothing available reports the no-models message",
        "[coding_agent][model-resolution][resume][issue404])[spec]") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);

    {
        auto request = cli_request(fixture);
        request.session_facts.provider = "beta";
        request.session_facts.model = "beta-1";
        auto created = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request)));
        REQUIRE(created.has_value());
        created->session->close();
    }

    // The catalog disappears entirely: the restore fails and the chain lands
    // on the unknown placeholder, so pi replaces the fallback message with
    // `formatNoModelsAvailableMessage()` (sdk.ts `if (!model)` branch).
    std::filesystem::remove(fixture.agent_dir / "models.json");

    auto resumed = fixture.runtime.run(coding_agent::create_agent_session_async(cli_resume_request(fixture)));
    REQUIRE(resumed.has_value());
    CHECK(resumed->resolved_identity.provider == "unknown");
    CHECK(resumed->resolved_identity.model == "unknown");
    REQUIRE(resumed->model_fallback_message.has_value());
    CHECK(resumed->model_fallback_message->starts_with(
        "No models available. Use /login to log into a provider via OAuth or API key. See:"));
    resumed->session->close();
}

TEST_CASE("CLI model resolution: resume with a missing model falls back with pi's message",
        "[coding_agent][model-resolution][resume][issue357])[spec]") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);

    {
        auto request = cli_request(fixture);
        request.session_facts.provider = "beta";
        request.session_facts.model = "beta-1";
        auto created = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request)));
        REQUIRE(created.has_value());
        created->session->close();
    }

    // The stored model disappears from the catalog between create and resume:
    // pi sdk.ts reports the failure through `modelFallbackMessage` (the reason
    // stays internal — it is not part of the baseline message), the chain
    // continues through the runtime default, and the message names the
    // resolved fallback identity.
    fixture.write_models(R"({
      "providers": {
        "gamma": {
          "baseUrl": "https://gamma.example/v1",
          "api": "openai-responses",
          "apiKey": "dummy-gamma-key",
          "models": [{"id": "gamma-1"}]
        }
      }
    })");

    auto resumed = fixture.runtime.run(coding_agent::create_agent_session_async(cli_resume_request(fixture)));
    REQUIRE(resumed.has_value());
    CHECK(resumed->resolved_identity.provider == "gamma");
    CHECK(resumed->resolved_identity.model == "gamma-1");
    REQUIRE(resumed->model_fallback_message.has_value());
    CHECK(*resumed->model_fallback_message ==
          "Could not restore model beta/beta-1. Using gamma/gamma-1");
    resumed->session->close();
}

TEST_CASE("session files persist only model_change provider/modelId, never auth material",
        "[coding_agent][model-resolution][resume][issue357])[spec]") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);

    {
        auto request = cli_request(fixture);
        request.session_facts.provider = "beta";
        request.session_facts.model = "beta-1";
        auto created = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request)));
        REQUIRE(created.has_value());
        created->session->close();
    }

    auto loaded = harness::session::SessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    const harness::session::SessionEntry* model_change = nullptr;
    for (const auto& entry : loaded->entries) {
        if (entry.kind == harness::session::SessionEntryKind::ModelChange) {
            model_change = &entry;
            break;
        }
    }
    REQUIRE(model_change != nullptr);
    const auto& value =
        std::get<harness::session::ModelChangeValue>(model_change->value);
    CHECK(value.provider == "beta");
    CHECK(value.model_id == "beta-1");
    // The persisted line carries exactly the pi `{provider, modelId}` identity:
    // no baseUrl, key-source, environment template, or any authentication
    // material ever reaches the session file (#327 / ADR 0031).
    const auto& line = model_change->raw_line;
    CHECK(line.find(R"("type":"model_change")") != std::string::npos);
    CHECK(line.find(R"("provider":"beta")") != std::string::npos);
    CHECK(line.find(R"("modelId":"beta-1")") != std::string::npos);
    CHECK(line.find("apiKey") == std::string::npos);
    CHECK(line.find("baseUrl") == std::string::npos);
    CHECK(line.find("dummy") == std::string::npos);
    CHECK(line.find("token") == std::string::npos);
}

TEST_CASE("CLI model resolution: nothing configured keeps kDefaultModel and fails through provider lookup",
        "[coding_agent][model-resolution][issue353])[spec]") {
    Fixture fixture;
    // Empty Agent Config Directory: no providers, no models, no auth. The
    // persistent store needs a Runtime channel so the prompt below can admit
    // its session entries.
    auto request = cli_request(fixture);
    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request)));
    REQUIRE(result.has_value());
    tests::RuntimeLoopDriver runtime_driver(fixture.runtime);
    // The concrete unknown kDefaultModel is the resolved identity, with no
    // construction-time default silently winning.
    CHECK(result->resolved_identity.provider == "unknown");
    CHECK(result->resolved_identity.model == "unknown");
    const auto state = result->session->snapshot().agent_state;
    CHECK(state.model.id == "unknown");
    CHECK(state.model.provider == "unknown");
    // pi sdk.ts `if (!model)`: nothing available replaces the fallback
    // message with formatNoModelsAvailableMessage(), shown as an interactive
    // boot warning.
    REQUIRE(result->model_fallback_message.has_value());
    CHECK(result->model_fallback_message->starts_with(
        "No models available. Use /login to log into a provider via OAuth or API key. See:"));

    // Streaming against it fails through the normal provider/auth lookup
    // exactly like pi's `Unknown provider: ${model.provider}`.
    auto prompted = result->session->prompt_blocking("hello");
    REQUIRE(prompted.has_value());
    const auto& messages = result->session->snapshot().agent_state.messages;
    REQUIRE(messages.size() == 3);
    const auto* system = std::get_if<ai::SystemMessage>(&messages.front());
    REQUIRE(system != nullptr);
    CHECK(system->content.empty());
    const auto* terminal = std::get_if<ai::AssistantMessage>(&messages.back());
    REQUIRE(terminal != nullptr);
    CHECK(terminal->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(terminal->error_message);
    CHECK(terminal->error_message->find("Unknown provider: unknown") != std::string::npos);
    result->session->close();
}

TEST_CASE("a zero-model session never persists a placeholder model_change identity",
        "[coding_agent][model-resolution][issue404])[spec]") {
    Fixture fixture;
    // Empty Agent Config Directory: nothing available, so the resolution
    // lands on the unknown placeholder. pi sdk.ts guards the model_change
    // append with `if (model)`; the initial thinking entry still persists
    // (pi appends it unconditionally, clamped to "off" for the placeholder).
    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
    REQUIRE(result.has_value());
    result->session->close();

    auto loaded = harness::session::SessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    bool persisted_model_change = false;
    for (const auto& entry : loaded->entries) {
        if (entry.kind == harness::session::SessionEntryKind::ModelChange) {
            persisted_model_change = true;
        }
    }
    CHECK_FALSE(persisted_model_change);
    const auto* thinking = find_thinking_entry(*loaded);
    REQUIRE(thinking != nullptr);
    const auto& value =
        std::get<harness::session::ThinkingLevelChangeValue>(thinking->value);
    CHECK(value.thinking_level == "off");
}

// ─────────────────────────────────────────────────────────────────────────────
// Model resolution chain — default CLI creation profile
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("default creation resolves the first available model with configured auth",
        "[coding_agent][model-resolution][issue353])[spec]") {
    Fixture fixture;
    fixture.write_models(kKeylessAlphaKeyedBeta);

    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
    REQUIRE(result.has_value());
    CHECK(result->resolved_identity.provider == "beta");
    CHECK(result->resolved_identity.model == "beta-1");
    result->session->close();
}

TEST_CASE("default creation honors the settings default only with configured auth",
        "[coding_agent][model-resolution][issue353])[spec]") {
    Fixture fixture;
    fixture.write_models(kKeylessAlphaKeyedBeta);
    // defaultModel alpha-1 (keyless) is skipped; defaultModel beta-1 wins.
    fixture.write_settings(R"({"defaultProvider":"alpha","defaultModel":"alpha-1"})");

    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
    REQUIRE(result.has_value());
    CHECK(result->resolved_identity.provider == "beta");
    CHECK(result->resolved_identity.model == "beta-1");
    result->session->close();
}

TEST_CASE("default creation resume re-resolves the stored model with configured auth",
        "[coding_agent][model-resolution][resume][issue353])[spec]") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);

    {
        auto created = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
        REQUIRE(created.has_value());
        CHECK(created->resolved_identity.provider == "alpha");
        created->session->close();
    }

    auto resumed = fixture.runtime.run(coding_agent::create_agent_session_async(cli_resume_request(fixture)));
    REQUIRE(resumed.has_value());
    CHECK(resumed->resolved_identity.provider == "alpha");
    CHECK(resumed->resolved_identity.model == "alpha-1");
    CHECK_FALSE(resumed->model_fallback_message.has_value());
    resumed->session->close();
}

TEST_CASE("default creation resume re-resolves a non-default stored model identity",
        "[coding_agent][model-resolution][resume][issue357])[spec]") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);

    {
        // Explicitly request beta-1 so the stored model_change is not the
        // runtime default (alpha-1): resume must re-resolve the recorded
        // identity, not fall through to the first available model.
        auto request = cli_request(fixture);
        request.session_facts.provider = "beta";
        request.session_facts.model = "beta-1";
        auto created = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request)));
        REQUIRE(created.has_value());
        CHECK(created->resolved_identity.provider == "beta");
        CHECK(created->resolved_identity.model == "beta-1");
        created->session->close();
    }

    auto resumed = fixture.runtime.run(coding_agent::create_agent_session_async(cli_resume_request(fixture)));
    REQUIRE(resumed.has_value());
    CHECK(resumed->resolved_identity.provider == "beta");
    CHECK(resumed->resolved_identity.model == "beta-1");
    CHECK_FALSE(resumed->model_fallback_message.has_value());
    resumed->session->close();
}

// ─────────────────────────────────────────────────────────────────────────────
// Thinking-level persistence
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("set_thinking_level persists a thinking_level_change entry session-only by default",
        "[coding_agent][thinking-persistence][issue353][issue774][spec]") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);

    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
    REQUIRE(result.has_value());
    // The resolution chain landed the first available model with configured
    // auth; the session's model supports reasoning so a level change is real.
    CHECK(result->resolved_identity.model == "deepseek-v4-flash");

    auto changed = result->session->set_thinking_level("high");
    REQUIRE(changed.has_value());
    CHECK(*changed == "high");
    CHECK(result->session->snapshot().agent_state.thinking_level == "high");
    result->session->close();

    // The durable session file carries the `thinking_level_change` entry.
    auto loaded = harness::session::SessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    const auto* entry = find_thinking_entry(*loaded);
    REQUIRE(entry != nullptr);

    // Session-only by default (pi ModelMutationOptions): no settings default
    // write.
    CHECK(fixture.read_settings().empty());

    // pi setThinkingLevel: an explicit persist records the requested level in
    // the global settings default, and a reloaded manager sees it merged.
    auto result_two = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
    REQUIRE(result_two.has_value());
    auto persisted =
            result_two->session->set_thinking_level("high", coding_agent::ModelMutationOptions{.persist = true});
    REQUIRE(persisted.has_value());
    result_two->session->close();

    const auto settings_text = fixture.read_settings();
    const auto settings = support::read_json(settings_text);
    REQUIRE(settings.has_value());
    const auto& settings_object = settings->get_object();
    const auto found = settings_object.find("defaultThinkingLevel");
    REQUIRE(found != settings_object.end());
    CHECK(found->second.get_if<std::string>() != nullptr);
    CHECK(*found->second.get_if<std::string>() == "high");

    auto reloaded = coding_agent::SettingsManager::create(
            fixture.workspace.path(), fixture.agent_dir, /* project_trusted */ false);
    CHECK(reloaded.settings().default_thinking_level == "high");
}

TEST_CASE("resume restores the persisted thinking level from the session entry",
        "[coding_agent][thinking-persistence][resume][issue353])[spec]") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);

    {
        auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
        REQUIRE(result.has_value());
        auto changed = result->session->set_thinking_level("high");
        REQUIRE(changed.has_value());
        CHECK(*changed == "high");
        result->session->close();
    }

    // Resume: the nearest `thinking_level_change` on the active path wins over
    // the settings default and DEFAULT_THINKING_LEVEL (pi sdk.ts).
    auto resumed = fixture.runtime.run(coding_agent::create_agent_session_async(cli_resume_request(fixture)));
    REQUIRE(resumed.has_value());
    CHECK(resumed->session->snapshot().agent_state.thinking_level == "high");
    resumed->session->close();
}

TEST_CASE("resumed session without a thinking entry uses the settings default",
        "[coding_agent][thinking-persistence][resume][issue353])[spec]") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);

    {
        // Create without any level change: the new-session initial
        // `thinking_level_change` entry records the creation level. Strip it
        // so the resumed session genuinely has no thinking entry (the
        // hasThinkingEntry gate pi gates against).
        auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
        REQUIRE(result.has_value());
        result->session->close();

        std::ifstream in(fixture.session_file, std::ios::binary);
        std::ostringstream kept;
        std::string line;
        while (std::getline(in, line)) {
            if (line.find(R"("type":"thinking_level_change")") ==
                std::string::npos) {
                kept << line << '\n';
            }
        }
        std::ofstream out(fixture.session_file, std::ios::binary);
        out << kept.str();
    }

    fixture.write_settings(R"({"defaultThinkingLevel":"low"})");

    auto resumed = fixture.runtime.run(coding_agent::create_agent_session_async(cli_resume_request(fixture)));
    REQUIRE(resumed.has_value());
    CHECK(resumed->session->snapshot().agent_state.thinking_level == "low");
    resumed->session->close();
}

TEST_CASE("fresh session requests the settings default thinking level",
        "[coding_agent][thinking-persistence][issue353])[spec]") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);
    fixture.write_settings(R"({"defaultThinkingLevel":"high"})");

    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
    REQUIRE(result.has_value());
    // The Agent clamped the settings default against the reasoning model; the
    // supported set (no thinkingLevelMap) is off..high, so "high" survives.
    CHECK(result->session->snapshot().agent_state.thinking_level == "high");
    result->session->close();
}

TEST_CASE("set_thinking_level to off records the entry session-only and persists only on request",
        "[coding_agent][thinking-persistence][issue353][issue774][spec]") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);

    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
    REQUIRE(result.has_value());
    CHECK(result->session->snapshot().agent_state.thinking_level == "medium");

    // A real change to "off" appends the entry; session-only by default, so
    // no settings default lands.
    auto changed = result->session->set_thinking_level("off");
    REQUIRE(changed.has_value());
    CHECK(*changed == "off");
    CHECK(result->session->snapshot().agent_state.thinking_level == "off");
    CHECK(fixture.read_settings().empty());

    // The persist mutation writes the requested "off" (pi v0.87.1 dropped
    // the supportsThinking gate: an explicit persist records the request).
    auto persisted = result->session->set_thinking_level("off", coding_agent::ModelMutationOptions{.persist = true});
    REQUIRE(persisted.has_value());
    result->session->close();

    auto loaded = harness::session::SessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    REQUIRE(find_thinking_entry(*loaded) != nullptr);

    const auto settings = support::read_json(fixture.read_settings());
    REQUIRE(settings.has_value());
    const auto& settings_object = settings->get_object();
    const auto found = settings_object.find("defaultThinkingLevel");
    REQUIRE(found != settings_object.end());
    CHECK(*found->second.get_if<std::string>() == "off");
}

TEST_CASE("set_thinking_level clamps to the active model and rejects invalid levels",
        "[coding_agent][thinking-persistence][issue353])[spec]") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);

    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
    REQUIRE(result.has_value());

    // No thinkingLevelMap: supported is off..high, so "max" clamps to "high".
    auto clamped = result->session->set_thinking_level("max");
    REQUIRE(clamped.has_value());
    CHECK(*clamped == "high");
    CHECK(result->session->snapshot().agent_state.thinking_level == "high");

    // An invalid level is rejected without state or persistence changes.
    auto invalid = result->session->set_thinking_level("sometimes");
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error().code == support::ErrorCode::Validation);
    CHECK(result->session->snapshot().agent_state.thinking_level == "high");

    // A no-op change persists nothing: the initial creation entry plus the
    // one real change remain (pi appends the initial thinking level at
    // creation, so a new session starts with exactly one entry).
    auto unchanged = result->session->set_thinking_level("high");
    REQUIRE(unchanged.has_value());
    CHECK(*unchanged == "high");
    result->session->close();

    auto loaded = harness::session::SessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    const auto entries = std::count_if(
        loaded->entries.begin(),
        loaded->entries.end(),
        [](const harness::session::SessionEntry& entry) {
            return entry.kind == harness::session::SessionEntryKind::ThinkingLevelChange;
        });
    CHECK(entries == 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// New-session initial entries (P8 resume chain)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("new sessions append model_change then the initial thinking_level_change",
        "[coding_agent][model-resolution][thinking-persistence][issue404])[spec]") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);

    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
    REQUIRE(result.has_value());
    CHECK(result->resolved_identity.model == "deepseek-v4-flash");
    result->session->close();

    // pi sdk.ts: a new session appends `model_change {provider, modelId}` and
    // the initial (clamped) `thinking_level_change` so a later resume can
    // restore both. The reasoning model supports off..high, so the creation
    // default "medium" survives the clamp.
    auto loaded = harness::session::SessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    // The first content entries after the header are the model_change and the
    // initial thinking_level_change, in pi sdk.ts's order.
    std::vector<harness::session::SessionEntryKind> kinds;
    for (const auto& entry : loaded->entries) {
        if (entry.kind == harness::session::SessionEntryKind::Header) {
            continue;
        }
        kinds.push_back(entry.kind);
    }
    REQUIRE(kinds.size() >= 2);
    CHECK(kinds[0] == harness::session::SessionEntryKind::ModelChange);
    CHECK(kinds[1] == harness::session::SessionEntryKind::ThinkingLevelChange);
    const auto* thinking = find_thinking_entry(*loaded);
    REQUIRE(thinking != nullptr);
    const auto& value =
        std::get<harness::session::ThinkingLevelChangeValue>(thinking->value);
    CHECK(value.thinking_level == "medium");
}

TEST_CASE("the initial thinking entry carries the clamped creation level",
        "[coding_agent][model-resolution][thinking-persistence][issue404])[spec]") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);
    // A non-reasoning stored model would clamp the default to "off" (pi:
    // `if (!model) thinkingLevel = "off"` and capability clamping); here the
    // settings default "max" clamps to the reasoning model's "high" and the
    // persisted entry records the clamped value, not the request.
    fixture.write_settings(R"({"defaultThinkingLevel":"max"})");

    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
    REQUIRE(result.has_value());
    CHECK(result->session->snapshot().agent_state.thinking_level == "high");
    result->session->close();

    auto loaded = harness::session::SessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    const auto* thinking = find_thinking_entry(*loaded);
    REQUIRE(thinking != nullptr);
    const auto& value =
        std::get<harness::session::ThinkingLevelChangeValue>(thinking->value);
    CHECK(value.thinking_level == "high");
}

TEST_CASE("resume without a thinking entry appends the restored level",
        "[coding_agent][thinking-persistence][resume][issue404])[spec]") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);

    {
        auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
        REQUIRE(result.has_value());
        result->session->close();

        // Strip the initial thinking entry so the resume path restores from
        // the settings default (pi hasThinkingEntry gate).
        std::ifstream in(fixture.session_file, std::ios::binary);
        std::ostringstream kept;
        std::string line;
        while (std::getline(in, line)) {
            if (line.find(R"("type":"thinking_level_change")") ==
                std::string::npos) {
                kept << line << '\n';
            }
        }
        std::ofstream out(fixture.session_file, std::ios::binary);
        out << kept.str();
    }

    fixture.write_settings(R"({"defaultThinkingLevel":"low"})");

    auto resumed = fixture.runtime.run(coding_agent::create_agent_session_async(cli_resume_request(fixture)));
    REQUIRE(resumed.has_value());
    CHECK(resumed->session->snapshot().agent_state.thinking_level == "low");
    resumed->session->close();

    // pi sdk.ts: the resumed session without a thinking entry gets the
    // restored level appended so a later resume restores it.
    auto loaded = harness::session::SessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    const auto* thinking = find_thinking_entry(*loaded);
    REQUIRE(thinking != nullptr);
    const auto& value =
        std::get<harness::session::ThinkingLevelChangeValue>(thinking->value);
    CHECK(value.thinking_level == "low");
}

TEST_CASE("resume binds the settings manager to the session header cwd",
        "[coding_agent][model-resolution][resume][issue404])[spec]") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);

    {
        auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
        REQUIRE(result.has_value());
        result->session->close();

        // Strip the initial thinking entry so the resumed level comes from
        // the settings default rather than the session entry.
        std::ifstream in(fixture.session_file, std::ios::binary);
        std::ostringstream kept;
        std::string line;
        while (std::getline(in, line)) {
            if (line.find(R"("type":"thinking_level_change")") ==
                std::string::npos) {
                kept << line << '\n';
            }
        }
        std::ofstream out(fixture.session_file, std::ios::binary);
        out << kept.str();
    }

    // The session's project gains a project-scoped default after creation;
    // the launch project (other) carries no project scope at all.
    const auto project_settings = fixture.workspace.path() / ".pi" / "settings.json";
    std::filesystem::create_directories(project_settings.parent_path());
    {
        std::ofstream out(project_settings, std::ios::binary);
        out << R"({"defaultThinkingLevel":"high"})";
    }
    cch::tests::TempWorkspace other;

    // pi main.ts: cwd-bound services (settings, resources, ...) resolve
    // against the target session cwd, not the process cwd — the resumed
    // session sees the session project's scope ("high"), never the launch
    // project's (absent -> "medium").
    auto request = cli_resume_request(fixture);
    request.workspace = other.path();
    auto resumed = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request)));
    REQUIRE(resumed.has_value());
    CHECK(resumed->session->snapshot().agent_state.thinking_level == "high");
    resumed->session->close();
}

// ─────────────────────────────────────────────────────────────────────────────
// Committed golden: thinking_level_change entry shape + settings default write
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("thinking-persistence golden pins the entry shape and the settings default write",
        "[coding_agent][fixture][issue353][issue774])[compat-pi]") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);

    auto result = fixture.runtime.run(coding_agent::create_agent_session_async(cli_request(fixture)));
    REQUIRE(result.has_value());
    // The pinned settings default write is the persist mutation's (pi
    // `setThinkingLevel(level, { persist: true })` → `setDefaultThinkingLevel`);
    // the session-only default records the entry without it.
    auto changed = result->session->set_thinking_level("high", coding_agent::ModelMutationOptions{.persist = true});
    REQUIRE(changed.has_value());
    CHECK(*changed == "high");
    result->session->close();

    auto loaded = harness::session::SessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    const auto* entry = find_thinking_entry(*loaded);
    REQUIRE(entry != nullptr);

    const auto settings = support::read_json(fixture.read_settings());
    REQUIRE(settings.has_value());

    support::JsonValue golden{support::JsonValue::object_t{}};
    golden.get_object().emplace(
        "thinkingLevelChangeEntry", entry_shape_to_json(*entry));
    support::JsonValue settings_default{support::JsonValue::object_t{}};
    const auto& settings_object = settings->get_object();
    const auto found = settings_object.find("defaultThinkingLevel");
    REQUIRE(found != settings_object.end());
    settings_default.get_object().emplace("defaultThinkingLevel", found->second);
    golden.get_object().emplace("settingsDefaultWrite", std::move(settings_default));

    auto serialized = support::write_json(golden);
    REQUIRE(serialized);
    const std::string path = std::string{CCH_SOURCE_DIR} +
                             "/fixtures/pi-agent-core/thinking-persistence.json";
    std::ifstream input(path, std::ios::binary);
    const std::string expected{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
    if (*serialized != expected) {
        std::cerr << "\n[ModelResolutionTest] fixture mismatch: thinking-persistence.json"
                  << "\n--- expected ---\n"
                  << expected << "\n--- actual ---\n"
                  << *serialized << "\n--- end ---\n";
    }
    CHECK(*serialized == expected);
}
