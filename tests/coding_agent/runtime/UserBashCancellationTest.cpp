#include <cch/ai/ChatClient.hpp>
#include <cch/ai/Content.hpp>
#include <cch/coding_agent/Sdk.hpp>
#include <cch/harness/session/SessionResume.hpp>
#include "coding_agent/AgentSessionBridge.hpp"
#include "coding_agent/runtime/AgentSessionInteractiveAccess.hpp"
#include "support/FakeUserShell.hpp"
#include "support/GatedChatClient.hpp"
#include "support/TempWorkspace.hpp"
#include "support/UserBashTestHooks.hpp"

#include "../../../third_party/catch2/catch_test_macros.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <cstddef>
#include <optional>
#include <string>
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

/// Answers every provider request immediately so cancellation recovery paths
/// can run without gate choreography.
class ImmediateChatClient final : public ai::StreamingChatClient {
public:
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& request,
        ai::AssistantEventSink) override {
        requests.push_back(request);
        auto response = ai::assistant_text_message("immediate reply");
        response.provider = "immediate-fake";
        response.api = "fake";
        response.model = request.model->id;
        co_return response;
    }

    std::vector<ai::StreamChatRequest> requests;
};

[[nodiscard]] util::Expected<coding_agent::CreateAgentSessionResult> make_session(
    const std::filesystem::path& workspace,
    std::unique_ptr<ai::StreamingChatClient> client,
    std::unique_ptr<tests::FakeUserShell> shell,
    std::optional<std::filesystem::path> session_path = std::nullopt) {
    coding_agent::CreateAgentSessionOptions options;
    if (session_path) {
        options.session_target =
            coding_agent::ExplicitNewSessionTarget{*session_path};
    } else {
        options.session_target = coding_agent::InMemorySessionTarget{};
    }
    options.workspace = workspace;
    options.chat_client = std::move(client);
    options.builtin_tools = {
        .read = false,
        .write = false,
        .edit_file = false,
        .bash = false,
    };
    return coding_agent::create_agent_session(std::move(options), std::move(shell));
}

} // namespace

TEST_CASE(
    "idle User Bash cancellation retains partial output and commits one cancelled message",
    "[coding_agent][runtime][issue88]") {
    tests::TempWorkspace workspace;
    const auto session_path = workspace.path() / "cancel-idle.jsonl";
    auto client = std::make_unique<ImmediateChatClient>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"first chunk", "\nsecond chunk"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    auto created = make_session(
        workspace.path(), std::move(client), std::move(shell), session_path);
    REQUIRE(created);
    auto& session = *created->session;

    boost::asio::io_context io;
    BashResult bash_result;
    spawn_bash(io, session, "echo cancel me", bash_result);
    drain_ready(io);
    REQUIRE(shell_pointer->started_count == 1);

    coding_agent::detail::AgentSessionInteractiveAccess::cancel_user_bash(session);
    drain_ready(io);

    // The cancelled terminal outcome retains the already delivered sanitized
    // output, carries no exit code, and commits exactly once.
    REQUIRE(bash_result.has_value());
    REQUIRE(*bash_result);
    CHECK((*bash_result)->message.cancelled);
    CHECK_FALSE((*bash_result)->message.exit_code.has_value());
    CHECK((*bash_result)->message.output == "first chunk\nsecond chunk");
    CHECK((*bash_result)->message.command == "echo cancel me");
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
    BashResult recovery_bash;
    spawn_bash(io, session, "echo recovered", recovery_bash);
    drain_ready(io);
    REQUIRE(recovery_bash.has_value());
    REQUIRE(*recovery_bash);
    CHECK((*recovery_bash)->message.exit_code == 0);

    PromptResult prompt_result;
    spawn_prompt(io, session, "ordinary prompt", prompt_result);
    drain_ready(io);
    REQUIRE(prompt_result.has_value());
    CHECK(*prompt_result);
    CHECK(client_pointer->requests.size() == 1);
    session.close();
}

TEST_CASE(
    "repeated User Bash cancellation coalesces and a later command gets a fresh stop source",
    "[coding_agent][runtime][issue88]") {
    tests::TempWorkspace workspace;
    auto client = std::make_unique<ImmediateChatClient>();
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

    auto created = make_session(workspace.path(), std::move(client), std::move(shell));
    REQUIRE(created);
    auto& session = *created->session;

    boost::asio::io_context io;
    BashResult first;
    spawn_bash(io, session, "first command", first);
    drain_ready(io);
    REQUIRE(shell_pointer->started_count == 1);

    coding_agent::detail::AgentSessionInteractiveAccess::cancel_user_bash(session);
    coding_agent::detail::AgentSessionInteractiveAccess::cancel_user_bash(session);
    drain_ready(io);
    // Repeated cancellation reaches the Shell stop source exactly once.
    CHECK(shell_pointer->cancellation_request_count == 1);
    REQUIRE(first.has_value());
    REQUIRE(*first);
    CHECK((*first)->message.cancelled);
    CHECK(bash_message_count(session.snapshot().agent_state.messages) == 1);

    // Cancellation after completion is a no-op.
    coding_agent::detail::AgentSessionInteractiveAccess::cancel_user_bash(session);
    drain_ready(io);
    CHECK(shell_pointer->cancellation_request_count == 1);

    // The next command runs with a fresh stop source: releasing the gate
    // completes with the real exit code instead of a stale cancellation.
    BashResult second;
    spawn_bash(io, session, "second command", second);
    drain_ready(io);
    REQUIRE(shell_pointer->started_count == 2);
    shell_pointer->release();
    drain_ready(io);
    REQUIRE(second.has_value());
    REQUIRE(*second);
    CHECK_FALSE((*second)->message.cancelled);
    CHECK((*second)->message.exit_code == 42);
    CHECK(bash_message_count(session.snapshot().agent_state.messages) == 2);
    session.close();
}

TEST_CASE(
    "User Bash infrastructure failure commits no message and the Session stays usable",
    "[coding_agent][runtime][issue88]") {
    tests::TempWorkspace workspace;
    auto client = std::make_unique<ImmediateChatClient>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {},
        .result = {},
        .infrastructure_failure = util::make_error(
            util::ErrorCode::Process,
            "shell spawn failed"),
        .gated = false,
    });
    shell_pointer->enqueue({
        .updates = {"after failure"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });

    auto created = make_session(workspace.path(), std::move(client), std::move(shell));
    REQUIRE(created);
    auto& session = *created->session;

    boost::asio::io_context io;
    BashResult failed;
    spawn_bash(io, session, "cannot spawn", failed);
    drain_ready(io);
    // Infrastructure failure is an explicit error, not a fake Bash outcome.
    REQUIRE(failed.has_value());
    CHECK_FALSE(*failed);
    CHECK(bash_message_count(session.snapshot().agent_state.messages) == 0);
    CHECK(session.is_open());
    CHECK_FALSE(session.is_busy());

    BashResult recovered;
    spawn_bash(io, session, "works again", recovered);
    drain_ready(io);
    REQUIRE(recovered.has_value());
    REQUIRE(*recovered);
    CHECK((*recovered)->message.output == "after failure");
    CHECK(bash_message_count(session.snapshot().agent_state.messages) == 1);

    PromptResult prompt_result;
    spawn_prompt(io, session, "prompt after failure", prompt_result);
    drain_ready(io);
    REQUIRE(prompt_result.has_value());
    CHECK(*prompt_result);
    CHECK(client_pointer->requests.size() == 1);
    session.close();
}

TEST_CASE(
    "User Bash cancelled during an active run defers commitment without duplicating the outcome",
    "[coding_agent][runtime][issue88]") {
    tests::TempWorkspace workspace;
    auto client = std::make_unique<tests::GatedChatClient>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"mid run partial"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    auto created = make_session(workspace.path(), std::move(client), std::move(shell));
    REQUIRE(created);
    auto& session = *created->session;

    boost::asio::io_context io;
    PromptResult prompt_result;
    BashResult bash_result;
    spawn_prompt(io, session, "long run", prompt_result);
    drain_ready(io);
    REQUIRE(client_pointer->requests.size() == 1);
    spawn_bash(io, session, "mid run bash", bash_result);
    drain_ready(io);
    REQUIRE(shell_pointer->started_count == 1);

    coding_agent::detail::AgentSessionInteractiveAccess::cancel_user_bash(session);
    drain_ready(io);
    // The cancelled outcome completed but the run is active: commitment
    // defers to the run settle like any other mid-run result.
    CHECK(shell_pointer->cancellation_request_count == 1);
    CHECK(bash_message_count(session.snapshot().agent_state.messages) == 0);
    REQUIRE_FALSE(bash_result.has_value());

    client_pointer->release();
    drain_ready(io);
    REQUIRE(prompt_result.has_value());
    CHECK(*prompt_result);
    REQUIRE(bash_result.has_value());
    REQUIRE(*bash_result);
    CHECK((*bash_result)->message.cancelled);
    CHECK_FALSE((*bash_result)->message.exit_code.has_value());
    CHECK((*bash_result)->message.output == "mid run partial");
    // Exactly one Bash message landed after the run settled, in order.
    const auto& messages = session.snapshot().agent_state.messages;
    REQUIRE(messages.size() == 3);
    CHECK(bash_message_count(messages) == 1);
    CHECK(std::holds_alternative<ai::BashExecutionMessage>(messages[2]));
    session.close();
}

TEST_CASE(
    "Session Close rejects new work, cancels User Bash, and finalizes after quiescence",
    "[coding_agent][runtime][issue88]") {
    tests::TempWorkspace workspace;
    const auto session_path = workspace.path() / "close-bash.jsonl";
    auto client = std::make_unique<ImmediateChatClient>();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"close partial"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    auto created = make_session(
        workspace.path(), std::move(client), std::move(shell), session_path);
    REQUIRE(created);
    auto& session = *created->session;

    boost::asio::io_context io;
    BashResult bash_result;
    spawn_bash(io, session, "close me", bash_result);
    drain_ready(io);
    REQUIRE(shell_pointer->started_count == 1);

    session.close();
    CHECK_FALSE(session.is_open());
    // The Shell cancellation was requested but close retains the store and
    // capabilities until the active operation quiesces.
    CHECK(session.is_busy());
    CHECK(shell_pointer->cancellation_request_count == 1);

    // Close rejects new work while winding down.
    PromptResult rejected_prompt;
    spawn_prompt(io, session, "too late", rejected_prompt);
    BashResult rejected_bash;
    spawn_bash(io, session, "also too late", rejected_bash);
    drain_ready(io);
    REQUIRE(rejected_prompt.has_value());
    CHECK_FALSE(*rejected_prompt);
    REQUIRE(rejected_bash.has_value());
    CHECK_FALSE(*rejected_bash);
    CHECK(shell_pointer->started_count == 1);

    drain_ready(io);
    // The cancelled outcome still commits exactly once before teardown.
    REQUIRE(bash_result.has_value());
    REQUIRE(*bash_result);
    CHECK((*bash_result)->message.cancelled);
    CHECK((*bash_result)->message.output == "close partial");
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
