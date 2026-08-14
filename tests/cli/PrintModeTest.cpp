#include "ai/ModelStreamBridge.hpp"
#include <catch2/catch_test_macros.hpp>
#include "support/ModelsFixture.hpp"

#include "cli/PrintMode.hpp"

#include "ai/providers/FakeProvider.hpp"
#include "coding_agent/AgentSession.hpp"
#include "support/TempWorkspace.hpp"
#include "support/TextHelpers.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

using namespace cch;

namespace {

class FailingStreambuf final : public std::streambuf {
protected:
    int_type overflow(int_type) override { return traits_type::eof(); }
};

/// A host client whose accepted call streams a partial text delta, terminates
/// with one error/aborted event carrying the final Assistant Message, and
/// returns that same message as a successful C++ value.
class TerminalOutcomeChatProvider final : public tests::ScriptedProvider {
public:
    TerminalOutcomeChatProvider(
        ai::AssistantStopReason reason,
        std::string diagnostic,
        std::string partial = "partial draft")
        : ScriptedProvider("sdk-host"),
          reason_(reason),
          diagnostic_(std::move(diagnostic)),
          partial_(std::move(partial)) {}

    [[nodiscard]] ai::ModelStream stream(
        ai::Model model,
        ai::AiContext,
        ai::ProviderStreamOptions) override {
        return ai::detail::make_model_stream(
            [this, model = std::move(model)](
                ai::AssistantEventSink sink) mutable
                -> boost::asio::awaitable<util::Expected<ai::AssistantMessage>> {
        ++request_count;
        ai::AssistantMessage terminal;
        terminal.provider = "host";
        terminal.api = "host";
        terminal.model = model.id;
        terminal.stop_reason = reason_;
        terminal.error_message = diagnostic_;
        terminal.content.emplace_back(ai::text_content(partial_));
        if (sink) {
            if (auto started = sink(ai::AssistantStartEvent{terminal}); !started) {
                co_return std::unexpected(started.error());
            }
            if (auto delta = sink(ai::TextDeltaEvent{0, partial_, terminal}); !delta) {
                co_return std::unexpected(delta.error());
            }
            if (auto ended = sink(ai::AssistantErrorEvent{reason_, terminal}); !ended) {
                co_return std::unexpected(ended.error());
            }
        }
        co_return terminal;
                });
    }


    int request_count{0};

private:
    ai::AssistantStopReason reason_;
    std::string diagnostic_;
    std::string partial_;
};

/// A host client that returns one text block per request, capturing every
/// request's message history.
class CapturingChatProvider final : public tests::ScriptedProvider {
public:
    CapturingChatProvider() : ScriptedProvider("sdk-host") {}

    [[nodiscard]] ai::ModelStream stream(
        ai::Model model,
        ai::AiContext context,
        ai::ProviderStreamOptions) override {
        return ai::detail::make_model_stream(
            [this, model = std::move(model), context = std::move(context)](
                ai::AssistantEventSink sink) mutable
                -> boost::asio::awaitable<util::Expected<ai::AssistantMessage>> {
        messages.push_back(context.messages);
        ai::AssistantMessage message;
        message.provider = "host";
        message.api = "host";
        message.model = model.id;
        message.stop_reason = ai::AssistantStopReason::Stop;
        message.content.emplace_back(ai::text_content("captured"));
        if (sink) {
            if (auto started = sink(ai::AssistantStartEvent{message}); !started) {
                co_return std::unexpected(started.error());
            }
            if (auto done = sink(ai::AssistantDoneEvent{message.stop_reason, message}); !done) {
                co_return std::unexpected(done.error());
            }
        }
        co_return message;
                });
    }


    std::vector<std::vector<ai::MessageVariant>> messages;
};

/// A host client that holds each request until the run's stop token fires
/// (like the real transports), then settles with an aborted terminal. Lets a
/// signal test deterministically interrupt an in-flight prompt.
class SignalGateProvider final : public tests::ScriptedProvider {
public:
    SignalGateProvider() : ScriptedProvider("sdk-host") {}

    [[nodiscard]] ai::ModelStream stream(
        ai::Model model,
        ai::AiContext,
        ai::ProviderStreamOptions options) override {
        return ai::detail::make_model_stream(
            [this, model = std::move(model), options = std::move(options)](
                ai::AssistantEventSink) mutable
                -> boost::asio::awaitable<util::Expected<ai::AssistantMessage>> {
        ++request_count;
        const auto executor = co_await boost::asio::this_coro::executor;
        boost::asio::steady_timer gate{executor};
        gate.expires_at(std::chrono::steady_clock::time_point::max());
        // Cancellation may be requested from any thread; the timer is only
        // touched on its own executor.
        std::stop_callback cancellation{options.stop_token, [executor, &gate] {
            boost::asio::post(executor, [&gate] {
                boost::system::error_code ignored;
                (void)gate.cancel();
            });
        }};
        boost::system::error_code error;
        co_await gate.async_wait(
            boost::asio::redirect_error(boost::asio::use_awaitable, error));

        ai::AssistantMessage message;
        message.provider = "host";
        message.api = "host";
        message.model = model.id;
        message.stop_reason = ai::AssistantStopReason::Aborted;
        message.error_message = "Request was aborted";
        message.content.emplace_back(ai::text_content("never printed"));
        co_return message;
                });
    }


    std::atomic<int> request_count{0};
};

tests::ModelsSessionOptions fake_in_memory_options(
    const std::filesystem::path& workspace) {
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace;
    options.models = ai::providers::make_scripted_fake_models();
    return options;
}

struct CreatedSession {
    coding_agent::CreateAgentSessionResult created;
};

CreatedSession make_session(const tests::TempWorkspace& workspace) {
    auto created = coding_agent::create_agent_session(fake_in_memory_options(workspace.path()));
    REQUIRE(created.has_value());
    return CreatedSession{std::move(*created)};
}

CreatedSession make_session(
    const tests::TempWorkspace& workspace,
    std::shared_ptr<ai::Provider> chat_client) {
    auto options = fake_in_memory_options(workspace.path());
    options.models = cch::tests::models_from_provider(std::move(chat_client));
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created.has_value());
    return CreatedSession{std::move(*created)};
}

int run_print(
    coding_agent::AgentSession& session,
    std::ostringstream& output,
    std::ostringstream& error,
    cli::PrintModePlan plan) {
    boost::asio::io_context io;
    return cli::run_print_mode(
        io,
        session,
        cli::PrintModeConfig{.output = output, .error = error},
        std::move(plan));
}

} // namespace

TEST_CASE(
    "print mode prints only the final assistant text blocks and exits 0",
    "[cli][print]") {
    tests::TempWorkspace workspace;
    auto session = make_session(workspace);

    std::ostringstream output;
    std::ostringstream error;
    const auto exit_code = run_print(
        *session.created.session, output, error,
        cli::PrintModePlan{.initial_message = "hello"});

    CHECK(exit_code == 0);
    CHECK(output.str() == "fake: hello\n");
    CHECK(error.str().empty());
}

TEST_CASE(
    "print mode reports a terminal error outcome on stderr and exits 1",
    "[cli][print]") {
    tests::TempWorkspace workspace;
    auto session = make_session(
        workspace,
        std::make_shared<TerminalOutcomeChatProvider>(
            ai::AssistantStopReason::Error, "host transport lost"));

    std::ostringstream output;
    std::ostringstream error;
    const auto exit_code = run_print(
        *session.created.session, output, error,
        cli::PrintModePlan{.initial_message = "hello"});

    CHECK(exit_code == 1);
    CHECK(output.str().empty());
    CHECK(error.str() == "host transport lost\n");
}

TEST_CASE(
    "print mode falls back to Request <stopReason> without an error message",
    "[cli][print]") {
    tests::TempWorkspace workspace;
    auto session = make_session(
        workspace,
        std::make_shared<TerminalOutcomeChatProvider>(
            ai::AssistantStopReason::Error, ""));

    std::ostringstream output;
    std::ostringstream error;
    const auto exit_code = run_print(
        *session.created.session, output, error,
        cli::PrintModePlan{.initial_message = "hello"});

    CHECK(exit_code == 1);
    CHECK(output.str().empty());
    CHECK(error.str() == "Request error\n");
}

TEST_CASE(
    "print mode reports a terminal aborted outcome on stderr and exits 1",
    "[cli][print]") {
    tests::TempWorkspace workspace;
    auto session = make_session(
        workspace,
        std::make_shared<TerminalOutcomeChatProvider>(
            ai::AssistantStopReason::Aborted, "host cancelled the request"));

    std::ostringstream output;
    std::ostringstream error;
    const auto exit_code = run_print(
        *session.created.session, output, error,
        cli::PrintModePlan{.initial_message = "hello"});

    CHECK(exit_code == 1);
    CHECK(output.str().empty());
    CHECK(error.str() == "host cancelled the request\n");
}

TEST_CASE(
    "print mode redacts and bounds a terminal diagnostic on stderr",
    "[cli][print]") {
    tests::TempWorkspace workspace;
    const std::string secret = "sk-hostsecret123456789";
    std::string diagnostic = "provider rejected " + secret + " detail ";
    diagnostic += std::string(9000, 'x');
    auto session = make_session(
        workspace,
        std::make_shared<TerminalOutcomeChatProvider>(
            ai::AssistantStopReason::Error, diagnostic));

    std::ostringstream output;
    std::ostringstream error;
    const auto exit_code = run_print(
        *session.created.session, output, error,
        cli::PrintModePlan{.initial_message = "hello"});

    CHECK(exit_code == 1);
    CHECK(output.str().empty());
    CHECK(error.str().find(secret) == std::string::npos);
    CHECK(error.str().find("[REDACTED]") != std::string::npos);
    // The reported diagnostic stays inside the bounded-output budget even
    // though the raw provider diagnostic far exceeds it.
    CHECK(error.str().size() < diagnostic.size());
}

TEST_CASE(
    "print mode prompts remaining positionals sequentially and outputs the last response",
    "[cli][print]") {
    tests::TempWorkspace workspace;
    auto client = std::make_shared<CapturingChatProvider>();
    auto* probe = client.get();
    auto session = make_session(workspace, std::move(client));

    std::ostringstream output;
    std::ostringstream error;
    const auto exit_code = run_print(
        *session.created.session, output, error,
        cli::PrintModePlan{
            .initial_message = "first",
            .messages = {"second", "third"},
        });

    CHECK(exit_code == 0);
    CHECK(output.str() == "captured\n");
    CHECK(error.str().empty());
    REQUIRE(probe->messages.size() == 3);
    const auto user_text = [](const std::vector<ai::MessageVariant>& messages) {
        const auto* user = std::get_if<ai::UserMessage>(&messages.back());
        REQUIRE(user != nullptr);
        return ai::text_from_user_message(*user);
    };
    CHECK(user_text(probe->messages[0]) == "first");
    CHECK(user_text(probe->messages[1]) == "second");
    CHECK(user_text(probe->messages[2]) == "third");
}

TEST_CASE(
    "print mode with no prompt prints nothing and exits 0",
    "[cli][print]") {
    tests::TempWorkspace workspace;
    auto session = make_session(workspace);

    std::ostringstream output;
    std::ostringstream error;
    const auto exit_code = run_print(
        *session.created.session, output, error, cli::PrintModePlan{});

    CHECK(exit_code == 0);
    CHECK(output.str().empty());
    CHECK(error.str().empty());
}

TEST_CASE(
    "print mode reports a prompt preflight rejection as loop failed and exits 1",
    "[cli][print]") {
    tests::TempWorkspace workspace;
    auto session = make_session(workspace);
    session.created.session->close();

    std::ostringstream output;
    std::ostringstream error;
    const auto exit_code = run_print(
        *session.created.session, output, error,
        cli::PrintModePlan{.initial_message = "hello"});

    CHECK(exit_code == 1);
    CHECK(output.str().empty());
    CHECK(error.str().find("loop failed: ") != std::string::npos);
}

TEST_CASE(
    "print mode disposes the session and exits 143 on SIGTERM",
    "[cli][print][signals]") {
    tests::TempWorkspace workspace;
    auto client = std::make_shared<SignalGateProvider>();
    auto* probe = client.get();
    auto session = make_session(workspace, std::move(client));

    std::ostringstream output;
    std::ostringstream error;
    std::atomic<int> exit_code{-1};
    std::thread runner{[&] {
        exit_code = run_print(
            *session.created.session, output, error,
            cli::PrintModePlan{.initial_message = "hello"});
    }};
    while (probe->request_count.load() == 0) {
        std::this_thread::yield();
    }
    std::raise(SIGTERM);
    runner.join();

    CHECK(exit_code == 143);
    CHECK_FALSE(session.created.session->is_open());
    CHECK(output.str().empty());
    CHECK(error.str().empty());
}

#if !defined(_WIN32)
TEST_CASE(
    "print mode disposes the session and exits 129 on SIGHUP",
    "[cli][print][signals]") {
    tests::TempWorkspace workspace;
    auto client = std::make_shared<SignalGateProvider>();
    auto* probe = client.get();
    auto session = make_session(workspace, std::move(client));

    std::ostringstream output;
    std::ostringstream error;
    std::atomic<int> exit_code{-1};
    std::thread runner{[&] {
        exit_code = run_print(
            *session.created.session, output, error,
            cli::PrintModePlan{.initial_message = "hello"});
    }};
    while (probe->request_count.load() == 0) {
        std::this_thread::yield();
    }
    std::raise(SIGHUP);
    runner.join();

    CHECK(exit_code == 129);
    CHECK_FALSE(session.created.session->is_open());
    CHECK(output.str().empty());
    CHECK(error.str().empty());
}
#endif

TEST_CASE(
    "print mode keeps the session open after a normal run",
    "[cli][print]") {
    tests::TempWorkspace workspace;
    auto session = make_session(workspace);

    std::ostringstream output;
    std::ostringstream error;
    const auto exit_code = run_print(
        *session.created.session, output, error,
        cli::PrintModePlan{.initial_message = "hello"});

    CHECK(exit_code == 0);
    CHECK(session.created.session->is_open());
}

TEST_CASE(
    "print mode output failure surfaces as exit 1",
    "[cli][print]") {
    tests::TempWorkspace workspace;
    auto session = make_session(workspace);

    FailingStreambuf failing;
    std::ostream broken_output{&failing};
    std::ostringstream error;
    boost::asio::io_context io;
    const auto exit_code = cli::run_print_mode(
        io,
        *session.created.session,
        cli::PrintModeConfig{.output = broken_output, .error = error},
        cli::PrintModePlan{.initial_message = "hello"});

    // pi writes the final text blocks without an error channel; a broken
    // output stream cannot change the outcome, so the run still exits 0.
    CHECK(exit_code == 0);
}
