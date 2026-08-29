#include "ai/ModelStreamBridge.hpp"
#include <cch/coding_agent/ModelRuntime.hpp>
#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/coding_agent/AuthStorage.hpp>
#include "coding_agent/ModelRuntimeTestSupport.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ModelsFixture.hpp"
#include "support/PiEventSnapshot.hpp"
#include "support/PiFixture.hpp"
#include "support/StreamAdapterFixture.hpp"
#include "support/TempWorkspace.hpp"
#include "support/Json.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <fstream>
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

using tests::run_awaitable;
using tests::run_async_result;
using tests::ScriptedTransport;
using tests::TransportAttempt;

[[nodiscard]] std::string read_fixture_text(std::string_view relative_path) {
    const std::string path = std::string{CCH_SOURCE_DIR} + "/fixtures/pi-ai/" +
                             std::string{relative_path};
    std::ifstream input(path, std::ios::binary);
    return std::string{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
}

class MemoryCredentialStore final : public ai::CredentialStore {
public:
    [[nodiscard]] cch::support::AsyncResult<std::optional<ai::Credential>> read(
        std::string provider_id) override {
        std::optional<ai::Credential> value;
        if (const auto found = records.find(provider_id); found != records.end()) {
            value = found->second;
        }
        return cch::support::AsyncResult<std::optional<ai::Credential>>(
            std::expected<std::optional<ai::Credential>, cch::support::Error>{std::move(value)});
    }

    [[nodiscard]] cch::support::AsyncResult<std::vector<ai::CredentialInfo>> list() override {
        std::vector<ai::CredentialInfo> result;
        for (const auto& [id, credential] : records) {
            result.push_back(ai::CredentialInfo{
                .provider_id = id,
                .type = std::holds_alternative<ai::OAuthCredential>(credential) ? "oauth" : "api_key",
            });
        }
        return cch::support::AsyncResult<std::vector<ai::CredentialInfo>>(
            std::expected<std::vector<ai::CredentialInfo>, cch::support::Error>{std::move(result)});
    }

    [[nodiscard]] cch::support::AsyncResult<std::optional<ai::Credential>> modify(
        std::string provider_id,
        ai::CredentialModifyHook modifier) override {
        std::optional<ai::Credential> current;
        if (const auto found = records.find(provider_id); found != records.end()) {
            current = found->second;
        }
        auto updated = tests::run_hook(modifier(std::move(current)));
        if (!updated) {
            return cch::support::AsyncResult<std::optional<ai::Credential>>(
                std::expected<std::optional<ai::Credential>, cch::support::Error>{
                    std::unexpect, std::move(updated.error())});
        }
        if (*updated) {
            records.insert_or_assign(provider_id, **updated);
        }
        return cch::support::AsyncResult<std::optional<ai::Credential>>(
            std::expected<std::optional<ai::Credential>, cch::support::Error>{
                std::move(current)});
    }

    [[nodiscard]] cch::support::AsyncResult<void> remove(
        std::string provider_id) override {
        records.erase(provider_id);
        return cch::support::AsyncResult<void>(
            std::expected<void, cch::support::Error>{});
    }

    std::map<std::string, ai::Credential, std::less<>> records;
};

/// Provider with a scripted api-key login that always succeeds and a resolve
/// that returns the stored key.
class LoginProvider final : public ai::Provider {
public:
    LoginProvider() {
        ai::ApiKeyAuth api_key;
        api_key.name = "scripted";
        api_key.login = [](ai::AuthInteraction)
            -> cch::support::AsyncResult<ai::ApiKeyCredential> {
            ai::ApiKeyCredential credential;
            credential.key = "dummy-login-key";
            return cch::support::AsyncResult<ai::ApiKeyCredential>(
                std::expected<ai::ApiKeyCredential, cch::support::Error>{credential});
        };
        api_key.check = [](const ai::AuthContext&, std::optional<ai::ApiKeyCredential> credential)
            -> cch::support::AsyncResult<std::optional<ai::AuthCheck>> {
            if (credential && credential->key) {
                return cch::support::AsyncResult<std::optional<ai::AuthCheck>>(
                    std::expected<std::optional<ai::AuthCheck>, cch::support::Error>{
                        ai::AuthCheck{.source = "stored credential", .type = ai::AuthType::ApiKey}});
            }
            return cch::support::AsyncResult<std::optional<ai::AuthCheck>>(
                std::expected<std::optional<ai::AuthCheck>, cch::support::Error>{
                    std::optional<ai::AuthCheck>{}});
        };
        api_key.resolve = [](const ai::AuthContext&, std::optional<ai::ApiKeyCredential> credential)
            -> cch::support::AsyncResult<std::optional<ai::AuthResult>> {
            if (credential && credential->key) {
                return cch::support::AsyncResult<std::optional<ai::AuthResult>>(
                    std::expected<std::optional<ai::AuthResult>, cch::support::Error>{
                        ai::AuthResult{
                            .auth = ai::ModelAuth{.api_key = *credential->key},
                            .source = "stored credential",
                        }});
            }
            return cch::support::AsyncResult<std::optional<ai::AuthResult>>(
                std::expected<std::optional<ai::AuthResult>, cch::support::Error>{
                    std::optional<ai::AuthResult>{}});
        };
        auth_.api_key = std::move(api_key);
    }

    [[nodiscard]] std::string_view id() const noexcept override { return "login-provider"; }
    [[nodiscard]] std::string_view name() const noexcept override { return "Login Provider"; }
    [[nodiscard]] ai::ProviderAuth& auth() noexcept override { return auth_; }
    [[nodiscard]] std::vector<ai::Model> models() const override { return {}; }

        [[nodiscard]] ai::ModelStream stream(
        ai::Model,
        ai::AiContext,
        ai::ProviderStreamOptions) override {
            return ai::detail::make_model_stream(
                    [](ai::AssistantEventSink sink) mutable
                            -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
                        ai::AssistantMessage message;
                        message.api = "unknown";
                        message.provider = "login-provider";
                        message.model = "host-client";
                        message.stop_reason = ai::AssistantStopReason::Stop;
                        if (sink) {
                            if (auto emitted = sink(
                                        ai::AssistantDoneEvent{.reason = message.stop_reason, .message = message});
                                    !emitted) {
                                co_return std::unexpected(emitted.error());
                            }
                        }
                        co_return message;
                    });
    }


private:
    ai::ProviderAuth auth_;
};

[[nodiscard]] ai::AiContext request_context() {
    ai::AiContext context;
    context.system_prompt = "system";
    context.messages.push_back(ai::UserMessage{
        .content = std::vector<ai::Content>{
            ai::text_content("hi"),
            ai::image_content("YWJj", "image/png"),
        },
        .timestamp = 1,
    });
    context.tools.push_back(ai::Tool{
        .name = "lookup",
        .description = "Look up a value",
        .parameters = support::JsonValue::object_t{
            {"properties", support::JsonValue::object_t{
                {"q", support::JsonValue::object_t{{"type", "string"}}},
            }},
            {"required", support::JsonValue::array_t{"q"}},
            {"type", "object"},
        },
    });
    return context;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// ModelRuntime: composition, refresh, availability, login/logout, vertical path
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ModelRuntime default-created runtime composes the built-in providers", "[coding_agent][model-runtime][issue345]") {
    tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR", std::nullopt};
    home_guard.set(home.path().string());

    auto runtime = coding_agent::ModelRuntime::create({});
    REQUIRE(runtime);
    CHECK((*runtime)->agent_dir() == (home.path() / ".pi" / "agent"));
    CHECK((*runtime)->models_path() == (home.path() / ".pi" / "agent" / "models.json"));
    CHECK_FALSE((*runtime)->get_error().has_value());
    CHECK((*runtime)->model("openai-codex", "gpt-5.5").has_value());
    CHECK((*runtime)->model("kimi-coding", "kimi-for-coding").has_value());
    CHECK_FALSE((*runtime)->model("deepseek", "deepseek-v4-flash").has_value());

    const auto providers = (*runtime)->providers();
    REQUIRE(providers.size() == 2);
    const auto codex = (*runtime)->provider("openai-codex");
    REQUIRE(codex.has_value());
    CHECK(codex->id == "openai-codex");
    CHECK(codex->name == "OpenAI Codex");
    REQUIRE(codex->auth_methods.size() == 1);
    CHECK(codex->auth_methods.front().type == ai::AuthType::OAuth);
    CHECK(codex->auth_methods.front().has_login);
    CHECK_FALSE((*runtime)->provider("missing-provider").has_value());
}

TEST_CASE("ModelRuntime invalid models.json becomes empty user config plus diagnostics", "[coding_agent][model-runtime][issue345]") {
    tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR", std::nullopt};
    home_guard.set(home.path().string());
    home.write(".pi/agent/models.json", "{not valid json");

    auto runtime = coding_agent::ModelRuntime::create({});
    REQUIRE(runtime);
    // Built-ins still compose; the config diagnostic is exposed.
    CHECK((*runtime)->model("openai-codex", "gpt-5.5").has_value());
    REQUIRE((*runtime)->get_error().has_value());
    CHECK((*runtime)->get_error()->find("Failed to parse models.json") != std::string::npos);
}

TEST_CASE("ModelRuntime per-provider composition failure falls back to the built-in and records the error", "[coding_agent][model-runtime][issue345]") {
    tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR", std::nullopt};
    home_guard.set(home.path().string());
    home.write(".pi/agent/models.json", R"({
      "providers": {"kimi-coding": {"name": "Broken Kimi"}}
    })");

    auto runtime = coding_agent::ModelRuntime::create({});
    REQUIRE(runtime);
    // The broken overlay falls back to the built-in kimi provider.
    CHECK((*runtime)->model("kimi-coding", "kimi-for-coding").has_value());
    REQUIRE((*runtime)->get_error().has_value());
    CHECK((*runtime)->get_error()->find("kimi-coding") != std::string::npos);
    CHECK((*runtime)->get_error()->find("must specify") != std::string::npos);
}

TEST_CASE("ModelRuntime refresh reloads models.json and recomposes providers", "[coding_agent][model-runtime][issue345]") {
    tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR", std::nullopt};
    home_guard.set(home.path().string());

    auto runtime = coding_agent::ModelRuntime::create({});
    REQUIRE(runtime);
    CHECK_FALSE((*runtime)->model("deepseek", "deepseek-v4-flash").has_value());

    home.write(".pi/agent/models.json", R"({
      "providers": {
        "deepseek": {
          "baseUrl": "https://api.deepseek.example/v1",
          "api": "openai-responses",
          "apiKey": "dummy-deepseek-key",
          "models": [{"id": "deepseek-v4-flash"}]
        }
      }
    })");
    REQUIRE((*runtime)->refresh());
    const auto model = (*runtime)->model("deepseek", "deepseek-v4-flash");
    REQUIRE(model.has_value());
    CHECK(model->api == "openai-responses");
    CHECK(model->provider == "deepseek");
}

TEST_CASE("ModelRuntime config-only provider streams the frozen deepseek wire path", "[coding_agent][model-runtime][issue345][vertical]") {
    auto transport = std::make_shared<ScriptedTransport>();
    const auto sse = read_fixture_text("wire/openai-responses-deepseek.sse");
    REQUIRE_FALSE(sse.empty());
    const auto split = sse.size() / 2;
    transport->attempts.push_back(TransportAttempt{
        .chunks = {sse.substr(0, split), sse.substr(split)},
    });

    tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR", std::nullopt};
    home_guard.set(home.path().string());
    const auto models_json = read_fixture_text("models/models.json");
    REQUIRE_FALSE(models_json.empty());
    home.write(".pi/agent/models.json", models_json);

    auto runtime = coding_agent::create_model_runtime_for_testing(coding_agent::ModelRuntimeOptions{},
            coding_agent::ModelRuntimeTestOptions{
                    .transports =
                            ai::providers::ScriptedTransportOptions{
                                    .http_transport = transport,
                            },
            });
    REQUIRE(runtime);
    CHECK_FALSE((*runtime)->get_error().has_value());

    const auto model = (*runtime)->model("deepseek", "deepseek-v4-flash");
    REQUIRE(model.has_value());
    CHECK(model->reasoning == true);
    CHECK(model->input == std::vector<ai::ModelInput>{ai::ModelInput::Text});

    ai::SimpleStreamOptions options;
    options.temperature = 0.2;
    options.max_tokens = 123;
    options.reasoning = ai::ThinkingLevel::High;
    options.session_id = "session-1";
    options.cache_retention = ai::CacheRetention::Long;
    options.timeout_ms = 4321;

    std::vector<ai::AssistantStreamEvent> events;
    auto result = run_async_result(
        (*runtime)->ai_models()->stream(
            *model,
            request_context(),
            std::move(options)).run(
        [&events](const ai::AssistantStreamEvent& event) -> support::ExpectedVoid {
            events.push_back(event);
            return {};
        }));

    REQUIRE(result);
    CHECK(result->stop_reason == ai::AssistantStopReason::ToolUse);
    CHECK(result->response_id == "resp_deepseek");

    CHECK_FALSE(tests::pi_event_snapshot_mismatch(
        events,
        "wire/openai-responses-deepseek-ts-events.json"));

    REQUIRE(transport->requests.size() == 1);
    const auto& request = transport->requests.front();
    CHECK(request.url == "https://api.deepseek.example/v1/responses");
    CHECK(request.timeout == std::chrono::milliseconds{4321});
    CHECK(request.headers.at("Authorization") == "Bearer dummy-deepseek-key");
    CHECK(request.headers.at("session_id") == "session-1");
    CHECK(request.headers.at("x-client-request-id") == "session-1");

    auto expected_request_bytes = read_fixture_text(
        "wire/openai-responses-deepseek-ts-request.json");
    REQUIRE_FALSE(expected_request_bytes.empty());
    if (expected_request_bytes.back() == '\n') {
        expected_request_bytes.pop_back();
    }
    CHECK(request.body == expected_request_bytes);
}

TEST_CASE("ModelRuntime login persists the credential and refresh failures never fail the call", "[coding_agent][model-runtime][issue345]") {
    tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR", std::nullopt};
    home_guard.set(home.path().string());
    // Invalid models.json makes the post-login refresh fail, which must not
    // fail the login call.
    home.write(".pi/agent/models.json", "{not valid json");

    auto store = std::make_shared<MemoryCredentialStore>();
    auto runtime = coding_agent::ModelRuntime::create(coding_agent::ModelRuntimeOptions{
        .credentials = store,
    });
    REQUIRE(runtime);
    REQUIRE((*runtime)->ai_models()->set_provider(std::make_shared<LoginProvider>()));

    ai::AuthInteraction interaction;
    auto credential =
            run_async_result((*runtime)->login("login-provider", ai::AuthType::ApiKey, std::move(interaction)));
    REQUIRE(credential);
    REQUIRE(std::holds_alternative<ai::ApiKeyCredential>(*credential));
    CHECK(std::get<ai::ApiKeyCredential>(*credential).key == "dummy-login-key");
    // The credential was persisted.
    const auto stored = run_async_result(store->read("login-provider"));
    REQUIRE(stored);
    REQUIRE(stored->has_value());
    CHECK(std::get<ai::ApiKeyCredential>(**stored).key == "dummy-login-key");
    // The failed refresh is recorded in the composition-errors map.
    REQUIRE((*runtime)->get_error().has_value());
    CHECK((*runtime)->get_error()->find("Failed to parse models.json") != std::string::npos);
}

TEST_CASE("ModelRuntime logout removes the credential and recomposes", "[coding_agent][model-runtime][issue345]") {
    tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR", std::nullopt};
    home_guard.set(home.path().string());

    auto store = std::make_shared<MemoryCredentialStore>();
    store->records["login-provider"] = ai::ApiKeyCredential{.key = "dummy-login-key"};
    auto runtime = coding_agent::ModelRuntime::create(coding_agent::ModelRuntimeOptions{
        .credentials = store,
    });
    REQUIRE(runtime);
    REQUIRE((*runtime)->ai_models()->set_provider(std::make_shared<LoginProvider>()));

    REQUIRE(run_async_result((*runtime)->logout("login-provider")));
    const auto stored = run_async_result(store->read("login-provider"));
    REQUIRE(stored);
    CHECK_FALSE(stored->has_value());
}

TEST_CASE("ModelRuntime availability reflects configured providers", "[coding_agent][model-runtime][issue345]") {
    tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR", std::nullopt};
    tests::EnvVarGuard kimi_key{"KIMI_API_KEY"};
    home_guard.set(home.path().string());
    kimi_key.unset();

    auto runtime = coding_agent::ModelRuntime::create({});
    REQUIRE(runtime);
    const auto available = run_async_result((*runtime)->get_available());
    REQUIRE(available);
    // With no stored credentials and no ambient env keys, the OAuth-only
    // openai-codex provider and the env api-key kimi-coding provider are not
    // configured; no models are available.
    CHECK(available->empty());
    CHECK_FALSE((*runtime)->has_configured_auth("openai-codex"));
    CHECK((*runtime)->get_available_snapshot().empty());
}

TEST_CASE("ModelRuntime default-model table selects the runtime default", "[coding_agent][model-runtime][issue345]") {
    tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR", std::nullopt};
    home_guard.set(home.path().string());
    home.write(".pi/agent/models.json", R"({
      "providers": {
        "deepseek": {
          "baseUrl": "https://api.deepseek.example/v1",
          "api": "openai-responses",
          "apiKey": "dummy-deepseek-key",
          "models": [{"id": "deepseek-v4-flash"}]
        }
      }
    })");

    auto runtime = coding_agent::ModelRuntime::create({});
    REQUIRE(runtime);
    CHECK(coding_agent::ModelRuntime::default_model_for_provider("openai-codex") == std::optional<std::string>{"gpt-5.5"});
    CHECK(coding_agent::ModelRuntime::default_model_for_provider("kimi-coding") == std::optional<std::string>{"kimi-for-coding"});
    CHECK_FALSE(coding_agent::ModelRuntime::default_model_for_provider("deepseek").has_value());
}

TEST_CASE("ModelRuntime configured apiKey env templates surface for secret filtering", "[coding_agent][model-runtime][issue345]") {
    tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR", std::nullopt};
    home_guard.set(home.path().string());
    home.write(".pi/agent/models.json", R"({
      "providers": {
        "deepseek": {
          "baseUrl": "https://api.deepseek.example/v1",
          "api": "openai-responses",
          "apiKey": "$DEEPSEEK_SECRET",
          "models": [{"id": "deepseek-v4-flash"}]
        }
      }
    })");

    auto runtime = coding_agent::ModelRuntime::create({});
    REQUIRE(runtime);
    const auto names = (*runtime)->configured_api_key_env_names();
    REQUIRE(names.size() == 1);
    CHECK(names.front() == "DEEPSEEK_SECRET");
}

TEST_CASE("ModelRuntime env-template apiKey resolves at request time", "[coding_agent][model-runtime][issue345]") {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->attempts.push_back(TransportAttempt{
        .chunks = {"data: {\"type\":\"response.completed\",\"response\":{\"id\":\"r\",\"status\":\"completed\"}}\n\n"},
    });
    tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR", std::nullopt};
    tests::EnvVarGuard secret_guard{"DEEPSEEK_SECRET"};
    home_guard.set(home.path().string());
    secret_guard.set("dummy-env-key");
    home.write(".pi/agent/models.json", R"({
      "providers": {
        "deepseek": {
          "baseUrl": "https://api.deepseek.example/v1",
          "api": "openai-responses",
          "apiKey": "$DEEPSEEK_SECRET",
          "models": [{"id": "deepseek-v4-flash"}]
        }
      }
    })");

    auto runtime = coding_agent::create_model_runtime_for_testing(coding_agent::ModelRuntimeOptions{},
            coding_agent::ModelRuntimeTestOptions{
                    .transports =
                            ai::providers::ScriptedTransportOptions{
                                    .http_transport = transport,
                            },
            });
    REQUIRE(runtime);
    const auto model = (*runtime)->model("deepseek", "deepseek-v4-flash");
    REQUIRE(model.has_value());

    ai::SimpleStreamOptions options;
    options.max_tokens = 16;
    auto result = run_async_result(
        (*runtime)->ai_models()->stream(
            *model, {}, std::move(options)).run(
        [](const ai::AssistantStreamEvent&) { return support::ExpectedVoid{}; }));
    REQUIRE(result);
    REQUIRE(transport->requests.size() == 1);
    CHECK(transport->requests.front().headers.at("Authorization") == "Bearer dummy-env-key");
}

TEST_CASE("ModelRuntime resolves the pi 4-level auth precedence chain", "[coding_agent][model-runtime][issue346][precedence]") {
    tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR", std::nullopt};
    tests::EnvVarGuard secret{"DEEPSEEK_SECRET"};
    tests::EnvVarGuard kimi{"KIMI_API_KEY"};
    home_guard.set(home.path().string());
    home.write(".pi/agent/models.json", R"({
      "providers": {
        "deepseek": {
          "baseUrl": "https://api.deepseek.example/v1",
          "api": "openai-responses",
          "apiKey": "$DEEPSEEK_SECRET",
          "models": [{"id": "deepseek-v4-flash"}]
        }
      }
    })");

    auto storage = std::make_shared<coding_agent::AuthStorage>(
        home.path() / ".pi" / "agent" / "auth.json");
    auto runtime = coding_agent::ModelRuntime::create(
        coding_agent::ModelRuntimeOptions{.credentials = storage});
    REQUIRE(runtime);

    const auto get_auth_for = [&](std::string provider_id) {
        auto auth = run_async_result((*runtime)->get_auth(std::move(provider_id)));
        REQUIRE(auth);
        return std::move(*auth);
    };
    const auto store_key = [&](std::string provider_id, std::string key) {
        auto stored = run_async_result(storage->modify(
            std::move(provider_id),
            [key = std::move(key)](std::optional<ai::Credential>)
                -> cch::support::AsyncResult<std::optional<ai::Credential>> {
                return cch::support::AsyncResult<std::optional<ai::Credential>>(
                    std::expected<std::optional<ai::Credential>, cch::support::Error>{
                        std::optional<ai::Credential>{
                            ai::ApiKeyCredential{.key = key}}});
            }));
        REQUIRE(stored);
    };

    // Level 4 (models.json configured key): an env-template apiKey is
    // unconfigured until its environment variable is present.
    secret.unset();
    auto unconfigured = run_async_result((*runtime)->check_auth("deepseek"));
    REQUIRE(unconfigured);
    CHECK_FALSE(*unconfigured);

    secret.set("env-configured-key");
    auto configured = get_auth_for("deepseek");
    REQUIRE(configured);
    CHECK(configured->auth.api_key == "env-configured-key");
    CHECK(configured->source == "configured API key");

    // Level 3 (environment): the built-in kimi provider resolves its ambient
    // KIMI_API_KEY chain when nothing is stored.
    kimi.set("kimi-env-key");
    auto kimi_env = get_auth_for("kimi-coding");
    REQUIRE(kimi_env);
    CHECK(kimi_env->auth.api_key == "kimi-env-key");

    // Level 2 (stored auth.json credential) beats env and configured keys.
    store_key("deepseek", "stored-key");
    store_key("kimi-coding", "stored-kimi-key");
    auto deepseek_stored = get_auth_for("deepseek");
    REQUIRE(deepseek_stored);
    CHECK(deepseek_stored->auth.api_key == "stored-key");
    auto kimi_stored = get_auth_for("kimi-coding");
    REQUIRE(kimi_stored);
    CHECK(kimi_stored->auth.api_key == "stored-kimi-key");

    // Level 1 (runtime API key override, in-memory) beats the stored credential
    // and never persists.
    REQUIRE((*runtime)->set_runtime_api_key("deepseek", "runtime-key"));
    CHECK((*runtime)->has_runtime_api_key("deepseek"));
    CHECK((*runtime)->has_configured_auth("deepseek"));
    auto deepseek_runtime = get_auth_for("deepseek");
    REQUIRE(deepseek_runtime);
    CHECK(deepseek_runtime->auth.api_key == "runtime-key");
    auto status = (*runtime)->get_provider_auth_status("deepseek");
    REQUIRE(status);
    CHECK(status->configured);
    CHECK(status->source == "runtime");

    // list_credentials stays metadata-only but reports the runtime override.
    auto listed = run_async_result((*runtime)->list_credentials());
    REQUIRE(listed);
    const auto deepseek_entry = std::find_if(
        listed->begin(), listed->end(), [](const ai::CredentialInfo& entry) {
            return entry.provider_id == "deepseek";
        });
    REQUIRE(deepseek_entry != listed->end());
    CHECK(deepseek_entry->type == "api_key");
    CHECK(deepseek_entry->provider_id == "deepseek");

    REQUIRE((*runtime)->remove_runtime_api_key("deepseek"));
    CHECK_FALSE((*runtime)->has_runtime_api_key("deepseek"));
    auto deepseek_restored = get_auth_for("deepseek");
    REQUIRE(deepseek_restored);
    CHECK(deepseek_restored->auth.api_key == "stored-key");
}

TEST_CASE("ModelRuntime !command apiKey resolves through the shell with a process-lifetime cache", "[coding_agent][model-runtime][issue345]") {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->attempts.push_back(TransportAttempt{
        .chunks = {"data: {\"type\":\"response.completed\",\"response\":{\"id\":\"r\",\"status\":\"completed\"}}\n\n"},
    });
    tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR", std::nullopt};
    home_guard.set(home.path().string());
    home.write(".pi/agent/models.json", R"({
      "providers": {
        "deepseek": {
          "baseUrl": "https://api.deepseek.example/v1",
          "api": "openai-responses",
          "apiKey": "!printf dummy-command-key",
          "models": [{"id": "deepseek-v4-flash"}]
        }
      }
    })");

    auto runtime = coding_agent::create_model_runtime_for_testing(coding_agent::ModelRuntimeOptions{},
            coding_agent::ModelRuntimeTestOptions{
                    .transports =
                            ai::providers::ScriptedTransportOptions{
                                    .http_transport = transport,
                            },
            });
    REQUIRE(runtime);
    const auto model = (*runtime)->model("deepseek", "deepseek-v4-flash");
    REQUIRE(model.has_value());

    ai::SimpleStreamOptions options;
    options.max_tokens = 16;
    auto result = run_async_result(
        (*runtime)->ai_models()->stream(
            *model, {}, std::move(options)).run(
        [](const ai::AssistantStreamEvent&) { return support::ExpectedVoid{}; }));
    REQUIRE(result);
    REQUIRE(transport->requests.size() == 1);
    CHECK(transport->requests.front().headers.at("Authorization") == "Bearer dummy-command-key");
}

TEST_CASE("ModelRuntime auth status reports an unconfigured builtin as not configured", "[coding_agent][model-runtime][issue406]") {
    tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR", std::nullopt};
    home_guard.set(home.path().string());
    tests::EnvVarGuard kimi_guard{"KIMI_API_KEY"};
    kimi_guard.unset();

    auto runtime = coding_agent::ModelRuntime::create({});
    REQUIRE(runtime);
    static_cast<void>(run_async_result((*runtime)->get_available()));
    auto status = (*runtime)->get_provider_auth_status("openai-codex");
    REQUIRE(status.has_value());
    // pi `getProviderAuthStatus`: no runtime key, no stored credential, no
    // config, no environment → `{ configured: false }` (the selector renders
    // "• unconfigured"); structural auth hooks alone are not a source.
    CHECK_FALSE(status->configured);

    auto missing = (*runtime)->get_provider_auth_status("no-such-provider");
    CHECK_FALSE(missing.has_value());
}
