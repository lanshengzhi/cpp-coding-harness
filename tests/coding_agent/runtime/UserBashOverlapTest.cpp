#include "ai/ModelStreamBridge.hpp"
#include <cch/ai/Content.hpp>
#include "coding_agent/AgentSession.hpp"
#include <cch/agent/harness/session/SessionResume.hpp>
#include "coding_agent/runtime/AgentSessionInteractiveAccess.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "agent/harness/session/SessionJournalTestHooks.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/FakeUserShell.hpp"
#include "support/GatedChatProvider.hpp"
#include "support/ModelsFixture.hpp"
#include "support/ReleaseGate.hpp"
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
#include <filesystem>
#include <fstream>
#include <format>
#include <chrono>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

using tests::bash_message_count;

[[nodiscard]] ai::TimestampMs now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

/// First request answers with a read tool call; the post-tool-result request
/// gates until release(); later requests answer immediately.
class ToolCallThenGatedProvider final : public tests::ScriptedProvider {
public:
    explicit ToolCallThenGatedProvider(std::string read_path)
        : ScriptedProvider("sdk-host"),
          read_path_(std::move(read_path)) {}

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext context, coding_agent::ModelRuntimeTestStreamOptions options) override {
        return ai::detail::make_model_stream(
            [this, model = std::move(model), context = std::move(context), options = std::move(options)](
                ai::AssistantEventSink) mutable
                -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
        requests.push_back(tests::RecordedProviderRequest{model, context, options});
        auto response = ai::assistant_text_message("tool cycle complete");
        response.provider = "tool-gated-fake";
        response.api = "fake";
        response.model = model.id;
        response.timestamp = now_ms();

        if (requests.size() == 1) {
            support::JsonValue::object_t arguments;
            arguments.emplace("path", support::JsonValue{read_path_});
            response.content.clear();
            response.content.emplace_back(ai::text_content("reading file"));
            response.content.emplace_back(ai::tool_call_content(
                "fake-read-1",
                "read",
                std::format(R"({{"path":"{}"}})", read_path_),
                support::JsonValue{std::move(arguments)}));
            response.stop_reason = ai::AssistantStopReason::ToolUse;
            co_return response;
        }

        if (requests.size() == 2) {
            co_await gate_.wait();
        }
        co_return response;
                });
    }

    void release() {
        gate_.release();
    }

    std::vector<tests::RecordedProviderRequest> requests;

private:
    std::string read_path_;
    tests::ReleaseGate gate_;
};

/// Read a small journal file and report whether it carries `needle`.
[[nodiscard]] bool journal_contains(const std::filesystem::path& path, std::string_view needle) {
    std::ifstream file(path);
    const std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    return text.contains(needle);
}

[[nodiscard]] bool context_has_bash_command(
    const tests::RecordedProviderRequest& request,
    std::string_view command) {
    for (const auto& message : request.context.messages) {
        const auto* bash = std::get_if<ai::BashExecutionMessage>(&message);
        if (bash != nullptr && bash->command == command) return true;
    }
    return false;
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

[[nodiscard]] support::AsyncResult<coding_agent::CreateAgentSessionResult> make_session_async(
        tests::RuntimeFixture& runtime,
        const std::filesystem::path& workspace,
        std::shared_ptr<tests::ScriptedProvider> client,
        std::unique_ptr<tests::FakeUserShell> shell,
        std::optional<std::filesystem::path> session_path = std::nullopt) {
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_target =
            session_path ? coding_agent::SessionTarget{coding_agent::ExplicitOpenOrCreateSessionTarget{*session_path}}
                         : coding_agent::SessionTarget{coding_agent::InMemorySessionTarget{}};
    request.workspace = workspace;
    request.execution_runtime_target = runtime.make_target();
    return coding_agent::create_agent_session_async(std::move(request),
            std::nullopt,
            coding_agent::runtime::AssemblyOverrides{.model_runtime = nullptr,
                    .cli_fake = false,
                    .models = tests::models_from_provider(std::move(client)),
                    .user_shell = std::move(shell)});
}

} // namespace

TEST_CASE(
    "User Bash completed during an Agent run commits once after the whole run settles",
    "[coding_agent][runtime][issue87]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto client = std::make_shared<tests::GatedChatProvider>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"overlap output"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    auto created = runtime.run(make_session_async(runtime, workspace.path(), std::move(client), std::move(shell)));
    REQUIRE(created);
    auto& session = runtime.adopt_session(std::move(created->session));

    std::optional<support::ExpectedVoid> prompt_result;
    std::optional<support::Expected<coding_agent::runtime::UserBashCompletion>> bash_result;
    std::size_t messages_before_run_settle = 0;
    bool steering_succeeded = false;
    bool second_request_omits_bash = false;
    ai::TimestampMs before_completion = 0;
    const auto scenario = run_on_runtime(runtime, [&]() -> boost::asio::awaitable<support::ExpectedVoid> {
        const auto executor = co_await boost::asio::this_coro::executor;
        tests::spawn_to_slot(executor, session.prompt("start run"), prompt_result);
        co_await wait_until([&] { return client_pointer->requests.size() == 1; });

        steering_succeeded = static_cast<bool>(session.steer("steer input"));

        tests::spawn_to_slot(executor,
                coding_agent::detail::AgentSessionInteractiveAccess::run_user_bash(session,
                        "during run",
                        false,
                        [](const coding_agent::runtime::UserBashProgress&) { return support::ExpectedVoid{}; }),
                bash_result);
        co_await wait_until([&] { return shell_pointer->started_count == 1; });

        before_completion = now_ms();
        shell_pointer->release();
        messages_before_run_settle = bash_message_count(session.snapshot().agent_state.messages);

        client_pointer->release();
        co_await wait_until([&] { return client_pointer->requests.size() == 2; });
        second_request_omits_bash = !context_has_bash_command(client_pointer->requests[1], "during run");

        client_pointer->release();
        co_await wait_until([&] { return prompt_result.has_value(); });
        co_await wait_until([&] { return bash_result.has_value(); });
        co_return support::ExpectedVoid{};
    });
    REQUIRE(scenario);
    CHECK(steering_succeeded);
    CHECK(messages_before_run_settle == 0);
    CHECK(second_request_omits_bash);
    REQUIRE(prompt_result.has_value());
    CHECK(*prompt_result);
    REQUIRE(bash_result.has_value());
    REQUIRE(*bash_result);
    CHECK(bash_result->value().message.command == "during run");
    CHECK(bash_result->value().message.output == "overlap output");
    // The timestamp records process completion, not the delayed commitment.
    CHECK(bash_result->value().message.timestamp >= before_completion);
    CHECK(bash_result->value().message.timestamp <= now_ms());

    const auto& messages = session.snapshot().agent_state.messages;
    REQUIRE(messages.size() == 5);
    CHECK(std::holds_alternative<ai::UserMessage>(messages[0]));
    CHECK(std::holds_alternative<ai::AssistantMessage>(messages[1]));
    CHECK(std::holds_alternative<ai::UserMessage>(messages[2]));
    CHECK(std::holds_alternative<ai::AssistantMessage>(messages[3]));
    const auto* bash = std::get_if<ai::BashExecutionMessage>(&messages[4]);
    REQUIRE(bash != nullptr);
    CHECK(bash->command == "during run");
}

TEST_CASE(
    "an ordinary Prompt is admitted during active User Bash and orders deterministically",
    "[coding_agent][runtime][issue87]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto client = std::make_shared<tests::GatedChatProvider>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"bash first"},
        .result = {.exit_code = 3},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    auto created = runtime.run(make_session_async(runtime, workspace.path(), std::move(client), std::move(shell)));
    REQUIRE(created);
    auto& session = runtime.adopt_session(std::move(created->session));

    std::optional<support::ExpectedVoid> prompt_result;
    std::optional<support::Expected<coding_agent::runtime::UserBashCompletion>> bash_result;
    std::size_t messages_before_bash_release = 0;
    const auto scenario = run_on_runtime(runtime, [&]() -> boost::asio::awaitable<support::ExpectedVoid> {
        const auto executor = co_await boost::asio::this_coro::executor;
        tests::spawn_to_slot(executor,
                coding_agent::detail::AgentSessionInteractiveAccess::run_user_bash(session,
                        "first bash",
                        false,
                        [](const coding_agent::runtime::UserBashProgress&) { return support::ExpectedVoid{}; }),
                bash_result);
        co_await wait_until([&] { return shell_pointer->started_count == 1; });

        tests::spawn_to_slot(executor, session.prompt("during bash"), prompt_result);
        co_await wait_until([&] { return client_pointer->requests.size() == 1; });

        client_pointer->release();
        co_await wait_until([&] { return prompt_result.has_value(); });
        messages_before_bash_release = bash_message_count(session.snapshot().agent_state.messages);

        shell_pointer->release();
        co_await wait_until([&] { return bash_result.has_value(); });
        co_return support::ExpectedVoid{};
    });
    REQUIRE(scenario);
    REQUIRE(prompt_result.has_value());
    CHECK(*prompt_result);
    // The run settled while Bash was still active: nothing committed yet.
    CHECK(messages_before_bash_release == 0);

    REQUIRE(bash_result.has_value());
    REQUIRE(*bash_result);
    CHECK(bash_result->value().message.exit_code == 3);

    const auto& messages = session.snapshot().agent_state.messages;
    REQUIRE(messages.size() == 3);
    CHECK(std::holds_alternative<ai::UserMessage>(messages[0]));
    CHECK(std::holds_alternative<ai::AssistantMessage>(messages[1]));
    const auto* bash = std::get_if<ai::BashExecutionMessage>(&messages[2]);
    REQUIRE(bash != nullptr);
    CHECK(bash->command == "first bash");
}

TEST_CASE(
    "a second User Bash is rejected before runtime mutation and the Session stays usable",
    "[coding_agent][runtime][issue87]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto client = std::make_shared<tests::GatedChatProvider>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"first"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });
    shell_pointer->enqueue({
        .updates = {"second"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });

    auto created = runtime.run(make_session_async(runtime, workspace.path(), std::move(client), std::move(shell)));
    REQUIRE(created);
    auto& session = runtime.adopt_session(std::move(created->session));

    std::optional<support::Expected<coding_agent::runtime::UserBashCompletion>> first_result;
    std::optional<support::Expected<coding_agent::runtime::UserBashCompletion>> second_result;
    std::optional<support::ExpectedVoid> prompt_result;
    std::size_t commands_before_release = 0;
    std::size_t messages_before_release = 0;
    const auto scenario = run_on_runtime(runtime, [&]() -> boost::asio::awaitable<support::ExpectedVoid> {
        const auto executor = co_await boost::asio::this_coro::executor;
        tests::spawn_to_slot(executor,
                coding_agent::detail::AgentSessionInteractiveAccess::run_user_bash(session,
                        "first bash",
                        false,
                        [](const coding_agent::runtime::UserBashProgress&) { return support::ExpectedVoid{}; }),
                first_result);
        co_await wait_until([&] { return shell_pointer->started_count == 1; });

        tests::spawn_to_slot(executor,
                coding_agent::detail::AgentSessionInteractiveAccess::run_user_bash(session,
                        "second bash",
                        false,
                        [](const coding_agent::runtime::UserBashProgress&) { return support::ExpectedVoid{}; }),
                second_result);
        co_await wait_until([&] { return second_result.has_value(); });
        commands_before_release = shell_pointer->commands.size();
        messages_before_release = session.snapshot().agent_state.messages.size();

        shell_pointer->release();
        co_await wait_until([&] { return first_result.has_value(); });

        tests::spawn_to_slot(executor, session.prompt("after rejection"), prompt_result);
        co_await wait_until([&] { return client_pointer->requests.size() == 1; });
        client_pointer->release();
        co_await wait_until([&] { return prompt_result.has_value(); });
        co_return support::ExpectedVoid{};
    });
    REQUIRE(scenario);
    REQUIRE(second_result.has_value());
    REQUIRE_FALSE(*second_result);
    CHECK(second_result->error().message.find("User Bash") != std::string::npos);
    // Rejected before mutation: the shell never saw the second command and
    // Live Session State is untouched.
    CHECK(commands_before_release == 1);
    CHECK(messages_before_release == 0);

    REQUIRE(first_result.has_value());
    REQUIRE(*first_result);
    REQUIRE(prompt_result.has_value());
    CHECK(*prompt_result);
    CHECK(client_pointer->requests.size() == 1);
}

TEST_CASE(
    "deferred User Bash commits exactly once through JSONL and resumes in Session order",
    "[coding_agent][runtime][issue87]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    const auto session_path = workspace.path() / "overlap.jsonl";
    auto client = std::make_shared<tests::GatedChatProvider>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"persisted overlap"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    auto created = runtime.run(
            make_session_async(runtime, workspace.path(), std::move(client), std::move(shell), session_path));
    REQUIRE(created);
    auto& session = runtime.adopt_session(std::move(created->session));

    std::optional<support::ExpectedVoid> prompt_result;
    std::optional<support::Expected<coding_agent::runtime::UserBashCompletion>> bash_result;
    std::size_t messages_before_run_settle = 0;
    const auto scenario = run_on_runtime(runtime, [&]() -> boost::asio::awaitable<support::ExpectedVoid> {
        const auto executor = co_await boost::asio::this_coro::executor;
        tests::spawn_to_slot(executor, session.prompt("persisted run"), prompt_result);
        co_await wait_until([&] { return client_pointer->requests.size() == 1; });

        tests::spawn_to_slot(executor,
                coding_agent::detail::AgentSessionInteractiveAccess::run_user_bash(session,
                        "deferred bash",
                        false,
                        [](const coding_agent::runtime::UserBashProgress&) { return support::ExpectedVoid{}; }),
                bash_result);
        co_await wait_until([&] { return shell_pointer->started_count == 1; });
        shell_pointer->release();
        messages_before_run_settle = bash_message_count(session.snapshot().agent_state.messages);
        client_pointer->release();
        co_await wait_until([&] { return prompt_result.has_value(); });
        co_await wait_until([&] { return bash_result.has_value(); });
        // The deferred commitment's JSONL append crosses a Runtime
        // persistence worker; wait for the journal to carry the Bash entry
        // instead of assuming the completion callback implies the write
        // landed (#531).
        co_await wait_until([&] { return journal_contains(session_path, "deferred bash"); });
        session.close();
        co_await wait_until([&] { return !session.is_open() && !session.is_busy(); });
        co_return support::ExpectedVoid{};
    });
    REQUIRE(scenario);
    REQUIRE(prompt_result.has_value());
    CHECK(*prompt_result);
    CHECK(messages_before_run_settle == 0);
    REQUIRE(bash_result.has_value());
    REQUIRE(*bash_result);

    auto resumed = harness::session::resume_session(session_path);
    REQUIRE(resumed);
    const auto& history = resumed->history;
    REQUIRE(history.size() == 3);
    CHECK(std::holds_alternative<ai::UserMessage>(history[0]));
    CHECK(std::holds_alternative<ai::AssistantMessage>(history[1]));
    const auto* bash = std::get_if<ai::BashExecutionMessage>(&history[2]);
    REQUIRE(bash != nullptr);
    CHECK(bash->command == "deferred bash");
    CHECK(bash->output == "persisted overlap");
    CHECK(bash_message_count(history) == 1);
}

TEST_CASE(
    "pending User Bash never splits a tool-call/tool-result sequence",
    "[coding_agent][runtime][issue87]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    workspace.write("note.txt", "note contents");
    const auto note_path = (workspace.path() / "note.txt").string();
    auto client = std::make_shared<ToolCallThenGatedProvider>(note_path);
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"mid-run output"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });

    auto created = runtime.run(make_session_async(runtime, workspace.path(), std::move(client), std::move(shell)));
    REQUIRE(created);
    auto& session = runtime.adopt_session(std::move(created->session));

    std::optional<support::ExpectedVoid> first_prompt;
    std::optional<support::Expected<coding_agent::runtime::UserBashCompletion>> bash_result;
    std::optional<support::ExpectedVoid> second_prompt;
    std::vector<ai::MessageVariant> settled_messages;
    std::size_t messages_before_run_settle = 0;
    bool bash_deferred_before_run_settle = false;
    bool first_context_omits_bash = false;
    const auto scenario = run_on_runtime(runtime, [&]() -> boost::asio::awaitable<support::ExpectedVoid> {
        const auto executor = co_await boost::asio::this_coro::executor;
        tests::spawn_to_slot(executor, session.prompt("read the note"), first_prompt);
        // Filesystem completion is worker-backed, so wait until its tool
        // result advances the provider to the gated second request.
        co_await wait_until([&] { return client_pointer->requests.size() == 2; });

        tests::spawn_to_slot(executor,
                coding_agent::detail::AgentSessionInteractiveAccess::run_user_bash(session,
                        "mid-run bash",
                        false,
                        [](const coding_agent::runtime::UserBashProgress&) { return support::ExpectedVoid{}; }),
                bash_result);
        co_await wait_until([&] { return shell_pointer->started_count == 1; });
        // Ungated Bash completed during the run but stays uncommitted, and
        // the in-flight turn's context never saw it.
        messages_before_run_settle = bash_message_count(session.snapshot().agent_state.messages);
        bash_deferred_before_run_settle = messages_before_run_settle == 0;
        first_context_omits_bash = !context_has_bash_command(client_pointer->requests[1], "mid-run bash");

        client_pointer->release();
        co_await wait_until([&] { return first_prompt.has_value(); });
        co_await wait_until([&] { return bash_result.has_value(); });

        co_await wait_until([&] { return session.snapshot().agent_state.messages.size() == 5; });
        // The five-message settled state is pinned before the follow-up
        // prompt appends to it (the post-scenario assertion reads this
        // snapshot, not the then-current live state).
        settled_messages = session.snapshot().agent_state.messages;
        tests::spawn_to_slot(executor, session.prompt("after overlap"), second_prompt);
        co_await wait_until([&] { return second_prompt.has_value(); });
        co_return support::ExpectedVoid{};
    });
    REQUIRE(scenario);
    CHECK(messages_before_run_settle == 0);
    CHECK(bash_deferred_before_run_settle);
    CHECK(first_context_omits_bash);
    REQUIRE(first_prompt.has_value());
    CHECK(*first_prompt);
    REQUIRE(bash_result.has_value());
    REQUIRE(*bash_result);

    const auto& messages = settled_messages;
    REQUIRE(messages.size() == 5);
    CHECK(std::holds_alternative<ai::UserMessage>(messages[0]));
    const auto* tool_call_turn = std::get_if<ai::AssistantMessage>(&messages[1]);
    REQUIRE(tool_call_turn != nullptr);
    CHECK(std::holds_alternative<ai::ToolResultMessage>(messages[2]));
    CHECK(std::holds_alternative<ai::AssistantMessage>(messages[3]));
    const auto* bash = std::get_if<ai::BashExecutionMessage>(&messages[4]);
    REQUIRE(bash != nullptr);
    CHECK(bash->command == "mid-run bash");

    REQUIRE(second_prompt.has_value());
    CHECK(*second_prompt);
    REQUIRE(client_pointer->requests.size() == 3);
    // The idle Prompt's context carries the flushed Bash after the completed
    // tool-call/tool-result pair and before the new user message.
    const auto& context = client_pointer->requests[2].context.messages;
    REQUIRE(context.size() == 6);
    CHECK(std::holds_alternative<ai::UserMessage>(context[0]));
    CHECK(std::holds_alternative<ai::AssistantMessage>(context[1]));
    CHECK(std::holds_alternative<ai::ToolResultMessage>(context[2]));
    CHECK(std::holds_alternative<ai::AssistantMessage>(context[3]));
    CHECK(std::holds_alternative<ai::BashExecutionMessage>(context[4]));
    CHECK(std::holds_alternative<ai::UserMessage>(context[5]));
}

TEST_CASE(
    "deferred User Bash persistence failure is reported without rolling back Live Session State",
    "[coding_agent][runtime][issue87]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    const auto session_path = workspace.path() / "overlap-failure.jsonl";
    auto client = std::make_shared<tests::GatedChatProvider>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"unpersisted overlap"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    auto created = runtime.run(
            make_session_async(runtime, workspace.path(), std::move(client), std::move(shell), session_path));
    REQUIRE(created);
    auto& session = runtime.adopt_session(std::move(created->session));

    std::optional<support::ExpectedVoid> prompt_result;
    std::optional<support::Expected<coding_agent::runtime::UserBashCompletion>> bash_result;
    std::optional<support::ExpectedVoid> second_prompt;
    std::size_t messages_before_release = 0;
    bool bash_committed_live = false;
    const auto scenario = run_on_runtime(runtime, [&]() -> boost::asio::awaitable<support::ExpectedVoid> {
        const auto executor = co_await boost::asio::this_coro::executor;
        tests::spawn_to_slot(executor, session.prompt("failing persist run"), prompt_result);
        co_await wait_until([&] { return client_pointer->requests.size() == 1; });

        tests::spawn_to_slot(executor,
                coding_agent::detail::AgentSessionInteractiveAccess::run_user_bash(session,
                        "unpersisted bash",
                        false,
                        [](const coding_agent::runtime::UserBashProgress&) { return support::ExpectedVoid{}; }),
                bash_result);
        co_await wait_until([&] { return shell_pointer->started_count == 1; });
        shell_pointer->release();
        messages_before_release = bash_message_count(session.snapshot().agent_state.messages);

        // The deferred Bash commitment must be the second append after the
        // run's user entry (then the assistant entry), and appends cross
        // Runtime persistence workers: wait until the user entry has
        // landed in the journal so the counted failure pins exactly the
        // Bash write no matter when the earlier append lands (#531).
        co_await wait_until([&] { return journal_contains(session_path, "failing persist run"); });
        harness::session::testing::fail_nth_append_for_test(session_path, 2);
        client_pointer->release();
        co_await wait_until([&] { return prompt_result.has_value(); });
        co_await wait_until([&] { return bash_result.has_value(); });
        co_await wait_until([&] { return bash_message_count(session.snapshot().agent_state.messages) == 1; });
        bash_committed_live = true;

        // The Session stays usable: a later prompt runs and persists again.
        tests::spawn_to_slot(executor, session.prompt("still usable"), second_prompt);
        co_await wait_until([&] { return client_pointer->requests.size() == 2; });
        client_pointer->release();
        co_await wait_until([&] { return second_prompt.has_value(); });
        session.close();
        co_await wait_until([&] { return !session.is_open() && !session.is_busy(); });
        co_return support::ExpectedVoid{};
    });
    REQUIRE(scenario);
    CHECK(messages_before_release == 0);
    REQUIRE(prompt_result.has_value());
    CHECK(*prompt_result);
    REQUIRE(bash_result.has_value());
    REQUIRE(*bash_result);
    // Live Session State advanced; only persistence failed, reported explicitly.
    // (snapshot() reads the owned Agent, which Close releases — the live-state
    // observation is captured inside the scenario before Close.)
    REQUIRE(bash_result->value().diagnostic.has_value());
    CHECK(bash_committed_live);
    REQUIRE(second_prompt.has_value());
    CHECK(*second_prompt);
    CHECK(client_pointer->requests.size() == 2);

    auto resumed = harness::session::resume_session(session_path);
    REQUIRE(resumed);
    CHECK(bash_message_count(resumed->history) == 0);
}

TEST_CASE(
    "Session Close during an active run commits a pending User Bash before teardown",
    "[coding_agent][runtime][issue87]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    const auto session_path = workspace.path() / "close-overlap.jsonl";
    auto client = std::make_shared<tests::GatedChatProvider>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"close overlap"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    auto created = runtime.run(
            make_session_async(runtime, workspace.path(), std::move(client), std::move(shell), session_path));
    REQUIRE(created);
    auto& session = runtime.adopt_session(std::move(created->session));

    std::optional<support::ExpectedVoid> prompt_result;
    std::optional<support::Expected<coding_agent::runtime::UserBashCompletion>> bash_result;
    std::size_t messages_before_close = 0;
    bool busy_after_close = false;
    const auto scenario = run_on_runtime(runtime, [&]() -> boost::asio::awaitable<support::ExpectedVoid> {
        const auto executor = co_await boost::asio::this_coro::executor;
        tests::spawn_to_slot(executor, session.prompt("close during run"), prompt_result);
        co_await wait_until([&] { return client_pointer->requests.size() == 1; });

        tests::spawn_to_slot(executor,
                coding_agent::detail::AgentSessionInteractiveAccess::run_user_bash(session,
                        "pending at close",
                        false,
                        [](const coding_agent::runtime::UserBashProgress&) { return support::ExpectedVoid{}; }),
                bash_result);
        co_await wait_until([&] { return shell_pointer->started_count == 1; });
        shell_pointer->release();
        messages_before_close = bash_message_count(session.snapshot().agent_state.messages);

        // Close cancels the active run but must still commit the deferred
        // Bash through Live Session State and the Session Store before
        // teardown.
        session.close();
        busy_after_close = session.is_busy();
        client_pointer->release();
        co_await wait_until([&] { return prompt_result.has_value(); });
        co_await wait_until([&] { return bash_result.has_value(); });
        // Close finalization settles through Runtime hops behind the
        // completion callbacks; wait for quiescence instead of asserting
        // after one drain pass (#531).
        co_await wait_until([&] { return !session.is_open() && !session.is_busy(); });
        co_return support::ExpectedVoid{};
    });
    REQUIRE(scenario);
    CHECK(messages_before_close == 0);
    CHECK(busy_after_close);
    REQUIRE(prompt_result.has_value());
    CHECK(*prompt_result);
    REQUIRE(bash_result.has_value());
    REQUIRE(*bash_result);
    CHECK(bash_result->value().message.command == "pending at close");
    CHECK_FALSE(session.is_open());
    CHECK_FALSE(session.is_busy());

    auto resumed = harness::session::resume_session(session_path);
    REQUIRE(resumed);
    REQUIRE_FALSE(resumed->history.empty());
    CHECK(bash_message_count(resumed->history) == 1);
    const auto* bash =
        std::get_if<ai::BashExecutionMessage>(&resumed->history.back());
    REQUIRE(bash != nullptr);
    CHECK(bash->command == "pending at close");
}

TEST_CASE(
    "Session Close cancels an overlapping User Bash and finalizes after the last work settles",
    "[coding_agent][runtime][issue87]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto client = std::make_shared<tests::GatedChatProvider>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"partial"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    auto created = runtime.run(make_session_async(runtime, workspace.path(), std::move(client), std::move(shell)));
    REQUIRE(created);
    auto& session = runtime.adopt_session(std::move(created->session));
    const auto shell_counters = shell_pointer->counters();

    std::optional<support::ExpectedVoid> prompt_result;
    std::optional<support::Expected<coding_agent::runtime::UserBashCompletion>> bash_result;
    std::size_t messages_before_close = 0;
    bool busy_after_close = false;
    ai::TimestampMs before_close = 0;
    const auto scenario = run_on_runtime(runtime, [&]() -> boost::asio::awaitable<support::ExpectedVoid> {
        const auto executor = co_await boost::asio::this_coro::executor;
        tests::spawn_to_slot(executor, session.prompt("run"), prompt_result);
        co_await wait_until([&] { return client_pointer->requests.size() == 1; });

        tests::spawn_to_slot(executor,
                coding_agent::detail::AgentSessionInteractiveAccess::run_user_bash(session,
                        "overlapping bash",
                        false,
                        [](const coding_agent::runtime::UserBashProgress&) { return support::ExpectedVoid{}; }),
                bash_result);
        co_await wait_until([&] { return shell_pointer->started_count == 1; });

        before_close = now_ms();
        session.close();
        // Cancellation propagates to the shell through the stop_token
        // across Runtime hops; wait for the observable cancellation.
        co_await wait_until([&] { return shell_pointer->cancellation_request_count == 1; });
        messages_before_close = bash_message_count(session.snapshot().agent_state.messages);
        busy_after_close = session.is_busy();

        // Close cancelled the Bash while the run was still active: the
        // cancelled completion defers to the run's settle like any other
        // mid-run result.
        client_pointer->release();
        co_await wait_until([&] { return prompt_result.has_value(); });
        co_await wait_until([&] { return bash_result.has_value(); });
        // Close finalization settles through Runtime hops behind the
        // completion callbacks; wait for quiescence instead of asserting
        // after one drain pass (#531).
        co_await wait_until([&] { return !session.is_open() && !session.is_busy(); });
        co_return support::ExpectedVoid{};
    });
    REQUIRE(scenario);
    CHECK(shell_counters->cancellation_request_count == 1);
    CHECK(messages_before_close == 0);
    CHECK(busy_after_close);
    REQUIRE(prompt_result.has_value());
    CHECK(*prompt_result);
    REQUIRE(bash_result.has_value());
    REQUIRE(*bash_result);
    // The cancelled terminal outcome is committed exactly once, timestamped at
    // the observed cancellation rather than the deferred commitment.
    CHECK(bash_result->value().message.cancelled);
    CHECK(bash_result->value().message.timestamp >= before_close);
    CHECK(bash_result->value().message.timestamp <= now_ms());
    CHECK_FALSE(session.is_open());
    CHECK_FALSE(session.is_busy());
}
