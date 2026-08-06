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
#include <cch/coding_agent/Sdk.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/harness/session/JsonlSessionStore.hpp>
#include <cch/util/Error.hpp>
#include "coding_agent/AgentSessionBridge.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ModelFixture.hpp"
#include "support/TempWorkspace.hpp"
#include "util/Json.hpp"

#include "../../third_party/catch2/catch_test_macros.hpp"

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
    request.session_target = coding_agent::ExplicitNewSessionTarget{fixture.session_file};
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

[[nodiscard]] bool has_diagnostic(
    const std::vector<coding_agent::SdkDiagnostic>& diagnostics,
    std::string_view code) {
    return std::any_of(
        diagnostics.begin(),
        diagnostics.end(),
        [code](const coding_agent::SdkDiagnostic& diagnostic) {
            return diagnostic.code == code;
        });
}

/// The message of the first diagnostic with `code`, for asserting pi's exact
/// fallback-message text.
[[nodiscard]] std::optional<std::string> diagnostic_message(
    const std::vector<coding_agent::SdkDiagnostic>& diagnostics,
    std::string_view code) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.code == code) {
            return diagnostic.message;
        }
    }
    return std::nullopt;
}

/// The canonical `thinking_level_change` entry-shape projection: the fields
/// this ticket owns (type, thinkingLevel). Generic tree metadata (id,
/// timestamp, parentId null-vs-omitted) is pinned by the T07 session-wire
/// contract, not here.
[[nodiscard]] util::JsonValue entry_shape_to_json(
    const harness::session::SessionEntry& entry) {
    util::JsonValue object{util::JsonValue::object_t{}};
    auto& o = object.get_object();
    const auto parsed = util::read_json<util::JsonValue>(entry.raw_line);
    REQUIRE(parsed.has_value());
    const auto& parsed_object = parsed->get_object();
    o.emplace("type", parsed_object.at("type"));
    const auto* thinking = std::get_if<harness::session::ThinkingLevelChangeValue>(&entry.value);
    REQUIRE(thinking != nullptr);
    o.emplace("thinkingLevel", util::JsonValue(thinking->thinking_level));
    return object;
}

[[nodiscard]] const harness::session::SessionEntry* find_thinking_entry(
    const harness::session::LoadedSession& loaded) {
    for (const auto& entry : loaded.entries) {
        if (entry.kind == harness::session::SessionEntryKind::ThinkingLevelChange) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Model resolution chain — CLI profile
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("CLI model resolution: --model wins over settings defaults", "[sdk][model-resolution][issue353]") {
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

TEST_CASE("CLI model resolution: settings default wins with configured auth", "[sdk][model-resolution][issue353]") {
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
    "[sdk][model-resolution][issue353]") {
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

TEST_CASE("CLI model resolution: scoped models select the first scoped model for new sessions", "[sdk][model-resolution][issue353]") {
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
    "[sdk][model-resolution][issue353]") {
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

TEST_CASE("CLI model resolution: resume re-resolves the stored model identity", "[sdk][model-resolution][resume][issue353]") {
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
    CHECK_FALSE(has_diagnostic(resumed->diagnostics, "resume_model_unresolved"));
    resumed->session->close();
}

TEST_CASE(
    "CLI model resolution: resume without configured auth falls back with pi's message",
    "[sdk][model-resolution][resume][issue357]") {
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
    // pi restoreModelFromSession requires `restoredModel && hasConfiguredAuth`,
    // so the chain falls back to the first available model with configured auth
    // and the diagnostic carries pi's "no auth configured" reason plus the
    // resolved fallback identity.
    fixture.write_models(kKeylessBetaKeyedAlpha);

    auto resumed = coding_agent::create_agent_session(cli_resume_request(fixture));
    REQUIRE(resumed.has_value());
    CHECK(resumed->provider == "alpha");
    CHECK(resumed->model == "alpha-1");
    const auto message = diagnostic_message(resumed->diagnostics, "resume_model_unresolved");
    REQUIRE(message.has_value());
    CHECK(*message ==
          "Could not restore model beta/beta-1 (no auth configured). Using alpha/alpha-1.");
    resumed->session->close();
}

TEST_CASE(
    "CLI model resolution: resume with a missing model falls back with pi's message",
    "[sdk][model-resolution][resume][issue357]") {
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
    // pi restoreModelFromSession reports "model no longer exists", the chain
    // continues through the runtime default, and the message names the resolved
    // fallback identity.
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
    const auto message = diagnostic_message(resumed->diagnostics, "resume_model_unresolved");
    REQUIRE(message.has_value());
    CHECK(*message ==
          "Could not restore model beta/beta-1 (model no longer exists). Using gamma/gamma-1.");
    resumed->session->close();
}

TEST_CASE(
    "session files persist only model_change provider/modelId, never auth material",
    "[sdk][model-resolution][resume][issue357]") {
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
    "[sdk][model-resolution][issue353]") {
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

// ─────────────────────────────────────────────────────────────────────────────
// Model resolution chain — SDK public profile
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE(
    "SDK public path resolves the first available model with configured auth",
    "[sdk][model-resolution][issue353]") {
    Fixture fixture;
    fixture.write_models(kKeylessAlphaKeyedBeta);

    coding_agent::CreateAgentSessionOptions options;
    options.session_target = coding_agent::ExplicitNewSessionTarget{fixture.session_file};
    options.workspace = fixture.workspace.path();

    auto result = coding_agent::create_agent_session(std::move(options));
    REQUIRE(result.has_value());
    CHECK(result->provider == "beta");
    CHECK(result->model == "beta-1");
    result->session->close();
}

TEST_CASE(
    "SDK public path honors the settings default only with configured auth",
    "[sdk][model-resolution][issue353]") {
    Fixture fixture;
    fixture.write_models(kKeylessAlphaKeyedBeta);
    // defaultModel alpha-1 (keyless) is skipped; defaultModel beta-1 wins.
    fixture.write_settings(R"({"defaultProvider":"alpha","defaultModel":"alpha-1"})");

    coding_agent::CreateAgentSessionOptions options;
    options.session_target = coding_agent::ExplicitNewSessionTarget{fixture.session_file};
    options.workspace = fixture.workspace.path();

    auto result = coding_agent::create_agent_session(std::move(options));
    REQUIRE(result.has_value());
    CHECK(result->provider == "beta");
    CHECK(result->model == "beta-1");
    result->session->close();
}

TEST_CASE(
    "SDK public path resume re-resolves the stored model with configured auth",
    "[sdk][model-resolution][resume][issue353]") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);

    {
        coding_agent::CreateAgentSessionOptions options;
        options.session_target = coding_agent::ExplicitNewSessionTarget{fixture.session_file};
        options.workspace = fixture.workspace.path();
        auto created = coding_agent::create_agent_session(std::move(options));
        REQUIRE(created.has_value());
        CHECK(created->provider == "alpha");
        created->session->close();
    }

    coding_agent::CreateAgentSessionOptions options;
    options.session_target = coding_agent::ExplicitResumeSessionTarget{fixture.session_file};
    options.workspace = fixture.workspace.path();

    auto resumed = coding_agent::create_agent_session(std::move(options));
    REQUIRE(resumed.has_value());
    CHECK(resumed->provider == "alpha");
    CHECK(resumed->model == "alpha-1");
    CHECK_FALSE(has_diagnostic(resumed->diagnostics, "resume_model_unresolved"));
    resumed->session->close();
}

TEST_CASE(
    "SDK public path resume re-resolves a non-default stored model identity",
    "[sdk][model-resolution][resume][issue357]") {
    Fixture fixture;
    fixture.write_models(kTwoKeyedProviders);

    {
        // Explicitly request beta-1 so the stored model_change is not the
        // runtime default (alpha-1): resume must re-resolve the recorded
        // identity, not fall through to the first available model.
        coding_agent::CreateAgentSessionOptions options;
        options.session_target = coding_agent::ExplicitNewSessionTarget{fixture.session_file};
        options.workspace = fixture.workspace.path();
        options.model = tests::make_model("beta-1", "beta");
        auto created = coding_agent::create_agent_session(std::move(options));
        REQUIRE(created.has_value());
        CHECK(created->provider == "beta");
        CHECK(created->model == "beta-1");
        created->session->close();
    }

    coding_agent::CreateAgentSessionOptions options;
    options.session_target = coding_agent::ExplicitResumeSessionTarget{fixture.session_file};
    options.workspace = fixture.workspace.path();

    auto resumed = coding_agent::create_agent_session(std::move(options));
    REQUIRE(resumed.has_value());
    CHECK(resumed->provider == "beta");
    CHECK(resumed->model == "beta-1");
    CHECK_FALSE(has_diagnostic(resumed->diagnostics, "resume_model_unresolved"));
    resumed->session->close();
}

// ─────────────────────────────────────────────────────────────────────────────
// Thinking-level persistence
// ─────────────────────────────────────────────────────────────────────────────

namespace {

[[nodiscard]] coding_agent::CreateAgentSessionOptions reasoning_session_options(
    const Fixture& fixture) {
    coding_agent::CreateAgentSessionOptions options;
    options.session_target = coding_agent::ExplicitNewSessionTarget{fixture.session_file};
    options.workspace = fixture.workspace.path();
    return options;
}

} // namespace

TEST_CASE(
    "set_thinking_level persists a thinking_level_change entry and the settings default",
    "[sdk][thinking-persistence][issue353]") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);

    auto result = coding_agent::create_agent_session(reasoning_session_options(fixture));
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
    const auto settings = util::read_json<util::JsonValue>(settings_text);
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
    "[sdk][thinking-persistence][resume][issue353]") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);

    {
        auto result = coding_agent::create_agent_session(reasoning_session_options(fixture));
        REQUIRE(result.has_value());
        auto changed = result->session->set_thinking_level("high");
        REQUIRE(changed.has_value());
        CHECK(*changed == "high");
        result->session->close();
    }

    // Resume: the nearest `thinking_level_change` on the active path wins over
    // the settings default and DEFAULT_THINKING_LEVEL (pi sdk.ts).
    coding_agent::CreateAgentSessionOptions options;
    options.session_target = coding_agent::ExplicitResumeSessionTarget{fixture.session_file};
    options.workspace = fixture.workspace.path();

    auto resumed = coding_agent::create_agent_session(std::move(options));
    REQUIRE(resumed.has_value());
    CHECK(resumed->session->snapshot().agent_state.thinking_level == "high");
    resumed->session->close();
}

TEST_CASE(
    "resumed session without a thinking entry uses the settings default",
    "[sdk][thinking-persistence][resume][issue353]") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);

    {
        // Create without any level change: no thinking_level_change entry.
        auto result = coding_agent::create_agent_session(reasoning_session_options(fixture));
        REQUIRE(result.has_value());
        result->session->close();
    }

    fixture.write_settings(R"({"defaultThinkingLevel":"low"})");

    coding_agent::CreateAgentSessionOptions options;
    options.session_target = coding_agent::ExplicitResumeSessionTarget{fixture.session_file};
    options.workspace = fixture.workspace.path();

    auto resumed = coding_agent::create_agent_session(std::move(options));
    REQUIRE(resumed.has_value());
    CHECK(resumed->session->snapshot().agent_state.thinking_level == "low");
    resumed->session->close();
}

TEST_CASE(
    "fresh session requests the settings default thinking level",
    "[sdk][thinking-persistence][issue353]") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);
    fixture.write_settings(R"({"defaultThinkingLevel":"high"})");

    auto result = coding_agent::create_agent_session(reasoning_session_options(fixture));
    REQUIRE(result.has_value());
    // The Agent clamped the settings default against the reasoning model; the
    // supported set (no thinkingLevelMap) is off..high, so "high" survives.
    CHECK(result->session->snapshot().agent_state.thinking_level == "high");
    result->session->close();
}

TEST_CASE(
    "set_thinking_level to off persists on a reasoning model (pi supportsThinking gate)",
    "[sdk][thinking-persistence][issue353]") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);

    auto result = coding_agent::create_agent_session(reasoning_session_options(fixture));
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

    const auto settings = util::read_json<util::JsonValue>(fixture.read_settings());
    REQUIRE(settings.has_value());
    const auto& settings_object = settings->get_object();
    const auto found = settings_object.find("defaultThinkingLevel");
    REQUIRE(found != settings_object.end());
    CHECK(*found->second.get_if<std::string>() == "off");
}

TEST_CASE(
    "set_thinking_level clamps to the active model and rejects invalid levels",
    "[sdk][thinking-persistence][issue353]") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);

    auto result = coding_agent::create_agent_session(reasoning_session_options(fixture));
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

    // A no-op change persists nothing: exactly one thinking entry remains.
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
    CHECK(entries == 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Committed golden: thinking_level_change entry shape + settings default write
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE(
    "thinking-persistence golden pins the entry shape and the settings default write",
    "[sdk][fixture][issue353]") {
    Fixture fixture;
    fixture.write_models(kKeyedReasoningProvider);

    auto result = coding_agent::create_agent_session(reasoning_session_options(fixture));
    REQUIRE(result.has_value());
    auto changed = result->session->set_thinking_level("high");
    REQUIRE(changed.has_value());
    CHECK(*changed == "high");
    result->session->close();

    auto loaded = harness::session::JsonlSessionStore::load(fixture.session_file);
    REQUIRE(loaded.has_value());
    const auto* entry = find_thinking_entry(*loaded);
    REQUIRE(entry != nullptr);

    const auto settings = util::read_json<util::JsonValue>(fixture.read_settings());
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
