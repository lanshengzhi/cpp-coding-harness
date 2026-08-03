#include <cch/ai/ChatClient.hpp>
#include "support/ModelsFixture.hpp"
#include <cch/ai/Content.hpp>
#include <cch/coding_agent/Sdk.hpp>
#include <cch/harness/session/SessionResume.hpp>
#include "coding_agent/AgentSessionBridge.hpp"
#include "coding_agent/runtime/AgentSessionInteractiveAccess.hpp"
#include "harness/session/SessionJournalTestHooks.hpp"
#include "support/FakeUserShell.hpp"
#include "support/GatedChatClient.hpp"
#include "support/TempWorkspace.hpp"
#include "support/UserBashTestHooks.hpp"

#include "../../../third_party/catch2/catch_test_macros.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

using tests::drain_ready;
using tests::bash_message_count;
using tests::BashResult;
using tests::PromptResult;
using tests::spawn_bash;
using tests::spawn_prompt;

[[nodiscard]] ai::TimestampMs now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

/// First request answers with a read tool call; the post-tool-result request
/// gates until release(); later requests answer immediately.
class ToolCallThenGatedChatClient final : public ai::StreamingChatClient {
public:
    explicit ToolCallThenGatedChatClient(std::string read_path)
        : read_path_(std::move(read_path)) {}

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& request,
        ai::AssistantEventSink) override {
        requests.push_back(request);
        auto response = ai::assistant_text_message("tool cycle complete");
        response.provider = "tool-gated-fake";
        response.api = "fake";
        response.model = request.model.id;
        response.timestamp = now_ms();

        if (requests.size() == 1) {
            util::JsonValue::object_t arguments;
            arguments.emplace("path", util::JsonValue{read_path_});
            response.content.clear();
            response.content.emplace_back(ai::text_content("reading file"));
            response.content.emplace_back(ai::tool_call_content(
                "fake-read-1",
                "read",
                std::format(R"({{"path":"{}"}})", read_path_),
                util::JsonValue{std::move(arguments)}));
            response.stop_reason = ai::AssistantStopReason::ToolUse;
            co_return response;
        }

        if (requests.size() == 2) {
            const auto executor = co_await boost::asio::this_coro::executor;
            gate_.emplace(executor);
            gate_->expires_at(std::chrono::steady_clock::time_point::max());
            boost::system::error_code error;
            co_await gate_->async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, error));
            gate_.reset();
        }
        co_return response;
    }

    void release() {
        if (gate_) (void)gate_->cancel();
    }

    std::vector<ai::StreamChatRequest> requests;

private:
    std::string read_path_;
    std::optional<boost::asio::steady_timer> gate_;
};

[[nodiscard]] bool context_has_bash_command(
    const ai::StreamChatRequest& request,
    std::string_view command) {
    for (const auto& message : request.context.messages) {
        const auto* bash = std::get_if<ai::BashExecutionMessage>(&message);
        if (bash != nullptr && bash->command == command) return true;
    }
    return false;
}

} // namespace

TEST_CASE(
    "User Bash completed during an Agent run commits once after the whole run settles",
    "[coding_agent][runtime][issue87]") {
    tests::TempWorkspace workspace;
    auto client = std::make_unique<tests::GatedChatClient>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"overlap output"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_stream(std::move(client));
    options.builtin_tools = {
        .read = false,
        .write = false,
        .edit_file = false,
        .bash = false,
    };
    auto created = coding_agent::create_agent_session(
        std::move(options), std::move(shell));
    REQUIRE(created);
    auto& session = *created->session;

    boost::asio::io_context io;
    PromptResult prompt_result;
    BashResult bash_result;
    spawn_prompt(io, session, "start run", prompt_result);
    drain_ready(io);
    REQUIRE(client_pointer->requests.size() == 1);

    REQUIRE(session.steer("steer input"));

    spawn_bash(io, session, "during run", bash_result);
    drain_ready(io);
    REQUIRE(shell_pointer->started_count == 1);

    const auto before_completion = now_ms();
    shell_pointer->release();
    drain_ready(io);
    // The execution finished but the run is active: no commitment, no return.
    CHECK(bash_message_count(session.snapshot().agent_state.messages) == 0);
    REQUIRE_FALSE(bash_result.has_value());

    client_pointer->release();
    drain_ready(io);
    // The steering continuation keeps the run active; Bash stays pending.
    REQUIRE(client_pointer->requests.size() == 2);
    CHECK(bash_message_count(session.snapshot().agent_state.messages) == 0);
    REQUIRE_FALSE(bash_result.has_value());
    CHECK_FALSE(context_has_bash_command(client_pointer->requests[1], "during run"));

    client_pointer->release();
    drain_ready(io);
    REQUIRE(prompt_result.has_value());
    CHECK(*prompt_result);
    REQUIRE(bash_result.has_value());
    REQUIRE(*bash_result);
    CHECK((*bash_result)->message.command == "during run");
    CHECK((*bash_result)->message.output == "overlap output");
    // The timestamp records process completion, not the delayed commitment.
    CHECK((*bash_result)->message.timestamp >= before_completion);
    CHECK((*bash_result)->message.timestamp <= now_ms());

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
    auto client = std::make_unique<tests::GatedChatClient>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"bash first"},
        .result = {.exit_code = 3},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_stream(std::move(client));
    options.builtin_tools = {
        .read = false,
        .write = false,
        .edit_file = false,
        .bash = false,
    };
    auto created = coding_agent::create_agent_session(
        std::move(options), std::move(shell));
    REQUIRE(created);
    auto& session = *created->session;

    boost::asio::io_context io;
    PromptResult prompt_result;
    BashResult bash_result;
    spawn_bash(io, session, "first bash", bash_result);
    drain_ready(io);
    REQUIRE(shell_pointer->started_count == 1);

    spawn_prompt(io, session, "during bash", prompt_result);
    drain_ready(io);
    REQUIRE(client_pointer->requests.size() == 1);

    client_pointer->release();
    drain_ready(io);
    REQUIRE(prompt_result.has_value());
    CHECK(*prompt_result);
    // The run settled while Bash was still active: nothing committed yet.
    CHECK(bash_message_count(session.snapshot().agent_state.messages) == 0);
    REQUIRE_FALSE(bash_result.has_value());

    shell_pointer->release();
    drain_ready(io);
    REQUIRE(bash_result.has_value());
    REQUIRE(*bash_result);
    CHECK((*bash_result)->message.exit_code == 3);

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
    auto client = std::make_unique<tests::GatedChatClient>();
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

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_stream(std::move(client));
    options.builtin_tools = {
        .read = false,
        .write = false,
        .edit_file = false,
        .bash = false,
    };
    auto created = coding_agent::create_agent_session(
        std::move(options), std::move(shell));
    REQUIRE(created);
    auto& session = *created->session;

    boost::asio::io_context io;
    BashResult first_result;
    spawn_bash(io, session, "first bash", first_result);
    drain_ready(io);
    REQUIRE(shell_pointer->started_count == 1);

    BashResult second_result;
    spawn_bash(io, session, "second bash", second_result);
    drain_ready(io);
    REQUIRE(second_result.has_value());
    REQUIRE_FALSE(*second_result);
    CHECK(second_result->error().message.find("User Bash") != std::string::npos);
    // Rejected before mutation: the shell never saw the second command and
    // Live Session State is untouched.
    CHECK(shell_pointer->commands.size() == 1);
    CHECK(session.snapshot().agent_state.messages.empty());

    shell_pointer->release();
    drain_ready(io);
    REQUIRE(first_result.has_value());
    REQUIRE(*first_result);

    PromptResult prompt_result;
    spawn_prompt(io, session, "after rejection", prompt_result);
    drain_ready(io);
    client_pointer->release();
    drain_ready(io);
    REQUIRE(prompt_result.has_value());
    CHECK(*prompt_result);
    CHECK(client_pointer->requests.size() == 1);
}

TEST_CASE(
    "deferred User Bash commits exactly once through JSONL and resumes in Session order",
    "[coding_agent][runtime][issue87]") {
    tests::TempWorkspace workspace;
    const auto session_path = workspace.path() / "overlap.jsonl";
    auto client = std::make_unique<tests::GatedChatClient>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"persisted overlap"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::ExplicitNewSessionTarget{session_path};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_stream(std::move(client));
    options.builtin_tools = {
        .read = false,
        .write = false,
        .edit_file = false,
        .bash = false,
    };
    auto created = coding_agent::create_agent_session(
        std::move(options), std::move(shell));
    REQUIRE(created);
    auto& session = *created->session;

    boost::asio::io_context io;
    PromptResult prompt_result;
    BashResult bash_result;
    spawn_prompt(io, session, "persisted run", prompt_result);
    drain_ready(io);
    spawn_bash(io, session, "deferred bash", bash_result);
    drain_ready(io);
    shell_pointer->release();
    drain_ready(io);
    REQUIRE_FALSE(bash_result.has_value());
    client_pointer->release();
    drain_ready(io);
    REQUIRE(prompt_result.has_value());
    CHECK(*prompt_result);
    REQUIRE(bash_result.has_value());
    REQUIRE(*bash_result);
    session.close();

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
    workspace.write("note.txt", "note contents");
    const auto note_path = (workspace.path() / "note.txt").string();
    auto client = std::make_unique<ToolCallThenGatedChatClient>(note_path);
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"mid-run output"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_stream(std::move(client));
    options.builtin_tools = {
        .read = true,
        .write = false,
        .edit_file = false,
        .bash = false,
    };
    auto created = coding_agent::create_agent_session(
        std::move(options), std::move(shell));
    REQUIRE(created);
    auto& session = *created->session;

    boost::asio::io_context io;
    PromptResult first_prompt;
    spawn_prompt(io, session, "read the note", first_prompt);
    drain_ready(io);
    // Turn 1 produced the tool call; the run is held on the post-tool turn.
    REQUIRE(client_pointer->requests.size() == 2);

    BashResult bash_result;
    spawn_bash(io, session, "mid-run bash", bash_result);
    drain_ready(io);
    // Ungated Bash completed during the run but stays uncommitted, and the
    // in-flight turn's context never saw it.
    REQUIRE_FALSE(bash_result.has_value());
    CHECK(bash_message_count(session.snapshot().agent_state.messages) == 0);
    CHECK_FALSE(context_has_bash_command(client_pointer->requests[1], "mid-run bash"));

    client_pointer->release();
    drain_ready(io);
    REQUIRE(first_prompt.has_value());
    CHECK(*first_prompt);
    REQUIRE(bash_result.has_value());
    REQUIRE(*bash_result);

    const auto& messages = session.snapshot().agent_state.messages;
    REQUIRE(messages.size() == 5);
    CHECK(std::holds_alternative<ai::UserMessage>(messages[0]));
    const auto* tool_call_turn = std::get_if<ai::AssistantMessage>(&messages[1]);
    REQUIRE(tool_call_turn != nullptr);
    CHECK(std::holds_alternative<ai::ToolResultMessage>(messages[2]));
    CHECK(std::holds_alternative<ai::AssistantMessage>(messages[3]));
    const auto* bash = std::get_if<ai::BashExecutionMessage>(&messages[4]);
    REQUIRE(bash != nullptr);
    CHECK(bash->command == "mid-run bash");

    PromptResult second_prompt;
    spawn_prompt(io, session, "after overlap", second_prompt);
    drain_ready(io);
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
    const auto session_path = workspace.path() / "overlap-failure.jsonl";
    auto client = std::make_unique<tests::GatedChatClient>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"unpersisted overlap"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::ExplicitNewSessionTarget{session_path};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_stream(std::move(client));
    options.builtin_tools = {
        .read = false,
        .write = false,
        .edit_file = false,
        .bash = false,
    };
    auto created = coding_agent::create_agent_session(
        std::move(options), std::move(shell));
    REQUIRE(created);
    auto& session = *created->session;

    boost::asio::io_context io;
    PromptResult prompt_result;
    BashResult bash_result;
    spawn_prompt(io, session, "failing persist run", prompt_result);
    drain_ready(io);
    spawn_bash(io, session, "unpersisted bash", bash_result);
    drain_ready(io);
    shell_pointer->release();
    drain_ready(io);
    REQUIRE_FALSE(bash_result.has_value());

    // The next appends on this path are the run's assistant entry, then the
    // deferred Bash commitment; fail only the Bash write.
    harness::session::testing::fail_nth_append_for_test(session_path, 2);
    client_pointer->release();
    drain_ready(io);
    REQUIRE(prompt_result.has_value());
    CHECK(*prompt_result);
    REQUIRE(bash_result.has_value());
    REQUIRE(*bash_result);
    // Live Session State advanced; only persistence failed, reported explicitly.
    REQUIRE((*bash_result)->diagnostic.has_value());
    CHECK(bash_message_count(session.snapshot().agent_state.messages) == 1);

    // The Session stays usable: a later prompt runs and persists again.
    PromptResult second_prompt;
    spawn_prompt(io, session, "still usable", second_prompt);
    drain_ready(io);
    client_pointer->release();
    drain_ready(io);
    REQUIRE(second_prompt.has_value());
    CHECK(*second_prompt);
    CHECK(client_pointer->requests.size() == 2);
    session.close();

    auto resumed = harness::session::resume_session(session_path);
    REQUIRE(resumed);
    CHECK(bash_message_count(resumed->history) == 0);
}

TEST_CASE(
    "Session Close during an active run commits a pending User Bash before teardown",
    "[coding_agent][runtime][issue87]") {
    tests::TempWorkspace workspace;
    const auto session_path = workspace.path() / "close-overlap.jsonl";
    auto client = std::make_unique<tests::GatedChatClient>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"close overlap"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::ExplicitNewSessionTarget{session_path};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_stream(std::move(client));
    options.builtin_tools = {
        .read = false,
        .write = false,
        .edit_file = false,
        .bash = false,
    };
    auto created = coding_agent::create_agent_session(
        std::move(options), std::move(shell));
    REQUIRE(created);
    auto& session = *created->session;

    boost::asio::io_context io;
    PromptResult prompt_result;
    BashResult bash_result;
    spawn_prompt(io, session, "close during run", prompt_result);
    drain_ready(io);
    spawn_bash(io, session, "pending at close", bash_result);
    drain_ready(io);
    shell_pointer->release();
    drain_ready(io);
    REQUIRE_FALSE(bash_result.has_value());

    // Close cancels the active run but must still commit the deferred Bash
    // through Live Session State and the Session Store before teardown.
    session.close();
    CHECK(session.is_busy());
    client_pointer->release();
    drain_ready(io);
    REQUIRE(prompt_result.has_value());
    CHECK(*prompt_result);
    REQUIRE(bash_result.has_value());
    REQUIRE(*bash_result);
    CHECK((*bash_result)->message.command == "pending at close");
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
    auto client = std::make_unique<tests::GatedChatClient>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"partial"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_stream(std::move(client));
    options.builtin_tools = {
        .read = false,
        .write = false,
        .edit_file = false,
        .bash = false,
    };
    auto created = coding_agent::create_agent_session(
        std::move(options), std::move(shell));
    REQUIRE(created);
    auto& session = *created->session;

    boost::asio::io_context io;
    PromptResult prompt_result;
    BashResult bash_result;
    spawn_prompt(io, session, "run", prompt_result);
    drain_ready(io);
    spawn_bash(io, session, "overlapping bash", bash_result);
    drain_ready(io);
    REQUIRE(shell_pointer->started_count == 1);

    const auto before_close = now_ms();
    session.close();
    drain_ready(io);
    // Close cancelled the Bash while the run was still active: the cancelled
    // completion defers to the run's settle like any other mid-run result.
    CHECK(shell_pointer->cancellation_request_count == 1);
    REQUIRE_FALSE(bash_result.has_value());
    CHECK(session.is_busy());

    client_pointer->release();
    drain_ready(io);
    REQUIRE(prompt_result.has_value());
    CHECK(*prompt_result);
    REQUIRE(bash_result.has_value());
    REQUIRE(*bash_result);
    // The cancelled terminal outcome is committed exactly once, timestamped at
    // the observed cancellation rather than the deferred commitment.
    CHECK((*bash_result)->message.cancelled);
    CHECK((*bash_result)->message.timestamp >= before_close);
    CHECK((*bash_result)->message.timestamp <= now_ms());
    CHECK_FALSE(session.is_open());
    CHECK_FALSE(session.is_busy());
}
