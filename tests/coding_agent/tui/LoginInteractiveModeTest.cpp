// The interactive login/logout presentation end to end (pi
// `interactive-mode.ts` login flows, #328; G2 decision 4): the auth-type
// picker and provider selector, the Codex/Kimi OAuth dialog branches, the
// DeepSeek API-key dialog branch through real models.json composition,
// post-login default-model auto-selection with the four verbatim
// selection-error messages, Login Cancellation suppression on the stable
// cancelled kind, and the re-auth guidance through the chat surface. Driven
// through the VirtualTerminal seam with scripted providers and temp
// credential state — no live network, no real credentials.

#include "ai/ModelStreamBridge.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "coding_agent/tui/TestTuiActionSink.hpp"

#include <cch/ai/Auth.hpp>
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include <cch/coding_agent/Settings.hpp>
#include <cch/agent/harness/session/SessionStore.hpp>
#include <cch/tui/VirtualTerminal.hpp>
#include "support/EnvVarGuard.hpp"
#include "support/ModelsFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace cch;

namespace {

void drain_ready(boost::asio::io_context& io) {
    if (io.stopped()) io.restart();
    while (io.poll() != 0) {
    }
    // The credential store persists on its own worker thread (AuthStorage,
    // ADR 0040 / #454); yield and re-poll so a completion posted from that
    // thread is processed before the caller's next assertion observes the
    // screen.
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
    while (io.poll() != 0) {
    }
}

[[nodiscard]] std::string visible_screen(const tui::VirtualTerminal& terminal) {
    std::string text;
    for (const auto& line : terminal.screen()) {
        text.append(line);
        text.push_back('\n');
    }
    return text;
}

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

/// One scripted OAuth provider: fixed identity/models, an OAuth login hook
/// that plays a caller-supplied script against the Auth Interaction, and a
/// fixed to_auth projection. `check_auth` needs no hook for a stored OAuth
/// credential (`Models` derives it from the store), matching production.
class ScriptedOAuthProvider final : public ai::Provider {
public:
    using LoginScript = std::move_only_function<
        boost::asio::awaitable<support::Expected<ai::OAuthCredential>>(ai::AuthInteraction)>;
    using RefreshScript = std::move_only_function<
        boost::asio::awaitable<support::Expected<ai::OAuthCredential>>(ai::OAuthCredential)>;

    ScriptedOAuthProvider(
        std::string provider_id,
        std::string provider_name,
        std::vector<ai::Model> models,
        LoginScript login_script,
        RefreshScript refresh_script = {})
        : provider_id_(std::move(provider_id)),
          provider_name_(std::move(provider_name)),
          models_(std::move(models)) {
        ai::OAuthAuth oauth;
        oauth.name = provider_name_ + " OAuth";
        oauth.login = [script = std::make_shared<LoginScript>(std::move(login_script))](
                          ai::AuthInteraction interaction)
            -> cch::support::AsyncResult<ai::OAuthCredential> {
            return cch::ai::detail::make_async_result(
                [script, interaction = std::move(interaction)]() mutable
                    -> boost::asio::awaitable<support::Expected<ai::OAuthCredential>> {
                    co_return co_await (*script)(std::move(interaction));
                });
        };
        if (refresh_script) {
            oauth.refresh = [script = std::make_shared<RefreshScript>(std::move(refresh_script))](
                                ai::OAuthCredential credential)
                -> cch::support::AsyncResult<ai::OAuthCredential> {
                return cch::ai::detail::make_async_result(
                    [script, credential = std::move(credential)]() mutable
                        -> boost::asio::awaitable<support::Expected<ai::OAuthCredential>> {
                        co_return co_await (*script)(std::move(credential));
                    });
            };
        } else {
            oauth.refresh = [](ai::OAuthCredential credential)
                -> cch::support::AsyncResult<ai::OAuthCredential> {
                return cch::support::AsyncResult<ai::OAuthCredential>(
                    std::expected<ai::OAuthCredential, cch::support::Error>{credential});
            };
        }
        oauth.to_auth = [](const ai::OAuthCredential& credential)
            -> cch::support::AsyncResult<ai::ModelAuth> {
            return cch::support::AsyncResult<ai::ModelAuth>(
                std::expected<ai::ModelAuth, cch::support::Error>{
                    ai::ModelAuth{.api_key = credential.access}});
        };
        auth_.oauth = std::move(oauth);
    }

    /// Install an environment-only api-key method without a login hook (pi's
    /// ambient auth: configured outside the binary).
    void install_ambient_api_key(std::string method_name) {
        ai::ApiKeyAuth api_key;
        api_key.name = std::move(method_name);
        api_key.check = [](const ai::AuthContext&, std::optional<ai::ApiKeyCredential>)
            -> cch::support::AsyncResult<std::optional<ai::AuthCheck>> {
            return cch::support::AsyncResult<std::optional<ai::AuthCheck>>(
                std::expected<std::optional<ai::AuthCheck>, cch::support::Error>{
                    std::optional<ai::AuthCheck>{}});
        };
        api_key.resolve = [](const ai::AuthContext&, std::optional<ai::ApiKeyCredential>)
            -> cch::support::AsyncResult<std::optional<ai::AuthResult>> {
            return cch::support::AsyncResult<std::optional<ai::AuthResult>>(
                std::expected<std::optional<ai::AuthResult>, cch::support::Error>{
                    std::optional<ai::AuthResult>{}});
        };
        auth_.api_key = std::move(api_key);
    }

    [[nodiscard]] std::string_view id() const noexcept override { return provider_id_; }
    [[nodiscard]] std::string_view name() const noexcept override { return provider_name_; }
    [[nodiscard]] ai::ProviderAuth& auth() noexcept override { return auth_; }
    [[nodiscard]] std::vector<ai::Model> models() const override { return models_; }

    [[nodiscard]] ai::ModelStream stream(
        ai::Model,
        ai::AiContext,
        ai::ProviderStreamOptions) override {
        return ai::detail::make_model_stream(
            [this](
                ai::AssistantEventSink) mutable
                -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Provider, "scripted provider does not stream"));
                });
    }


private:
    std::string provider_id_;
    std::string provider_name_;
    std::vector<ai::Model> models_;
    ai::ProviderAuth auth_;
};

[[nodiscard]] ai::OAuthCredential dummy_oauth_credential() {
    ai::OAuthCredential credential;
    credential.refresh = "dummy-refresh";
    credential.access = "dummy-access";
    credential.expires = 9'999'999'999'999;
    return credential;
}

/// One isolated login fixture: a temp Agent Config Directory shared by the
/// runtime's credential store and the interactive settings scope, plus a temp
/// workspace. `PI_CODING_AGENT_DIR` isolates the runtime's agent dir; `HOME`
/// isolates ambient user state.
struct LoginFixture {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard dir_guard{"PI_CODING_AGENT_DIR"};
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard kimi_guard{"KIMI_API_KEY"};

    LoginFixture() {
        dir_guard.set(agent_dir.path().string());
        home_guard.set(workspace.path().string());
        kimi_guard.unset();
    }

    /// Create a real ModelRuntime over the fixture's agent dir (file-backed
    /// credential store, real composition and resolution chain), replacing
    /// the given providers with scripted ones through the native-provider
    /// registration seam.
    [[nodiscard]] std::shared_ptr<coding_agent::ModelRuntime> create_runtime(
        std::vector<std::shared_ptr<ai::Provider>> replacements = {}) {
        auto runtime = coding_agent::ModelRuntime::create(coding_agent::ModelRuntimeOptions{
            .agent_dir = agent_dir.path(),
        });
        if (!runtime) return nullptr;
        for (auto& provider : replacements) {
            if (auto added = (*runtime)->register_native_provider(std::move(provider));
                !added) {
                return nullptr;
            }
        }
        return *runtime;
    }

    [[nodiscard]] std::filesystem::path auth_path() const {
        return agent_dir.path() / "auth.json";
    }

    /// Create a session on the injected runtime (the private E2E seam), so
    /// the real resolution chain lands on the unknown placeholder while no
    /// provider has configured auth — pi's `isUnknownModel` boot state.
    [[nodiscard]] support::Expected<coding_agent::CreateAgentSessionResult> create_session(
        std::shared_ptr<coding_agent::ModelRuntime> runtime) {
        coding_agent::runtime::AgentSessionCreationRequest request;
        request.session_facts.no_skills = true;
        request.session_facts.no_prompt_templates = true;
        request.workspace = workspace.path();
        request.session_target = coding_agent::InMemorySessionTarget{};
        request.model_runtime = std::move(runtime);
        return coding_agent::create_agent_session(std::move(request));
    }
};

struct InteractiveRun {
    // Wide enough that status lines never wrap mid-assertion.
    tui::VirtualTerminal terminal{{.columns = 220, .rows = 40}};
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    std::vector<std::string> opened_urls;

    void start(
        coding_agent::AgentSession& session,
        const std::filesystem::path& agent_config_directory) {
        auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
            .with_session(session)
            .with_agent_config_directory(agent_config_directory)
            .with_action_sink([this](
                                  std::size_t /* action_generation */,
                                  coding_agent::tui::TuiActionVariant action)
                              -> support::Expected<coding_agent::tui::TuiActionResultVariant> {
                if (const auto* open =
                        std::get_if<coding_agent::tui::OpenBrowserAction>(&action)) {
                    opened_urls.push_back(open->url);
                }
                return coding_agent::tui::TuiActionResultVariant{std::monostate{}};
            })
            .build();

        boost::asio::co_spawn(
            io,
            coding_agent::tui::run_interactive_mode(
                terminal,
                std::move(run)),
            [this](std::exception_ptr exception, support::ExpectedVoid result) {
                CHECK(exception == nullptr);
                run_result.emplace(std::move(result));
            });
        drain_ready(io);
    }

    void type(std::string_view text) {
        REQUIRE(terminal.inject_input(std::string{text}));
        drain_ready(io);
    }

    void exit() {
        type("\x04");
        // The credential store persists on its own worker thread (AuthStorage,
        // ADR 0040 / #454); poll the io_context and yield until the interactive
        // mode reaches its terminal outcome so a background completion can be
        // posted back and processed deterministically.
        for (int attempt = 0; attempt < 2000 && !run_result; ++attempt) {
            drain_ready(io);
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        REQUIRE(run_result);
        CHECK(*run_result);
    }
};

/// The codex login script: auth URL, then the manual-code prompt; the
/// submitted code is recorded and a dummy credential returned.
[[nodiscard]] ScriptedOAuthProvider::LoginScript codex_url_then_manual_code(
    std::shared_ptr<std::optional<std::string>> submitted_code) {
    return [submitted_code](ai::AuthInteraction interaction)
        -> boost::asio::awaitable<support::Expected<ai::OAuthCredential>> {
        interaction.notify(ai::AuthEvent{ai::AuthUrl{
            .url = "https://auth.openai.example/authorize?client=abc",
            .instructions = "Complete sign-in in your browser.",
        }});
        auto code = co_await cch::ai::detail::await_async_result(interaction.prompt(ai::AuthPrompt{
            .kind = ai::AuthPromptManualCode{.message = "Paste the authorization code"},
            .stop_token = std::nullopt,
        }));
        if (!code) co_return std::unexpected(std::move(code.error()));
        *submitted_code = *code;
        co_return dummy_oauth_credential();
    };
}

} // namespace

TEST_CASE(
    "login picks the auth type, provider, runs the Codex OAuth branch, and auto-selects the default model",
    "[coding_agent][tui][login][issue406]") {
    LoginFixture fixture;
    auto submitted_code = std::make_shared<std::optional<std::string>>();
    auto codex = std::make_shared<ScriptedOAuthProvider>(
        "openai-codex",
        "OpenAI Codex",
        std::vector<ai::Model>{tests::scripted_request_model("openai-codex", "gpt-5.5")},
        codex_url_then_manual_code(submitted_code));
    auto kimi = std::make_shared<ScriptedOAuthProvider>(
        "kimi-coding",
        "Kimi For Coding",
        std::vector<ai::Model>{tests::scripted_request_model("kimi-coding", "kimi-for-coding")},
        [](ai::AuthInteraction) -> boost::asio::awaitable<support::Expected<ai::OAuthCredential>> {
            co_return dummy_oauth_credential();
        });
    kimi->install_ambient_api_key("Kimi API key");
    auto runtime = fixture.create_runtime({codex, kimi});
    REQUIRE(runtime != nullptr);
    auto session = fixture.create_session(std::move(runtime));
    REQUIRE(session);

    InteractiveRun run;
    run.start(*session->session, fixture.agent_dir.path());

    // /login with no reference opens the auth-type picker (pi's generic
    // string-list selector).
    run.type("/login\r");
    auto screen = visible_screen(run.terminal);
    CHECK(screen.find("Select authentication method:") != std::string::npos);
    CHECK(screen.find("Sign in with an account") != std::string::npos);
    CHECK(screen.find("Sign in with an API key") != std::string::npos);

    // The OAuth branch opens the provider selector with auth-status labels.
    run.type("\r");
    screen = visible_screen(run.terminal);
    CHECK(screen.find("Select provider to configure:") != std::string::npos);
    CHECK(screen.find("OpenAI Codex") != std::string::npos);
    CHECK(screen.find("Kimi For Coding") != std::string::npos);
    CHECK(screen.find("unconfigured") != std::string::npos);

    // Kimi sorts before OpenAI; move to OpenAI Codex and confirm.
    run.type("\x1b[B");
    run.type("\r");
    screen = visible_screen(run.terminal);
    CHECK(screen.find("Login to OpenAI Codex") != std::string::npos);
    CHECK(screen.find("https://auth.openai.example/authorize?client=abc") != std::string::npos);
    CHECK(screen.find("Complete sign-in in your browser.") != std::string::npos);
    CHECK(screen.find("Paste the authorization code") != std::string::npos);
    // pi opens the browser best-effort with the presented URL.
    REQUIRE(run.opened_urls.size() == 1);
    CHECK(run.opened_urls[0] == "https://auth.openai.example/authorize?client=abc");

    // Submit the manual code; the login completes and the post-login
    // auto-selection picks the provider's frozen default model.
    run.type("dummy-code\r");
    CHECK(submitted_code->has_value());
    CHECK(*submitted_code == std::optional<std::string>{"dummy-code"});
    screen = visible_screen(run.terminal);
    const std::string expected_status =
        "Logged in to OpenAI Codex. Selected gpt-5.5. Credentials saved to " +
        fixture.auth_path().string();
    CHECK(screen.find(expected_status) != std::string::npos);
    CHECK(screen.find("Failed to login") == std::string::npos);

    // The session's live model switched (pi `session.setModel`).
    const auto snapshot = session->session->snapshot();
    CHECK(snapshot.agent_state.model.provider == "openai-codex");
    CHECK(snapshot.agent_state.model.id == "gpt-5.5");

    // The credential persisted through the store, and the global settings
    // default was written.
    const auto auth_text = read_text(fixture.auth_path());
    CHECK(auth_text.find("dummy-refresh") != std::string::npos);
    const auto settings_text = read_text(fixture.agent_dir.path() / "settings.json");
    CHECK(settings_text.find("\"defaultProvider\": \"openai-codex\"") != std::string::npos);
    CHECK(settings_text.find("\"defaultModel\": \"gpt-5.5\"") != std::string::npos);

    run.exit();
}

TEST_CASE(
    "login runs the Kimi device-code OAuth branch and renders the waiting view",
    "[coding_agent][tui][login][issue406]") {
    LoginFixture fixture;
    auto kimi = std::make_shared<ScriptedOAuthProvider>(
        "kimi-coding",
        "Kimi For Coding",
        std::vector<ai::Model>{tests::scripted_request_model("kimi-coding", "kimi-for-coding")},
        [](ai::AuthInteraction interaction)
            -> boost::asio::awaitable<support::Expected<ai::OAuthCredential>> {
            interaction.notify(ai::AuthEvent{ai::AuthDeviceCode{
                .user_code = "ABCD-EFGH",
                .verification_uri = "https://kimi.example/device",
            }});
            auto acknowledged = co_await cch::ai::detail::await_async_result(interaction.prompt(ai::AuthPrompt{
                .kind = ai::AuthPromptText{.message = "Press enter after approving"},
                .stop_token = std::nullopt,
            }));
            if (!acknowledged) co_return std::unexpected(std::move(acknowledged.error()));
            co_return dummy_oauth_credential();
        });
    auto runtime = fixture.create_runtime({kimi});
    REQUIRE(runtime != nullptr);
    auto session = fixture.create_session(std::move(runtime));
    REQUIRE(session);

    InteractiveRun run;
    run.start(*session->session, fixture.agent_dir.path());

    // A single (provider, auth-type) match skips both pickers (pi
    // startProviderLogin direct branch).
    run.type("/login kimi-coding\r");
    auto screen = visible_screen(run.terminal);
    CHECK(screen.find("Select authentication method") == std::string::npos);
    CHECK(screen.find("Login to Kimi For Coding") != std::string::npos);
    CHECK(screen.find("https://kimi.example/device") != std::string::npos);
    CHECK(screen.find("Enter code: ABCD-EFGH") != std::string::npos);
    CHECK(screen.find("Waiting for authentication...") != std::string::npos);

    run.type("\r");
    screen = visible_screen(run.terminal);
    const std::string expected_status =
        "Logged in to Kimi For Coding. Selected kimi-for-coding. Credentials saved to " +
        fixture.auth_path().string();
    CHECK(screen.find(expected_status) != std::string::npos);

    run.exit();
}

TEST_CASE(
    "login takes the DeepSeek API-key dialog branch through real models.json composition",
    "[coding_agent][tui][login][issue406]") {
    LoginFixture fixture;
    std::ofstream(fixture.agent_dir.path() / "models.json", std::ios::binary) << R"({
  "providers": {
    "deepseek": {
      "name": "DeepSeek",
      "baseUrl": "https://api.deepseek.example/v1",
      "api": "openai-responses",
      "apiKey": "$DEEPSEEK_API_KEY",
      "models": [{"id": "deepseek-v4-flash"}]
    }
  }
})";

    // The real creation path composes the config-only DeepSeek provider: its
    // composed api-key method carries pi's generic "Enter API key" login.
    auto runtime = fixture.create_runtime();
    REQUIRE(runtime != nullptr);
    auto session = fixture.create_session(std::move(runtime));
    REQUIRE(session);

    InteractiveRun run;
    run.start(*session->session, fixture.agent_dir.path());

    run.type("/login deepseek\r");
    auto screen = visible_screen(run.terminal);
    CHECK(screen.find("Login to DeepSeek") != std::string::npos);
    CHECK(screen.find("Enter API key") != std::string::npos);

    run.type("dummy-deepseek-key\r");
    screen = visible_screen(run.terminal);
    // The C++ frozen default-model table has no deepseek entry: pi's first
    // verbatim selection error follows the success status.
    const std::string expected_status =
        "Saved API key for DeepSeek. Credentials saved to " + fixture.auth_path().string();
    CHECK(screen.find(expected_status) != std::string::npos);
    CHECK(screen.find(
        "Saved API key for DeepSeek, but no default model is configured for provider "
        "\"deepseek\". Use /model to select a model.") != std::string::npos);

    // The key persisted as an api_key credential through the composed store.
    const auto auth_text = read_text(fixture.auth_path());
    CHECK(auth_text.find("\"deepseek\"") != std::string::npos);
    CHECK(auth_text.find("dummy-deepseek-key") != std::string::npos);

    run.exit();
}

TEST_CASE(
    "login cancellation suppresses the failure UI on the stable cancelled kind",
    "[coding_agent][tui][login][issue406]") {
    LoginFixture fixture;
    auto codex = std::make_shared<ScriptedOAuthProvider>(
        "openai-codex",
        "OpenAI Codex",
        std::vector<ai::Model>{tests::scripted_request_model("openai-codex", "gpt-5.5")},
        codex_url_then_manual_code(std::make_shared<std::optional<std::string>>()));
    auto runtime = fixture.create_runtime({codex});
    REQUIRE(runtime != nullptr);
    auto session = fixture.create_session(std::move(runtime));
    REQUIRE(session);

    InteractiveRun run;
    run.start(*session->session, fixture.agent_dir.path());

    run.type("/login openai-codex\r");
    REQUIRE(visible_screen(run.terminal).find("Paste the authorization code") !=
        std::string::npos);

    // Esc/Ctrl+C cancels through the Auth Interaction stop source: the
    // prompt rejects with the stable cancelled kind and no failure UI
    // renders.
    run.type("\x03");
    const auto screen = visible_screen(run.terminal);
    CHECK(screen.find("Failed to login") == std::string::npos);
    CHECK(screen.find("Login cancelled") == std::string::npos);

    // The editor is restored and still accepts ordinary input.
    run.type("hello after cancel");
    CHECK(visible_screen(run.terminal).find("hello after cancel") != std::string::npos);

    // pi: app.exit (Ctrl+D) exits only when the editor is empty; clear it
    // first (app.clear is Ctrl+C in the main editor), then exit.
    run.type("\x03");
    run.exit();
}

TEST_CASE(
    "login failure renders pi's failure message through the chat surface",
    "[coding_agent][tui][login][issue406]") {
    LoginFixture fixture;
    auto codex = std::make_shared<ScriptedOAuthProvider>(
        "openai-codex",
        "OpenAI Codex",
        std::vector<ai::Model>{tests::scripted_request_model("openai-codex", "gpt-5.5")},
        [](ai::AuthInteraction) -> boost::asio::awaitable<support::Expected<ai::OAuthCredential>> {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::OAuth,
                "OpenAI Codex token exchange failed (400): invalid grant"));
        });
    auto runtime = fixture.create_runtime({codex});
    REQUIRE(runtime != nullptr);
    auto session = fixture.create_session(std::move(runtime));
    REQUIRE(session);

    InteractiveRun run;
    run.start(*session->session, fixture.agent_dir.path());

    run.type("/login openai-codex\r");
    const auto screen = visible_screen(run.terminal);
    CHECK(screen.find(
        "Error: Failed to login to OpenAI Codex: OpenAI Codex token exchange failed (400): "
        "invalid grant") != std::string::npos);
    // No credentials were persisted by the failed flow: the store's eager
    // auth.json initialization (pi's ensureFileExists at first storage
    // access) may create an empty file, but it must not contain the
    // provider's credential.
    if (std::filesystem::exists(fixture.auth_path())) {
        const auto auth_text = read_text(fixture.auth_path());
        CHECK(auth_text.find("openai-codex") == std::string::npos);
        CHECK(auth_text.find("dummy-refresh") == std::string::npos);
    }

    run.exit();
}

TEST_CASE(
    "login with an unmatched provider reference opens the searched provider selector",
    "[coding_agent][tui][login][issue406]") {
    LoginFixture fixture;
    auto codex = std::make_shared<ScriptedOAuthProvider>(
        "openai-codex",
        "OpenAI Codex",
        std::vector<ai::Model>{tests::scripted_request_model("openai-codex", "gpt-5.5")},
        codex_url_then_manual_code(std::make_shared<std::optional<std::string>>()));
    auto runtime = fixture.create_runtime({codex});
    REQUIRE(runtime != nullptr);
    auto session = fixture.create_session(std::move(runtime));
    REQUIRE(session);

    InteractiveRun run;
    run.start(*session->session, fixture.agent_dir.path());

    run.type("/login no-such-provider\r");
    const auto screen = visible_screen(run.terminal);
    // pi falls through to the provider selector with the reference as the
    // initial search; nothing matches.
    CHECK(screen.find("Select provider to configure:") != std::string::npos);
    CHECK(screen.find("No matching providers") != std::string::npos);

    // The selector owns the editor slot; cancel it before exiting.
    run.type("\x03");
    run.exit();
}

TEST_CASE(
    "login select-type AuthPrompt resolves through the generic string-list selector",
    "[coding_agent][tui][login][issue406]") {
    LoginFixture fixture;
    auto selected_id = std::make_shared<std::optional<std::string>>();
    auto codex = std::make_shared<ScriptedOAuthProvider>(
        "openai-codex",
        "OpenAI Codex",
        std::vector<ai::Model>{tests::scripted_request_model("openai-codex", "gpt-5.5")},
        [selected_id](ai::AuthInteraction interaction)
            -> boost::asio::awaitable<support::Expected<ai::OAuthCredential>> {
            ai::AuthPromptSelect select;
            select.message = "Choose sign-in method";
            select.options = {
                {.id = "account", .label = "Work account"},
                {.id = "device", .label = "Device code"},
            };
            auto selected = co_await cch::ai::detail::await_async_result(interaction.prompt(ai::AuthPrompt{
                .kind = std::move(select),
                .stop_token = std::nullopt,
            }));
            if (!selected) co_return std::unexpected(std::move(selected.error()));
            *selected_id = *selected;
            co_return dummy_oauth_credential();
        });
    auto runtime = fixture.create_runtime({codex});
    REQUIRE(runtime != nullptr);
    auto session = fixture.create_session(std::move(runtime));
    REQUIRE(session);

    InteractiveRun run;
    run.start(*session->session, fixture.agent_dir.path());

    run.type("/login openai-codex\r");
    auto screen = visible_screen(run.terminal);
    CHECK(screen.find("Choose sign-in method") != std::string::npos);
    CHECK(screen.find("Work account") != std::string::npos);
    CHECK(screen.find("Device code") != std::string::npos);

    run.type("\x1b[B");
    run.type("\r");
    REQUIRE(selected_id->has_value());
    CHECK(*selected_id == std::optional<std::string>{"device"});

    run.exit();
}

TEST_CASE(
    "login api-key ambient method shows the configured-outside info dialog",
    "[coding_agent][tui][login][issue406]") {
    LoginFixture fixture;
    auto kimi = std::make_shared<ScriptedOAuthProvider>(
        "kimi-coding",
        "Kimi For Coding",
        std::vector<ai::Model>{tests::scripted_request_model("kimi-coding", "kimi-for-coding")},
        [](ai::AuthInteraction) -> boost::asio::awaitable<support::Expected<ai::OAuthCredential>> {
            co_return dummy_oauth_credential();
        });
    kimi->install_ambient_api_key("Kimi API key");
    auto runtime = fixture.create_runtime({kimi});
    REQUIRE(runtime != nullptr);
    auto session = fixture.create_session(std::move(runtime));
    REQUIRE(session);

    InteractiveRun run;
    run.start(*session->session, fixture.agent_dir.path());

    // One provider with both auth types lands on the auth-type picker for
    // that provider (pi's same-id branch).
    run.type("/login kimi-coding\r");
    auto screen = visible_screen(run.terminal);
    CHECK(screen.find("Select authentication method for Kimi For Coding:") != std::string::npos);

    // The api-key method has no login hook: pi shows the ambient info dialog.
    run.type("\x1b[B");
    run.type("\r");
    screen = visible_screen(run.terminal);
    CHECK(screen.find("Kimi For Coding setup") != std::string::npos);
    CHECK(screen.find("Kimi API key is configured outside cch.") != std::string::npos);
    CHECK(screen.find("to close") != std::string::npos);
    CHECK(run.opened_urls.empty());

    // Esc/Ctrl+C closes the ambient dialog back to the editor.
    run.type("\x03");
    run.type("back to the editor");
    CHECK(visible_screen(run.terminal).find("back to the editor") != std::string::npos);

    // pi: app.exit (Ctrl+D) exits only when the editor is empty; clear it
    // first (app.clear is Ctrl+C in the main editor), then exit.
    run.type("\x03");
    run.exit();
}

TEST_CASE(
    "logout lists stored credentials and removes the selected one",
    "[coding_agent][tui][login][issue406]") {
    LoginFixture fixture;
    std::ofstream(fixture.auth_path(), std::ios::binary) << R"({
  "openai-codex": {
    "type": "oauth",
    "refresh": "dummy-refresh",
    "access": "dummy-access",
    "expires": 9999999999999
  }
})";
    auto codex = std::make_shared<ScriptedOAuthProvider>(
        "openai-codex",
        "OpenAI Codex",
        std::vector<ai::Model>{tests::scripted_request_model("openai-codex", "gpt-5.5")},
        [](ai::AuthInteraction) -> boost::asio::awaitable<support::Expected<ai::OAuthCredential>> {
            co_return dummy_oauth_credential();
        });
    auto runtime = fixture.create_runtime({codex});
    REQUIRE(runtime != nullptr);
    auto session = fixture.create_session(std::move(runtime));
    REQUIRE(session);

    InteractiveRun run;
    run.start(*session->session, fixture.agent_dir.path());

    run.type("/logout\r");
    auto screen = visible_screen(run.terminal);
    CHECK(screen.find("Select provider to logout:") != std::string::npos);
    CHECK(screen.find("OpenAI Codex") != std::string::npos);
    CHECK(screen.find("configured") != std::string::npos);

    run.type("\r");
    screen = visible_screen(run.terminal);
    CHECK(screen.find("Logged out of OpenAI Codex") != std::string::npos);
    // The stored credential was removed through the store.
    const auto auth_text = read_text(fixture.auth_path());
    CHECK(auth_text.find("openai-codex") == std::string::npos);

    // A second /logout reports the empty state verbatim (pi).
    run.type("/logout\r");
    screen = visible_screen(run.terminal);
    CHECK(screen.find(
        "No stored credentials to remove. /logout only removes credentials saved by "
        "/login; environment variables and models.json config are unchanged.") !=
        std::string::npos);

    run.exit();
}

TEST_CASE(
    "request-time re-auth guidance renders through the chat surface",
    "[coding_agent][tui][login][issue406]") {
    LoginFixture fixture;
    // A stored-but-expired OAuth credential whose request-time refresh fails:
    // pi's `_getRequiredRequestAuth` OAuth branch maps the dead credential to
    // the re-auth guidance (the no-key branch is the preflight's surface).
    std::ofstream(fixture.auth_path(), std::ios::binary) << R"({
  "openai-codex": {
    "type": "oauth",
    "refresh": "dummy-stale-refresh",
    "access": "dummy-stale-access",
    "expires": 1
  }
})";
    std::ofstream(fixture.agent_dir.path() / "settings.json", std::ios::binary)
        << R"({"defaultProvider": "openai-codex", "defaultModel": "gpt-5.5"})";
    auto codex = std::make_shared<ScriptedOAuthProvider>(
        "openai-codex",
        "OpenAI Codex",
        std::vector<ai::Model>{tests::scripted_request_model("openai-codex", "gpt-5.5")},
        [](ai::AuthInteraction) -> boost::asio::awaitable<support::Expected<ai::OAuthCredential>> {
            co_return dummy_oauth_credential();
        },
        [](ai::OAuthCredential) -> boost::asio::awaitable<support::Expected<ai::OAuthCredential>> {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::OAuth,
                "OpenAI Codex token refresh failed (401): token expired"));
        });
    auto runtime = fixture.create_runtime({codex});
    REQUIRE(runtime != nullptr);
    auto session = fixture.create_session(std::move(runtime));
    REQUIRE(session);
    // The stored credential resolves the settings default at boot.
    REQUIRE(session->session->snapshot().agent_state.model.id == "gpt-5.5");

    InteractiveRun run;
    run.start(*session->session, fixture.agent_dir.path());

    run.type("hello\r");
    const auto screen = visible_screen(run.terminal);
    CHECK(screen.find(
        "Authentication failed for \"openai-codex\". Credentials may have expired or "
        "network is unavailable. Run '/login openai-codex' to re-authenticate.") !=
        std::string::npos);

    run.exit();
}
