// T11 re-auth guidance evidence (#360): pi's two verbatim re-auth guidance
// branches — no key → `formatNoApiKeyFoundMessage` ("No API key found for X
// … Use /login to log into a provider via OAuth or API key"), OAuth
// credential missing/expired → "Authentication failed for X. Credentials may
// have expired or network is unavailable. Run '/login X' to re-authenticate."
// — at both trigger points. Preflight mirrors pi `agent-session.ts`
// `prompt()`'s `hasConfiguredAuth || checkAuth` check; request time mirrors
// `_getRequiredRequestAuth` through the session-layer stream decorator
// (`apply_auth_guidance`) driven by scripted auth terminals on the narrow
// `ModelStream` fake and scripted-client seams. No live keys or network:
// every request is served by scripted runtimes. Committed verbatim goldens
// pin both branches at both trigger points under `fixtures/pi-agent-core/`.

#include "ai/ModelStreamBridge.hpp"
#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/ai/Models.hpp>
#include <cch/coding_agent/AuthGuidance.hpp>
#include "coding_agent/AgentSession.hpp"
#include <cch/harness/session/JsonlSessionStore.hpp>
#include <cch/util/Error.hpp>
#include "coding_agent/runtime/AuthGuidanceStream.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/FakeModelStream.hpp"
#include "support/ModelsFixture.hpp"
#include "support/ModelFixture.hpp"
#include "support/TempWorkspace.hpp"
#include "util/ExpectedMacros.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

/// One isolated assembly fixture for the preflight tests: a temp workspace
/// for the session file and a temp Agent Config Directory (`PI_CODING_AGENT_DIR`)
/// whose models.json drives resolution deterministically. Ambient
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
};

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

/// CLI creation request for the preflight tests.
[[nodiscard]] coding_agent::runtime::AgentSessionCreationRequest cli_request(
    const Fixture& fixture) {
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_target =
        coding_agent::ExplicitOpenOrCreateSessionTarget{fixture.session_file};
    request.workspace = fixture.workspace.path();
    return request;
}

/// A FIFO scripted chat client that answers every request with one scripted
/// auth-category terminal (exactly one `error` terminal event plus the
/// agreeing final AssistantMessage — the #326 terminal contract), driving the
/// request-time guidance through the session's stream seam.
class AuthTerminalProvider final : public tests::ScriptedProvider {
public:
    AuthTerminalProvider(util::ErrorCode code,
                         std::string message,
                         std::string content = {})
        : ScriptedProvider("sdk-host"),
          code_(code),
          message_(std::move(message)),
          content_(std::move(content)) {}

    [[nodiscard]] ai::ModelStream stream(
        ai::Model model,
        ai::AiContext,
        ai::ProviderStreamOptions) override {
        return ai::detail::make_model_stream(
            [this, model = std::move(model)](
                ai::AssistantEventSink sink) mutable
                -> boost::asio::awaitable<util::Expected<ai::AssistantMessage>> {
        ++request_count;
        auto terminal = ai::assistant_text_message(content_);
        terminal.stop_reason = ai::AssistantStopReason::Error;
        terminal.error_message = message_;
        terminal.provider = "sdk-host";
        terminal.api = "fake";
        terminal.model = model.id;
        // Session files require real epoch timestamps on assistant messages.
        terminal.timestamp = 1718000000123;
        if (sink) {
            CCH_TRY_VOID(sink(ai::AssistantErrorEvent{
                .reason = terminal.stop_reason,
                .error = terminal,
                .failure = util::make_error(code_, message_),
            }));
        }
        co_return terminal;
                });
    }


    int request_count{0};

private:
    util::ErrorCode code_;
    std::string message_;
    std::string content_;
};

/// Session creation against the scripted-client seam (a real ModelRuntime
/// composed over scripted providers, ADR 0034 "primary seam — Agent +
/// session-assembly composition").
[[nodiscard]] util::Expected<coding_agent::CreateAgentSessionResult>
create_scripted_session(
    std::shared_ptr<ai::Provider> client,
    const std::filesystem::path& session_file,
    const std::filesystem::path& workspace) {
    tests::ModelsSessionOptions options;
    options.session_target =
        coding_agent::ExplicitOpenOrCreateSessionTarget{session_file};
    options.workspace = workspace;
    options.models = cch::tests::models_from_provider(std::move(client));
    options.request_model = cch::tests::scripted_request_model("sdk-host", "sdk-model");
    return coding_agent::create_agent_session(std::move(options));
}

template <typename T>
[[nodiscard]] T run_awaitable(boost::asio::awaitable<T> awaitable) {
    boost::asio::io_context io;
    std::optional<T> result;
    std::exception_ptr exception;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            try {
                result = co_await std::move(awaitable);
            } catch (...) {
                exception = std::current_exception();
            }
            co_return;
        },
        boost::asio::detached);
    io.run();
    if (exception) {
        std::rethrow_exception(exception);
    }
    REQUIRE(result.has_value());
    return std::move(*result);
}

[[nodiscard]] std::string read_golden_text(std::string_view name) {
    const std::string path = std::string{CCH_SOURCE_DIR} +
                             "/fixtures/pi-agent-core/" +
                             std::string{name};
    std::ifstream input(path, std::ios::binary);
    return std::string{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::string default_no_key_guidance(std::string_view provider) {
    return coding_agent::format_no_api_key_found_message(
        provider,
        std::filesystem::path{
            coding_agent::kDefaultAuthGuidanceDocsPath});
}

[[nodiscard]] std::string default_oauth_guidance(std::string_view provider) {
    return coding_agent::format_oauth_reauthenticate_message(provider);
}

[[nodiscard]] ai::AssistantMessage terminal_message(std::string error_message) {
    auto terminal = ai::assistant_text_message("");
    terminal.stop_reason = ai::AssistantStopReason::Error;
    terminal.error_message = std::move(error_message);
    return terminal;
}

/// Drive one decorated stream end to end: produce the inner ModelStream from
/// the narrow fake factory, wrap it with `apply_auth_guidance`, and consume it
/// with `ai::consume`, recording every forwarded event.
struct GuidedRun {
    util::Expected<ai::AssistantMessage> result;
    std::vector<ai::AssistantStreamEvent> events;
};

[[nodiscard]] GuidedRun run_guided(
    std::shared_ptr<tests::FakeModelStream> fake,
    bool is_using_oauth) {
    // Produce and consume the stream on one executor: the narrow fake's
    // factory captures the consuming executor, so the ModelStream it returns
    // must be consumed on that same io_context.
    boost::asio::io_context io;
    GuidedRun out;
    std::exception_ptr exception;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            auto factory = fake->factory();
            auto inner = factory(
                tests::make_model("gpt-test"),
                ai::AiContext{},
                ai::SimpleStreamOptions{});
            auto decorated = coding_agent::runtime::apply_auth_guidance(
                std::move(inner),
                "fake",
                [is_using_oauth](std::string_view) { return is_using_oauth; });
            out.result = co_await ai::consume(
                std::move(decorated),
                [&out](const ai::AssistantStreamEvent& event) -> util::ExpectedVoid {
                    out.events.push_back(event);
                    return {};
                });
            co_return;
        },
        boost::asio::detached);
    io.run();
    if (exception) {
        std::rethrow_exception(exception);
    }
    return out;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Preflight (pi `agent-session.ts` prompt() `hasConfiguredAuth` check)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE(
    "preflight no-key branch fails the prompt with pi's verbatim formatNoApiKeyFoundMessage",
    "[coding_agent][re-auth-guidance][issue360]") {
    Fixture fixture;
    fixture.write_models(kKeylessAlphaKeyedBeta);

    auto request = cli_request(fixture);
    request.provider = "alpha";
    request.model = "alpha-1";
    auto result = coding_agent::create_agent_session(std::move(request));
    REQUIRE(result.has_value());
    CHECK(result->provider == "alpha");
    CHECK(result->model == "alpha-1");

    // A real model whose provider resolves no auth fails at preflight, before
    // any stream: pi's verbatim no-key branch through the auth category of the
    // single Expected channel (no second exception hierarchy).
    auto prompted = result->session->prompt_blocking("hello");
    REQUIRE_FALSE(prompted.has_value());
    CHECK(prompted.error().code == util::ErrorCode::Auth);
    CHECK(prompted.error().message ==
          read_golden_text("re-auth-guidance-preflight-no-key.txt"));
    CHECK(prompted.error().message == default_no_key_guidance("alpha"));
    result->session->close();
}

TEST_CASE(
    "preflight OAuth branch fails the prompt with pi's verbatim re-auth guidance",
    "[coding_agent][re-auth-guidance][issue360]") {
    Fixture fixture;

    // No models.json: the built-in kimi-coding provider (OAuth + ambient API
    // key) resolves with no stored credential and no KIMI_API_KEY.
    auto request = cli_request(fixture);
    request.provider = "kimi-coding";
    request.model = "kimi-for-coding";
    auto result = coding_agent::create_agent_session(std::move(request));
    REQUIRE(result.has_value());
    CHECK(result->provider == "kimi-coding");

    auto prompted = result->session->prompt_blocking("hello");
    REQUIRE_FALSE(prompted.has_value());
    CHECK(prompted.error().code == util::ErrorCode::Auth);
    CHECK(prompted.error().message ==
          read_golden_text("re-auth-guidance-preflight-oauth.txt"));
    CHECK(prompted.error().message ==
          default_oauth_guidance("kimi-coding"));
    result->session->close();
}

// ─────────────────────────────────────────────────────────────────────────────
// Request time (pi `_getRequiredRequestAuth`, session-layer stream decorator)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE(
    "request-time no-key branch surfaces pi's verbatim guidance through the terminal assistant message",
    "[coding_agent][re-auth-guidance][issue360]") {
    tests::TempWorkspace workspace;
    auto client = std::make_shared<AuthTerminalProvider>(
        util::ErrorCode::Auth, "Provider is not configured: sdk-host");
    auto created = create_scripted_session(
        std::move(client),
        workspace.path() / "test-session.jsonl",
        workspace.path());
    REQUIRE(created.has_value());
    auto* session = created->session.get();

    // Preflight passes (the scripted provider resolves as configured); the
    // request-time auth terminal is rewritten to the verbatim no-key branch
    // with the category preserved (six-category channel).
    auto prompted = session->prompt_blocking("hello");
    REQUIRE(prompted.has_value());
    const auto& messages = session->snapshot().agent_state.messages;
    REQUIRE(messages.size() == 2);
    const auto& terminal = std::get<ai::AssistantMessage>(messages.back());
    CHECK(terminal.stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(terminal.error_message.has_value());
    CHECK(*terminal.error_message ==
          read_golden_text("re-auth-guidance-request-no-key.txt"));
    CHECK(*terminal.error_message == default_no_key_guidance("sdk-host"));
    session->close();
}

TEST_CASE(
    "request-time OAuth branch surfaces pi's verbatim re-auth guidance for dead credentials",
    "[coding_agent][re-auth-guidance][issue360]") {
    tests::TempWorkspace workspace;
    auto client = std::make_shared<AuthTerminalProvider>(
        util::ErrorCode::OAuth, "OAuth refresh failed for sdk-host");
    auto created = create_scripted_session(
        std::move(client),
        workspace.path() / "test-session.jsonl",
        workspace.path());
    REQUIRE(created.has_value());
    auto* session = created->session.get();

    // Dead credentials stay in auth.json and every subsequent request fails
    // with the re-auth guidance (pi `_getRequiredRequestAuth` OAuth branch).
    auto prompted = session->prompt_blocking("hello");
    REQUIRE(prompted.has_value());
    const auto& messages = session->snapshot().agent_state.messages;
    REQUIRE(messages.size() == 2);
    const auto& terminal = std::get<ai::AssistantMessage>(messages.back());
    CHECK(terminal.stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(terminal.error_message.has_value());
    CHECK(*terminal.error_message ==
          read_golden_text("re-auth-guidance-request-oauth.txt"));
    CHECK(*terminal.error_message == default_oauth_guidance("sdk-host"));
    session->close();
}

// ─────────────────────────────────────────────────────────────────────────────
// Request-time decorator (pi `_getRequiredRequestAuth`): `apply_auth_guidance`
// wraps one AI-owned ModelStream produced by the narrow fake factory, rewriting
// auth/oauth-category terminals. No virtual Models Runtime surface is involved.

TEST_CASE(
    "request-time decorator maps an auth terminal to the no-key branch through a ModelStream",
    "[coding_agent][re-auth-guidance][issue360]") {
    auto fake = std::make_shared<tests::FakeModelStream>();
    fake->terminal_failure_code = util::ErrorCode::Auth;
    fake->responses.push_back(
        terminal_message("Provider is not configured: gpt-test"));

    auto run = run_guided(fake, /*is_using_oauth=*/false);

    // Exactly one rewritten error terminal plus the agreeing final message,
    // with the auth category flowing through the single Expected channel.
    REQUIRE(run.result.has_value());
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(run.result->error_message.has_value());
    CHECK(*run.result->error_message == default_no_key_guidance("fake"));
    REQUIRE(run.events.size() == 1);
    const auto* error = std::get_if<ai::AssistantErrorEvent>(&run.events[0]);
    REQUIRE(error != nullptr);
    REQUIRE(error->failure.has_value());
    CHECK(error->failure->code == util::ErrorCode::Auth);
    REQUIRE(error->error.error_message.has_value());
    CHECK(*error->error.error_message == default_no_key_guidance("fake"));
    CHECK(fake->terminal_events == 1);
}

TEST_CASE(
    "request-time decorator maps an auth terminal on an OAuth provider to the re-auth branch",
    "[coding_agent][re-auth-guidance][issue360]") {
    auto fake = std::make_shared<tests::FakeModelStream>();
    fake->terminal_failure_code = util::ErrorCode::Auth;
    fake->responses.push_back(
        terminal_message("Provider is not configured: gpt-test"));

    auto run = run_guided(fake, /*is_using_oauth=*/true);

    REQUIRE(run.result.has_value());
    REQUIRE(run.result->error_message.has_value());
    CHECK(*run.result->error_message == default_oauth_guidance("fake"));
    REQUIRE(run.events.size() == 1);
    const auto* error = std::get_if<ai::AssistantErrorEvent>(&run.events[0]);
    REQUIRE(error != nullptr);
    REQUIRE(error->failure.has_value());
    CHECK(error->failure->code == util::ErrorCode::Auth);
    REQUIRE(error->error.error_message.has_value());
    CHECK(*error->error.error_message == default_oauth_guidance("fake"));
}

TEST_CASE(
    "request-time decorator maps an oauth terminal (dead credentials) to the re-auth branch",
    "[coding_agent][re-auth-guidance][issue360]") {
    auto fake = std::make_shared<tests::FakeModelStream>();
    fake->terminal_failure_code = util::ErrorCode::OAuth;
    fake->responses.push_back(
        terminal_message("OAuth refresh failed for gpt-test"));

    auto run = run_guided(fake, /*is_using_oauth=*/false);

    REQUIRE(run.result.has_value());
    REQUIRE(run.result->error_message.has_value());
    CHECK(*run.result->error_message == default_oauth_guidance("fake"));
    REQUIRE(run.events.size() == 1);
    const auto* error = std::get_if<ai::AssistantErrorEvent>(&run.events[0]);
    REQUIRE(error != nullptr);
    REQUIRE(error->failure.has_value());
    CHECK(error->failure->code == util::ErrorCode::OAuth);
    REQUIRE(error->error.error_message.has_value());
    CHECK(*error->error.error_message == default_oauth_guidance("fake"));
}

TEST_CASE(
    "request-time decorator passes non-auth terminals and successes through unchanged",
    "[coding_agent][re-auth-guidance][issue360]") {
    {
        auto fake = std::make_shared<tests::FakeModelStream>();
        fake->terminal_failure_code = util::ErrorCode::Stream;
        fake->responses.push_back(terminal_message("provider request failed"));
        auto run = run_guided(fake, /*is_using_oauth=*/false);
        REQUIRE(run.result.has_value());
        REQUIRE(run.result->error_message.has_value());
        CHECK(*run.result->error_message == "provider request failed");
    }
    {
        auto fake = std::make_shared<tests::FakeModelStream>();
        fake->responses.push_back(ai::assistant_text_message("hello user"));
        auto run = run_guided(fake, /*is_using_oauth=*/false);
        REQUIRE(run.result.has_value());
        CHECK(ai::text_from_assistant_content(run.result->content) ==
              "hello user");
    }
}


// Summarization seam (pi `_getSummarizationRequestAuth` → `_getRequiredRequestAuth`)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE(
    "summarization requests carry the request-time guidance when auth fails",
    "[coding_agent][re-auth-guidance][compaction][issue360]") {
    // A persisted session whose live history reaches the 20000-token
    // keepRecentTokens budget (three 20K-char user/assistant pairs, each
    // ~5001 estimated tokens), then a manual compaction whose summarization
    // request fails with an auth terminal: the guidance surfaces through the
    // compaction outcome.
    tests::TempWorkspace workspace;
    const std::string big(20000, 'x');
    auto client = std::make_shared<AuthTerminalProvider>(
        util::ErrorCode::Auth,
        "Provider is not configured: sdk-host",
        "a" + big);
    auto* client_ptr = client.get();
    auto created = create_scripted_session(
        std::move(client),
        workspace.path() / "test-session.jsonl",
        workspace.path());
    REQUIRE(created.has_value());
    auto* session = created->session.get();

    // Preflight passes; the first three prompts stream normally (the scripted
    // client serves every request with the auth terminal, which is what the
    // request-time mapping expects — but preflight must not fire first).
    REQUIRE(session->prompt_blocking(big + " u1").has_value());
    REQUIRE(session->prompt_blocking(big + " u2").has_value());
    REQUIRE(session->prompt_blocking(big + " u3").has_value());
    CHECK(client_ptr->request_count == 3);

    auto compacted = run_awaitable(session->compact());
    REQUIRE_FALSE(compacted.has_value());
    // The fourth request (summarization) failed with the auth terminal, which
    // the decorator rewrote to the verbatim guidance; the compaction machinery
    // reports it as a summarization failure carrying the guidance.
    CHECK(client_ptr->request_count == 4);
    CHECK(compacted.error().message.find(
              default_no_key_guidance("sdk-host")) != std::string::npos);
    session->close();
}
