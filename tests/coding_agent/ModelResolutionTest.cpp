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
#include <cch/agent/harness/session/JsonlSessionStore.hpp>
#include <cch/util/Error.hpp>
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ModelFixture.hpp"
#include "support/TempWorkspace.hpp"
#include "util/Json.hpp"

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

using namespace cch;

namespace {

/// One isolated assembly fixture: a temp workspace for the session file and a
/// temp Agent Config Directory (`PI_CODING_AGENT_DIR`) whose models.json /
/// settings.json drive the resolution chain deterministically. Ambient
/// KIMI_API_KEY is unset so the built-in kimi-coding provider never resolves
/// as configured unless the test says so.
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
    "deepseek": {
      "baseUrl": "https://api.deepseek.example/v1",
      "api": "openai-responses",
      "apiKey": "dummy-deepseek-key",
      "models": [{"id": "deepseek-v4-flash", "reasoning": true}]
    }
  }
})";

/// One keyed non-reasoning provider.
[[nodiscard]] coding_agent::runtime::AgentSessionCreationRequest cli_request(
    const Fixture& fixture) {
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_target = coding_agent::ExplicitOpenOrCreateSessionTarget{fixture.session_file};
    request.workspace = fixture.workspace.path();
    return request;
}

[[nodiscard]] coding_agent::runtime::AgentSessionCreationRequest cli_resume_request(
    const Fixture& fixture) {
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_target = coding_agent::ExplicitResumeSessionTarget{fixture.session_file};
    request.workspace = fixture.workspace.path();
    return request;
}

/// The canonical `thinking_level_change` entry-shape projection: the fields
/// this ticket owns (type, thinkingLevel). Generic tree metadata (id,
/// timestamp, parentId null-vs-omitted) is pinned by the T07 session-wire
/// contract, not here.
[[nodiscard]] util::JsonValue entry_shape_to_json(
    const harness::session::SessionEntry& entry) {
    util::JsonValue object{util::JsonValue::object_t{}};
    auto& o = object.get_object();
    const auto parsed = util::read_json(entry.raw_line);
    REQUIRE(parsed.has_value());
    const auto& parsed_object = parsed->get_object();
    o.emplace("type", parsed_object.at("type"));
    const auto* thinking = std::get_if<harness::session::ThinkingLevelChangeValue>(&entry.value);
    REQUIRE(thinking != nullptr);
    o.emplace("thinkingLevel", util::JsonValue(thinking->thinking_level));
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

TEST_CASE("CLI model resolution: --model wins over settings defaults", "[coding_agent][model-resolution][issue353])") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);
    fixture.write_settings(R"({"defaultProvider":"alpha","defaultModel":"alpha-1"})");

    auto request = cli_request(fixture);
    request.provider = "beta";
    request.model = "beta-1";

    auto result = coding_agent::create_agent_session(std::move(request));
    REQUIRE(result.has_value());
    CHECK(result->provider == "beta");
    CHECK(result->model == "beta-1");
    CHECK(result->session->provider() == "beta");
    CHECK(result->session->model() == "beta-1");
    result->session->close();
}

TEST_CASE("CLI model resolution: settings default wins with configured auth", "[coding_agent][model-resolution][issue353])") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);
    fixture.write_settings(R"({"defaultProvider":"beta","defaultModel":"beta-1"})");

    auto result = coding_agent::create_agent_session(cli_request(fixture));
    REQUIRE(result.has_value());
    CHECK(result->provider == "beta");
    CHECK(result->model == "beta-1");
    result->session->close();
}

TEST_CASE(
    "CLI model resolution: unauthenticated settings default falls through to first available with auth",
    "[coding_agent][model-resolution][issue353])") {
    Fixture fixture;
    fixture.write_models(kKeylessAlphaKeyedBeta);
    // The saved default (alpha-1) has no configured auth; pi findInitialModel
    // skips it and the chain resolves the first available model whose provider
    // has configured auth (beta-1), never the keyless alpha-1.
    fixture.write_settings(R"({"defaultProvider":"alpha","defaultModel":"alpha-1"})");

    auto result = coding_agent::create_agent_session(cli_request(fixture));
    REQUIRE(result.has_value());
    CHECK(result->provider == "beta");
    CHECK(result->model == "beta-1");
    result->session->close();
}

TEST_CASE("CLI model resolution: scoped models select the first scoped model for new sessions", "[coding_agent][model-resolution][issue353])") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);

    auto request = cli_request(fixture);
    request.models = {"alpha*"};

    auto result = coding_agent::create_agent_session(std::move(request));
    REQUIRE(result.has_value());
    CHECK(result->provider == "alpha");
    CHECK(result->model == "alpha-1");
    result->session->close();
}

TEST_CASE(
    "CLI model resolution: the saved default in scope wins over the first scoped model",
    "[coding_agent][model-resolution][issue353])") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);
    fixture.write_settings(R"({"defaultProvider":"beta","defaultModel":"beta-1"})");

    auto request = cli_request(fixture);
    request.models = {"alpha*", "beta*"};

    auto result = coding_agent::create_agent_session(std::move(request));
    REQUIRE(result.has_value());
    // beta-1 is in scope and is the saved default, so it wins over the first
    // scoped model (alpha-1).
    CHECK(result->provider == "beta");
    CHECK(result->model == "beta-1");
    result->session->close();
}

TEST_CASE("CLI model resolution: resume re-resolves the stored model identity", "[coding_agent][model-resolution][resume][issue353])") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);

    {
        auto request = cli_request(fixture);
        request.provider = "beta";
        request.model = "beta-1";
        auto created = coding_agent::create_agent_session(std::move(request));
        REQUIRE(created.has_value());
        created->session->close();
    }

    // Resume with no CLI model flags: the stored `model_change {beta, beta-1}`
    // re-resolves against the live runtime catalog.
    auto resumed = coding_agent::create_agent_session(cli_resume_request(fixture));
    REQUIRE(resumed.has_value());
    CHECK(resumed->provider == "beta");
    CHECK(resumed->model == "beta-1");
    CHECK_FALSE(resumed->model_fallback_message.has_value());
    resumed->session->close();
}

TEST_CASE(
    "CLI model resolution: resume without configured auth falls back with pi's message",
    "[coding_agent][model-resolution][resume][issue357])") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);

    {
        auto request = cli_request(fixture);
        request.provider = "beta";
        request.model = "beta-1";
        auto created = coding_agent::create_agent_session(std::move(request));
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

    auto resumed = coding_agent::create_agent_session(cli_resume_request(fixture));
    REQUIRE(resumed.has_value());
    CHECK(resumed->provider == "alpha");
    CHECK(resumed->model == "alpha-1");
    REQUIRE(resumed->model_fallback_message.has_value());
    CHECK(*resumed->model_fallback_message ==
          "Could not restore model beta/beta-1. Using alpha/alpha-1");
    resumed->session->close();
}

TEST_CASE(
    "resume restore failure with nothing available reports the no-models message",
    "[coding_agent][model-resolution][resume][issue404])") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);

    {
        auto request = cli_request(fixture);
        request.provider = "beta";
        request.model = "beta-1";
        auto created = coding_agent::create_agent_session(std::move(request));
        REQUIRE(created.has_value());
        created->session->close();
    }

    // The catalog disappears entirely: the restore fails and the chain lands
    // on the unknown placeholder, so pi replaces the fallback message with
    // `formatNoModelsAvailableMessage()` (sdk.ts `if (!model)` branch).
    std::filesystem::remove(fixture.agent_dir.path() / "models.json");

    auto resumed = coding_agent::create_agent_session(cli_resume_request(fixture));
    REQUIRE(resumed.has_value());
    CHECK(resumed->provider == "unknown");
    CHECK(resumed->model == "unknown");
    REQUIRE(resumed->model_fallback_message.has_value());
    CHECK(resumed->model_fallback_message->starts_with(
        "No models available. Use /login to log into a provider via OAuth or API key. See:"));
    resumed->session->close();
}

TEST_CASE(
    "CLI model resolution: resume with a missing model falls back with pi's message",
    "[coding_agent][model-resolution][resume][issue357])") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);

    {
        auto request = cli_request(fixture);
        request.provider = "beta";
        request.model = "beta-1";
        auto created = coding_agent::create_agent_session(std::move(request));
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

    auto resumed = coding_agent::create_agent_session(cli_resume_request(fixture));
    REQUIRE(resumed.has_value());
    CHECK(resumed->provider == "gamma");
    CHECK(resumed->model == "gamma-1");
    REQUIRE(resumed->model_fallback_message.has_value());
    CHECK(*resumed->model_fallback_message ==
          "Could not restore model beta/beta-1. Using gamma/gamma-1");
    resumed->session->close();
}

TEST_CASE(
    "session files persist only model_change provider/modelId, never auth material",
    "[coding_agent][model-resolution][resume][issue357])") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);

    {
        auto request = cli_request(fixture);
        request.provider = "beta";
        request.model = "beta-1";
        auto created = coding_agent::create_agent_session(std::move(request));
        REQUIRE(created.has_value());
        created->session->close();
    }

    auto loaded = harness::session::JsonlSessionStore::load(fixture.session_file);
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

TEST_CASE(
    "CLI model resolution: nothing configured keeps kDefaultModel and fails through provider lookup",
    "[coding_agent][model-resolution][issue353])") {
    Fixture fixture;
    // Empty Agent Config Directory: no providers, no models, no auth.
    auto result = coding_agent::create_agent_session(cli_request(fixture));
    REQUIRE(result.has_value());
    // The concrete unknown kDefaultModel is the resolved identity, with no
    // construction-time default silently winning.
    CHECK(result->provider == "unknown");
    CHECK(result->model == "unknown");
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
    REQUIRE(messages.size() == 2);
    const auto& terminal = std::get<ai::AssistantMessage>(messages.back());
    CHECK(terminal.stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(terminal.error_message);
    CHECK(terminal.error_message->find("Unknown provider: unknown") != std::string::npos);
    result->session->close();
}

TEST_CASE(
    "a zero-model session never persists a placeholder model_change identity",
    "[coding_agent][model-resolution][issue404])") {
    Fixture fixture;
    // Empty Agent Config Directory: nothing available, so the resolution
    // lands on the unknown placeholder. pi sdk.ts guards the model_change
    // append with `if (model)`; the initial thinking entry still persists
    // (pi appends it unconditionally, clamped to "off" for the placeholder).
    auto result = coding_agent::create_agent_session(cli_request(fixture));
    REQUIRE(result.has_value());
    result->session->close();

    auto loaded = harness::session::JsonlSessionStore::load(fixture.session_file);
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

TEST_CASE(
    "default creation resolves the first available model with configured auth",
    "[coding_agent][model-resolution][issue353])") {
    Fixture fixture;
    fixture.write_models(kKeylessAlphaKeyedBeta);

    auto result = coding_agent::create_agent_session(cli_request(fixture));
    REQUIRE(result.has_value());
    CHECK(result->provider == "beta");
    CHECK(result->model == "beta-1");
    result->session->close();
}

TEST_CASE(
    "default creation honors the settings default only with configured auth",
    "[coding_agent][model-resolution][issue353])") {
    Fixture fixture;
    fixture.write_models(kKeylessAlphaKeyedBeta);
    // defaultModel alpha-1 (keyless) is skipped; defaultModel beta-1 wins.
    fixture.write_settings(R"({"defaultProvider":"alpha","defaultModel":"alpha-1"})");

    auto result = coding_agent::create_agent_session(cli_request(fixture));
    REQUIRE(result.has_value());
    CHECK(result->provider == "beta");
    CHECK(result->model == "beta-1");
    result->session->close();
}

TEST_CASE(
    "default creation resume re-resolves the stored model with configured auth",
    "[coding_agent][model-resolution][resume][issue353])") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);

    {
        auto created = coding_agent::create_agent_session(cli_request(fixture));
        REQUIRE(created.has_value());
        CHECK(created->provider == "alpha");
        created->session->close();
    }

    auto resumed = coding_agent::create_agent_session(cli_resume_request(fixture));
    REQUIRE(resumed.has_value());
    CHECK(resumed->provider == "alpha");
    CHECK(resumed->model == "alpha-1");
    CHECK_FALSE(resumed->model_fallback_message.has_value());
    resumed->session->close();
}

TEST_CASE(
    "default creation resume re-resolves a non-default stored model identity",
    "[coding_agent][model-resolution][resume][issue357])") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);

    {
        // Explicitly request beta-1 so the stored model_change is not the
        // runtime default (alpha-1): resume must re-resolve the recorded
        // identity, not fall through to the first available model.
        auto request = cli_request(fixture);
        request.provider = "beta";
        request.model = "beta-1";
        auto created = coding_agent::create_agent_session(std::move(request));
        REQUIRE(created.has_value());
        CHECK(created->provider == "beta");
        CHECK(created->model == "beta-1");
        created->session->close();
    }

    auto resumed = coding_agent::create_agent_session(cli_resume_request(fixture));
    REQUIRE(resumed.has_value());
    CHECK(resumed->provider == "beta");
    CHECK(resumed->model == "beta-1");
    CHECK_FALSE(resumed->model_fallback_message.has_value());
    resumed->session->close();
}

// ─────────────────────────────────────────────────────────────────────────────
// Thinking-level persistence
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE(
    "set_thinking_level persists a thinking_level_change entry and the settings default",
    "[coding_agent][thinking-persistence][issue353])") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);

    auto result = coding_agent::create_agent_session(cli_request(fixture));
    REQUIRE(result.has_value());
    // The resolution chain landed the first available model with configured
    // auth; the session's model supports reasoning so a level change is real.
    CHECK(result->model == "deepseek-v4-flash");

    auto changed = result->session->set_thinking_level("high");
    REQUIRE(changed.has_value());
    CHECK(*changed == "high");
    CHECK(result->session->snapshot().agent_state.thinking_level == "high");
    result->session->close();

    // The durable session file carries the `thinking_level_change` entry.
    auto loaded = harness::session::JsonlSessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    const auto* entry = find_thinking_entry(*loaded);
    REQUIRE(entry != nullptr);

    // The global settings file carries the defaultThinkingLevel write.
    const auto settings_text = fixture.read_settings();
    const auto settings = util::read_json(settings_text);
    REQUIRE(settings.has_value());
    const auto& settings_object = settings->get_object();
    const auto found = settings_object.find("defaultThinkingLevel");
    REQUIRE(found != settings_object.end());
    CHECK(found->second.get_if<std::string>() != nullptr);
    CHECK(*found->second.get_if<std::string>() == "high");

    // A reloaded settings manager sees the merged default too.
    auto reloaded = coding_agent::SettingsManager::create(
        fixture.workspace.path(), fixture.agent_dir.path(), /* project_trusted */ false);
    CHECK(reloaded.settings().default_thinking_level == "high");
}

TEST_CASE(
    "resume restores the persisted thinking level from the session entry",
    "[coding_agent][thinking-persistence][resume][issue353])") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);

    {
        auto result = coding_agent::create_agent_session(cli_request(fixture));
        REQUIRE(result.has_value());
        auto changed = result->session->set_thinking_level("high");
        REQUIRE(changed.has_value());
        CHECK(*changed == "high");
        result->session->close();
    }

    // Resume: the nearest `thinking_level_change` on the active path wins over
    // the settings default and DEFAULT_THINKING_LEVEL (pi sdk.ts).
    auto resumed = coding_agent::create_agent_session(cli_resume_request(fixture));
    REQUIRE(resumed.has_value());
    CHECK(resumed->session->snapshot().agent_state.thinking_level == "high");
    resumed->session->close();
}

TEST_CASE(
    "resumed session without a thinking entry uses the settings default",
    "[coding_agent][thinking-persistence][resume][issue353])") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);

    {
        // Create without any level change: the new-session initial
        // `thinking_level_change` entry records the creation level. Strip it
        // so the resumed session genuinely has no thinking entry (the
        // hasThinkingEntry gate pi gates against).
        auto result = coding_agent::create_agent_session(cli_request(fixture));
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

    auto resumed = coding_agent::create_agent_session(cli_resume_request(fixture));
    REQUIRE(resumed.has_value());
    CHECK(resumed->session->snapshot().agent_state.thinking_level == "low");
    resumed->session->close();
}

TEST_CASE(
    "fresh session requests the settings default thinking level",
    "[coding_agent][thinking-persistence][issue353])") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);
    fixture.write_settings(R"({"defaultThinkingLevel":"high"})");

    auto result = coding_agent::create_agent_session(cli_request(fixture));
    REQUIRE(result.has_value());
    // The Agent clamped the settings default against the reasoning model; the
    // supported set (no thinkingLevelMap) is off..high, so "high" survives.
    CHECK(result->session->snapshot().agent_state.thinking_level == "high");
    result->session->close();
}

TEST_CASE(
    "set_thinking_level to off persists on a reasoning model (pi supportsThinking gate)",
    "[coding_agent][thinking-persistence][issue353])") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);

    auto result = coding_agent::create_agent_session(cli_request(fixture));
    REQUIRE(result.has_value());
    CHECK(result->session->snapshot().agent_state.thinking_level == "medium");

    // A real change to "off" on a reasoning model persists both the entry and
    // the settings default (pi: `supportsThinking() || effectiveLevel !== "off"`).
    auto changed = result->session->set_thinking_level("off");
    REQUIRE(changed.has_value());
    CHECK(*changed == "off");
    CHECK(result->session->snapshot().agent_state.thinking_level == "off");
    result->session->close();

    auto loaded = harness::session::JsonlSessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    REQUIRE(find_thinking_entry(*loaded) != nullptr);

    const auto settings = util::read_json(fixture.read_settings());
    REQUIRE(settings.has_value());
    const auto& settings_object = settings->get_object();
    const auto found = settings_object.find("defaultThinkingLevel");
    REQUIRE(found != settings_object.end());
    CHECK(*found->second.get_if<std::string>() == "off");
}

TEST_CASE(
    "set_thinking_level clamps to the active model and rejects invalid levels",
    "[coding_agent][thinking-persistence][issue353])") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);

    auto result = coding_agent::create_agent_session(cli_request(fixture));
    REQUIRE(result.has_value());

    // No thinkingLevelMap: supported is off..high, so "max" clamps to "high".
    auto clamped = result->session->set_thinking_level("max");
    REQUIRE(clamped.has_value());
    CHECK(*clamped == "high");
    CHECK(result->session->snapshot().agent_state.thinking_level == "high");

    // An invalid level is rejected without state or persistence changes.
    auto invalid = result->session->set_thinking_level("sometimes");
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error().code == util::ErrorCode::Validation);
    CHECK(result->session->snapshot().agent_state.thinking_level == "high");

    // A no-op change persists nothing: the initial creation entry plus the
    // one real change remain (pi appends the initial thinking level at
    // creation, so a new session starts with exactly one entry).
    auto unchanged = result->session->set_thinking_level("high");
    REQUIRE(unchanged.has_value());
    CHECK(*unchanged == "high");
    result->session->close();

    auto loaded = harness::session::JsonlSessionStore::load(fixture.session_file);
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

TEST_CASE(
    "new sessions append model_change then the initial thinking_level_change",
    "[coding_agent][model-resolution][thinking-persistence][issue404])") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);

    auto result = coding_agent::create_agent_session(cli_request(fixture));
    REQUIRE(result.has_value());
    CHECK(result->model == "deepseek-v4-flash");
    result->session->close();

    // pi sdk.ts: a new session appends `model_change {provider, modelId}` and
    // the initial (clamped) `thinking_level_change` so a later resume can
    // restore both. The reasoning model supports off..high, so the creation
    // default "medium" survives the clamp.
    auto loaded = harness::session::JsonlSessionStore::load(fixture.session_file);
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

TEST_CASE(
    "the initial thinking entry carries the clamped creation level",
    "[coding_agent][model-resolution][thinking-persistence][issue404])") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);
    // A non-reasoning stored model would clamp the default to "off" (pi:
    // `if (!model) thinkingLevel = "off"` and capability clamping); here the
    // settings default "max" clamps to the reasoning model's "high" and the
    // persisted entry records the clamped value, not the request.
    fixture.write_settings(R"({"defaultThinkingLevel":"max"})");

    auto result = coding_agent::create_agent_session(cli_request(fixture));
    REQUIRE(result.has_value());
    CHECK(result->session->snapshot().agent_state.thinking_level == "high");
    result->session->close();

    auto loaded = harness::session::JsonlSessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    const auto* thinking = find_thinking_entry(*loaded);
    REQUIRE(thinking != nullptr);
    const auto& value =
        std::get<harness::session::ThinkingLevelChangeValue>(thinking->value);
    CHECK(value.thinking_level == "high");
}

TEST_CASE(
    "resume without a thinking entry appends the restored level",
    "[coding_agent][thinking-persistence][resume][issue404])") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);

    {
        auto result = coding_agent::create_agent_session(cli_request(fixture));
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

    auto resumed = coding_agent::create_agent_session(cli_resume_request(fixture));
    REQUIRE(resumed.has_value());
    CHECK(resumed->session->snapshot().agent_state.thinking_level == "low");
    resumed->session->close();

    // pi sdk.ts: the resumed session without a thinking entry gets the
    // restored level appended so a later resume restores it.
    auto loaded = harness::session::JsonlSessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    const auto* thinking = find_thinking_entry(*loaded);
    REQUIRE(thinking != nullptr);
    const auto& value =
        std::get<harness::session::ThinkingLevelChangeValue>(thinking->value);
    CHECK(value.thinking_level == "low");
}

TEST_CASE(
    "resume binds the settings manager to the session header cwd",
    "[coding_agent][model-resolution][resume][issue404])") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);

    {
        auto result = coding_agent::create_agent_session(cli_request(fixture));
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
    auto resumed = coding_agent::create_agent_session(std::move(request));
    REQUIRE(resumed.has_value());
    CHECK(resumed->session->snapshot().agent_state.thinking_level == "high");
    resumed->session->close();
}

// ─────────────────────────────────────────────────────────────────────────────
// Committed golden: thinking_level_change entry shape + settings default write
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE(
    "thinking-persistence golden pins the entry shape and the settings default write",
    "[coding_agent][fixture][issue353])") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);

    auto result = coding_agent::create_agent_session(cli_request(fixture));
    REQUIRE(result.has_value());
    auto changed = result->session->set_thinking_level("high");
    REQUIRE(changed.has_value());
    CHECK(*changed == "high");
    result->session->close();

    auto loaded = harness::session::JsonlSessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    const auto* entry = find_thinking_entry(*loaded);
    REQUIRE(entry != nullptr);

    const auto settings = util::read_json(fixture.read_settings());
    REQUIRE(settings.has_value());

    util::JsonValue golden{util::JsonValue::object_t{}};
    golden.get_object().emplace(
        "thinkingLevelChangeEntry", entry_shape_to_json(*entry));
    util::JsonValue settings_default{util::JsonValue::object_t{}};
    const auto& settings_object = settings->get_object();
    const auto found = settings_object.find("defaultThinkingLevel");
    REQUIRE(found != settings_object.end());
    settings_default.get_object().emplace("defaultThinkingLevel", found->second);
    golden.get_object().emplace("settingsDefaultWrite", std::move(settings_default));

    auto serialized = util::write_json(golden);
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
