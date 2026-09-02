#include "ai/ModelStreamBridge.hpp"
#include <cch/ai/Content.hpp>
#include "coding_agent/AgentSession.hpp"
#include <cch/agent/harness/session/SessionResume.hpp>
#include "coding_agent/runtime/AgentSessionInteractiveAccess.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/FakeUserShell.hpp"
#include "support/GatedChatProvider.hpp"
#include "support/ModelsFixture.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"
#include "support/UserBashTestHooks.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

using tests::bash_message_count;

/// Answers every provider request immediately so cancellation recovery paths
/// can run without gate choreography.
class ImmediateChatProvider final : public tests::ScriptedProvider {
public:
    ImmediateChatProvider() : ScriptedProvider("sdk-host") {}

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext context, coding_agent::ModelRuntimeTestStreamOptions options) override {
        return ai::detail::make_model_stream(
            [this, model = std::move(model), context = std::move(context), options = std::move(options)](
                ai::AssistantEventSink) mutable
                -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
        requests.push_back(tests::RecordedProviderRequest{model, context, options});
        auto response = ai::assistant_text_message("immediate reply");
        response.provider = "immediate-fake";
        response.api = "fake";
        response.model = model.id;
        co_return response;
                });
    }

    std::vector<tests::RecordedProviderRequest> requests;
};

[[nodiscard]] support::AsyncResult<coding_agent::CreateAgentSessionResult> make_session_async(
        tests::RuntimeFixture& runtime,
        const std::filesystem::path& workspace,
        std::shared_ptr<tests::ScriptedProvider> client,
        std::unique_ptr<tests::FakeUserShell> shell,
        std::optional<std::filesystem::path> session_path = std::nullopt) {
    coding_agent::runtime::AgentSessionCreationRequest request;
    if (session_path) {
        request.session_target = coding_agent::ExplicitOpenOrCreateSessionTarget{*session_path};
    } else {
        request.session_target = coding_agent::InMemorySessionTarget{};
    }
    request.workspace = workspace;
    request.execution_runtime_target = runtime.make_target();
    return coding_agent::create_agent_session_async(std::move(request),
            std::nullopt,
            coding_agent::runtime::AssemblyOverrides{.model_runtime = nullptr,
                    .cli_fake = false,
                    .models = cch::tests::models_from_provider(std::move(client)),
                    .user_shell = std::move(shell)});
}

template <typename Factory> [[nodiscard]] auto run_on_runtime(tests::RuntimeFixture& runtime, Factory factory) {
    return runtime.run(support::detail::make_async_result(std::move(factory)));
}

template <typename Predicate> [[nodiscard]] boost::asio::awaitable<void> wait_until(Predicate predicate) {
    const auto executor = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer timer(executor);
    while (!predicate()) {
        timer.expires_after(std::chrono::milliseconds{1});
        boost::system::error_code error;
        co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
    }
    co_return;
}

template <typename ProgressSink = coding_agent::runtime::UserBashProgressSink>
[[nodiscard]] auto run_bash(tests::RuntimeFixture& runtime,
        coding_agent::AgentSession& session,
        std::string command,
        ProgressSink progress_sink = {}) {
    return run_on_runtime(runtime,
            [&session,
                    command = std::move(command),
                    progress_sink = coding_agent::runtime::UserBashProgressSink{std::move(progress_sink)}]() mutable
                    -> boost::asio::awaitable<support::Expected<coding_agent::runtime::UserBashCompletion>> {
                co_return co_await coding_agent::detail::AgentSessionInteractiveAccess::run_user_bash(
                        session, std::move(command), false, std::move(progress_sink));
            });
}

auto run_prompt(tests::RuntimeFixture& runtime, coding_agent::AgentSession& session, std::string text) {
    return run_on_runtime(
            runtime, [&session, text = std::move(text)]() mutable -> boost::asio::awaitable<support::ExpectedVoid> {
                co_return co_await session.prompt(std::move(text));
            });
}

} // namespace

TEST_CASE(
    "User Bash progress and committed messages preserve the raw command",
    "[coding_agent][runtime][issue96]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto client = std::make_shared<ImmediateChatProvider>();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"command output"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });

    auto created = runtime.run(make_session_async(runtime, workspace.path(), std::move(client), std::move(shell)));
    REQUIRE(created);
    auto& session = runtime.adopt_session(std::move(created->session));

    const std::string command =
        "printf\t'雪 api_key=sk-abcdefghijklmnopqrstuvwxyz123456'\x1b[31m";
    std::vector<std::string> progress_commands;
    auto result = run_bash(
            runtime, session, command, [&progress_commands](const coding_agent::runtime::UserBashProgress& progress) {
                progress_commands.push_back(progress.command);
                return support::ExpectedVoid{};
            });

    REQUIRE(result);
    REQUIRE(shell_pointer->commands.size() == 1);
    CHECK(shell_pointer->commands.front() == command);
    REQUIRE(progress_commands.size() == 2);
    CHECK(progress_commands[0] == command);
    CHECK(progress_commands[1] == command);
    CHECK(result->message.command == command);
    const auto& committed = session.snapshot().agent_state.messages;
    REQUIRE(committed.size() == 1);
    CHECK(std::get<ai::BashExecutionMessage>(committed.front()).command == command);
}

TEST_CASE(
    "User Bash committed messages preserve the raw full output path",
    "[coding_agent][runtime][issue96]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    const auto spill_directory =
        workspace.path() / "api_key=raw-path-secret\x1b[31m";
    std::error_code directory_error;
    std::filesystem::create_directories(spill_directory, directory_error);
    REQUIRE_FALSE(directory_error);
    tests::EnvVarGuard tmpdir{"TMPDIR", spill_directory.string()};
    auto client = std::make_shared<ImmediateChatProvider>();
    auto shell = std::make_unique<tests::FakeUserShell>();
    shell->enqueue({
        .updates = {std::string(60 * 1024, 'x')},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });

    auto created = runtime.run(make_session_async(runtime, workspace.path(), std::move(client), std::move(shell)));
    REQUIRE(created);
    auto& session = runtime.adopt_session(std::move(created->session));

    auto result = run_bash(runtime, session, "large output");

    REQUIRE(result);
    REQUIRE(result->message.full_output_path.has_value());
    const std::filesystem::path full_output_path{*result->message.full_output_path};
    CHECK(full_output_path.parent_path() == spill_directory);
    const auto& committed = session.snapshot().agent_state.messages;
    REQUIRE(committed.size() == 1);
    const auto& committed_bash = std::get<ai::BashExecutionMessage>(committed.front());
    CHECK(committed_bash.full_output_path == result->message.full_output_path);
}

TEST_CASE(
    "User Bash shell error diagnostics pass through raw",
    "[coding_agent][runtime][issue96]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto client = std::make_shared<ImmediateChatProvider>();
    auto shell = std::make_unique<tests::FakeUserShell>();
    const std::string message = "spawn\x1b[31m failed api_key=message-secret";
    const std::string detail = "detail\t雪 api_key=detail-secret";
    const std::string context = "context\r\napi_key=context-secret";
    shell->enqueue({
        .updates = {},
        .result = {},
        .infrastructure_failure = support::make_error(
            support::ErrorCode::Process,
            message,
            detail,
            context),
        .gated = false,
    });

    auto created = runtime.run(make_session_async(runtime, workspace.path(), std::move(client), std::move(shell)));
    REQUIRE(created);
    auto& session = runtime.adopt_session(std::move(created->session));

    auto result = run_bash(runtime, session, "cannot spawn");

    REQUIRE_FALSE(result);
    CHECK(result.error().message == message);
    CHECK(result.error().detail == detail);
    REQUIRE(result.error().context.has_value());
    CHECK(*result.error().context == context);
    CHECK(session.snapshot().agent_state.messages.empty());
}

TEST_CASE(
    "idle User Bash cancellation retains partial output and commits one cancelled message",
    "[coding_agent][runtime][issue88]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    const auto session_path = workspace.path() / "cancel-idle.jsonl";
    auto client = std::make_shared<ImmediateChatProvider>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"first chunk", "\nsecond chunk"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    auto created = runtime.run(
            make_session_async(runtime, workspace.path(), std::move(client), std::move(shell), session_path));
    REQUIRE(created);
    auto& session = runtime.adopt_session(std::move(created->session));

    std::optional<support::Expected<coding_agent::runtime::UserBashCompletion>> bash_result;
    const auto scenario = run_on_runtime(runtime, [&]() -> boost::asio::awaitable<support::ExpectedVoid> {
        const auto executor = co_await boost::asio::this_coro::executor;
        auto bash = boost::asio::co_spawn(executor,
                coding_agent::detail::AgentSessionInteractiveAccess::run_user_bash(session,
                        "echo cancel me",
                        false,
                        [](const coding_agent::runtime::UserBashProgress&) { return support::ExpectedVoid{}; }),
                boost::asio::use_awaitable);
        co_await wait_until([&] { return shell_pointer->started_count == 1; });
        coding_agent::detail::AgentSessionInteractiveAccess::cancel_user_bash(session);
        bash_result.emplace(co_await std::move(bash));
        co_return support::ExpectedVoid{};
    });
    REQUIRE(scenario);

    // The cancelled terminal outcome retains the already delivered sanitized
    // output, carries no exit code, and commits exactly once.
    REQUIRE(bash_result.has_value());
    REQUIRE(*bash_result);
    CHECK(bash_result->value().message.cancelled);
    CHECK_FALSE(bash_result->value().message.exit_code.has_value());
    CHECK(bash_result->value().message.output == "first chunk\nsecond chunk");
    CHECK(bash_result->value().message.command == "echo cancel me");
    CHECK(bash_message_count(session.snapshot().agent_state.messages) == 1);
    CHECK(session.is_open());
    CHECK_FALSE(session.is_busy());

    auto resumed = harness::session::resume_session(session_path);
    REQUIRE(resumed);
    CHECK(bash_message_count(resumed->history) == 1);
    const auto* persisted =
        std::get_if<ai::BashExecutionMessage>(&resumed->history.back());
    REQUIRE(persisted != nullptr);
    CHECK(persisted->cancelled);
    CHECK_FALSE(persisted->exit_code.has_value());
    CHECK(persisted->output == "first chunk\nsecond chunk");

    // The Session stays usable: a later command and an ordinary Prompt work.
    shell_pointer->enqueue({
        .updates = {"recovery output"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });
    auto recovery_bash = run_bash(runtime, session, "echo recovered");
    REQUIRE(recovery_bash);
    CHECK(recovery_bash->message.exit_code == 0);

    auto prompt_result = run_prompt(runtime, session, "ordinary prompt");
    REQUIRE(prompt_result);
    CHECK(client_pointer->requests.size() == 1);
}

TEST_CASE(
    "repeated User Bash cancellation coalesces and a later command gets a fresh stop source",
    "[coding_agent][runtime][issue88]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto client = std::make_shared<ImmediateChatProvider>();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"held"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });
    shell_pointer->enqueue({
        .updates = {"second run"},
        .result = {.exit_code = 42},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    auto created = runtime.run(make_session_async(runtime, workspace.path(), std::move(client), std::move(shell)));
    REQUIRE(created);
    auto& session = runtime.adopt_session(std::move(created->session));

    std::optional<support::Expected<coding_agent::runtime::UserBashCompletion>> first;
    const auto first_scenario = run_on_runtime(runtime, [&]() -> boost::asio::awaitable<support::ExpectedVoid> {
        const auto executor = co_await boost::asio::this_coro::executor;
        auto operation = boost::asio::co_spawn(executor,
                coding_agent::detail::AgentSessionInteractiveAccess::run_user_bash(session,
                        "first command",
                        false,
                        [](const coding_agent::runtime::UserBashProgress&) { return support::ExpectedVoid{}; }),
                boost::asio::use_awaitable);
        co_await wait_until([&] { return shell_pointer->started_count == 1; });
        coding_agent::detail::AgentSessionInteractiveAccess::cancel_user_bash(session);
        coding_agent::detail::AgentSessionInteractiveAccess::cancel_user_bash(session);
        first.emplace(co_await std::move(operation));
        co_return support::ExpectedVoid{};
    });
    REQUIRE(first_scenario);

    // Repeated cancellation reaches the Shell stop source exactly once.
    CHECK(shell_pointer->cancellation_request_count == 1);
    REQUIRE(first.has_value());
    REQUIRE(*first);
    CHECK(first->value().message.cancelled);
    CHECK(bash_message_count(session.snapshot().agent_state.messages) == 1);

    // Cancellation after completion is a no-op. Run it on the fixture Runtime
    // loop so the Session owner remains serialized.
    const auto post_completion_cancel =
            run_on_runtime(runtime, [&session]() -> boost::asio::awaitable<support::ExpectedVoid> {
                coding_agent::detail::AgentSessionInteractiveAccess::cancel_user_bash(session);
                co_return support::ExpectedVoid{};
            });
    REQUIRE(post_completion_cancel);
    CHECK(shell_pointer->cancellation_request_count == 1);

    // The next command runs with a fresh stop source: releasing the gate
    // completes with the real exit code instead of a stale cancellation.
    std::optional<support::Expected<coding_agent::runtime::UserBashCompletion>> second;
    const auto second_scenario = run_on_runtime(runtime, [&]() -> boost::asio::awaitable<support::ExpectedVoid> {
        const auto executor = co_await boost::asio::this_coro::executor;
        auto operation = boost::asio::co_spawn(executor,
                coding_agent::detail::AgentSessionInteractiveAccess::run_user_bash(session,
                        "second command",
                        false,
                        [](const coding_agent::runtime::UserBashProgress&) { return support::ExpectedVoid{}; }),
                boost::asio::use_awaitable);
        co_await wait_until([&] { return shell_pointer->started_count == 2; });
        shell_pointer->release();
        second.emplace(co_await std::move(operation));
        co_return support::ExpectedVoid{};
    });
    REQUIRE(second_scenario);
    REQUIRE(second.has_value());
    REQUIRE(*second);
    CHECK_FALSE(second->value().message.cancelled);
    CHECK(second->value().message.exit_code == 42);
    CHECK(bash_message_count(session.snapshot().agent_state.messages) == 2);
}

TEST_CASE(
    "User Bash infrastructure failure commits no message and the Session stays usable",
    "[coding_agent][runtime][issue88]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto client = std::make_shared<ImmediateChatProvider>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {},
        .result = {},
        .infrastructure_failure = support::make_error(
            support::ErrorCode::Process,
            "shell spawn failed"),
        .gated = false,
    });
    shell_pointer->enqueue({
        .updates = {"after failure"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });

    auto created = runtime.run(make_session_async(runtime, workspace.path(), std::move(client), std::move(shell)));
    REQUIRE(created);
    auto& session = runtime.adopt_session(std::move(created->session));

    // Infrastructure failure is an explicit error, not a fake Bash outcome.
    auto failed = run_bash(runtime, session, "cannot spawn");
    CHECK_FALSE(failed);
    CHECK(bash_message_count(session.snapshot().agent_state.messages) == 0);
    CHECK(session.is_open());
    CHECK_FALSE(session.is_busy());

    auto recovered = run_bash(runtime, session, "works again");
    REQUIRE(recovered);
    CHECK(recovered->message.output == "after failure");
    CHECK(bash_message_count(session.snapshot().agent_state.messages) == 1);

    auto prompt_result = run_prompt(runtime, session, "prompt after failure");
    REQUIRE(prompt_result);
    CHECK(client_pointer->requests.size() == 1);
    CHECK(shell_pointer->started_count == 2);
}

TEST_CASE(
    "User Bash cancelled during an active run defers commitment without duplicating the outcome",
    "[coding_agent][runtime][issue88]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto client = std::make_shared<tests::GatedChatProvider>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"mid run partial"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    auto created = runtime.run(make_session_async(runtime, workspace.path(), std::move(client), std::move(shell)));
    REQUIRE(created);
    auto& session = runtime.adopt_session(std::move(created->session));

    std::optional<support::ExpectedVoid> prompt_result;
    std::optional<support::Expected<coding_agent::runtime::UserBashCompletion>> bash_result;
    bool bash_deferred_before_release = false;
    std::size_t messages_before_release = 0;
    const auto scenario = run_on_runtime(runtime, [&]() -> boost::asio::awaitable<support::ExpectedVoid> {
        const auto executor = co_await boost::asio::this_coro::executor;
        auto prompt = boost::asio::co_spawn(executor, session.prompt("long run"), boost::asio::use_awaitable);
        co_await wait_until([&] { return client_pointer->requests.size() == 1; });

        auto bash = boost::asio::co_spawn(executor,
                coding_agent::detail::AgentSessionInteractiveAccess::run_user_bash(session,
                        "mid run bash",
                        false,
                        [](const coding_agent::runtime::UserBashProgress&) { return support::ExpectedVoid{}; }),
                boost::asio::use_awaitable);
        co_await wait_until([&] { return shell_pointer->started_count == 1; });

        coding_agent::detail::AgentSessionInteractiveAccess::cancel_user_bash(session);
        bash_deferred_before_release = shell_pointer->cancellation_request_count == 1;
        messages_before_release = bash_message_count(session.snapshot().agent_state.messages);
        client_pointer->release();

        prompt_result.emplace(co_await std::move(prompt));
        bash_result.emplace(co_await std::move(bash));
        co_return support::ExpectedVoid{};
    });
    REQUIRE(scenario);

    // The cancelled outcome completed while the run was active: commitment
    // deferred to the run settle like any other mid-run result.
    CHECK(bash_deferred_before_release);
    CHECK(messages_before_release == 0);
    REQUIRE(prompt_result.has_value());
    CHECK(*prompt_result);
    REQUIRE(bash_result.has_value());
    REQUIRE(*bash_result);
    CHECK(bash_result->value().message.cancelled);
    CHECK_FALSE(bash_result->value().message.exit_code.has_value());
    CHECK(bash_result->value().message.output == "mid run partial");
    // Exactly one Bash message landed after the run settled, in order.
    const auto& messages = session.snapshot().agent_state.messages;
    REQUIRE(messages.size() == 3);
    CHECK(bash_message_count(messages) == 1);
    CHECK(std::holds_alternative<ai::BashExecutionMessage>(messages[2]));
}

TEST_CASE(
    "Session Close rejects new work, cancels User Bash, and finalizes after quiescence",
    "[coding_agent][runtime][issue88]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    const auto session_path = workspace.path() / "close-bash.jsonl";
    auto client = std::make_shared<ImmediateChatProvider>();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"close partial"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    auto created = runtime.run(
            make_session_async(runtime, workspace.path(), std::move(client), std::move(shell), session_path));
    REQUIRE(created);
    auto& session = runtime.adopt_session(std::move(created->session));
    // Close releases the shell after quiescence; observe it through the
    // shared counters rather than the released fake.
    const auto shell_counters = shell_pointer->counters();

    std::optional<support::Expected<coding_agent::runtime::UserBashCompletion>> bash_result;
    std::optional<support::ExpectedVoid> rejected_prompt;
    std::optional<support::Expected<coding_agent::runtime::UserBashCompletion>> rejected_bash;
    bool closed_after_request = false;
    bool busy_after_request = false;
    std::size_t cancellations_after_request = 0;
    const auto scenario = run_on_runtime(runtime, [&]() -> boost::asio::awaitable<support::ExpectedVoid> {
        const auto executor = co_await boost::asio::this_coro::executor;
        auto bash = boost::asio::co_spawn(executor,
                coding_agent::detail::AgentSessionInteractiveAccess::run_user_bash(session,
                        "close me",
                        false,
                        [](const coding_agent::runtime::UserBashProgress&) { return support::ExpectedVoid{}; }),
                boost::asio::use_awaitable);
        co_await wait_until([&] { return shell_pointer->started_count == 1; });

        session.close();
        closed_after_request = !session.is_open();
        busy_after_request = session.is_busy();
        cancellations_after_request = shell_pointer->cancellation_request_count;

        // Close rejects new work while winding down.
        auto rejected_prompt_operation =
                boost::asio::co_spawn(executor, session.prompt("too late"), boost::asio::use_awaitable);
        auto rejected_bash_operation = boost::asio::co_spawn(executor,
                coding_agent::detail::AgentSessionInteractiveAccess::run_user_bash(session,
                        "also too late",
                        false,
                        [](const coding_agent::runtime::UserBashProgress&) { return support::ExpectedVoid{}; }),
                boost::asio::use_awaitable);
        rejected_prompt.emplace(co_await std::move(rejected_prompt_operation));
        rejected_bash.emplace(co_await std::move(rejected_bash_operation));
        // The cancelled outcome still commits exactly once before teardown.
        bash_result.emplace(co_await std::move(bash));
        co_await wait_until([&] { return !session.is_busy(); });
        co_return support::ExpectedVoid{};
    });
    REQUIRE(scenario);
    CHECK(closed_after_request);
    CHECK(busy_after_request);
    CHECK(cancellations_after_request == 1);
    REQUIRE(rejected_prompt.has_value());
    CHECK_FALSE(*rejected_prompt);
    REQUIRE(rejected_bash.has_value());
    CHECK_FALSE(*rejected_bash);
    CHECK(shell_counters->started_count == 1);
    CHECK(shell_counters->cancellation_request_count == 1);

    REQUIRE(bash_result.has_value());
    REQUIRE(*bash_result);
    CHECK(bash_result->value().message.cancelled);
    CHECK(bash_result->value().message.output == "close partial");
    CHECK_FALSE(session.is_busy());

    auto resumed = harness::session::resume_session(session_path);
    REQUIRE(resumed);
    CHECK(bash_message_count(resumed->history) == 1);
    const auto* persisted =
        std::get_if<ai::BashExecutionMessage>(&resumed->history.back());
    REQUIRE(persisted != nullptr);
    CHECK(persisted->cancelled);
    CHECK(persisted->output == "close partial");
}
