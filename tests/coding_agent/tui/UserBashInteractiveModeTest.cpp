#include <cch/ai/ChatClient.hpp>
#include <cch/ai/Content.hpp>
#include <cch/coding_agent/Sdk.hpp>
#include <cch/harness/session/SessionResume.hpp>
#include <cch/tui/VirtualTerminal.hpp>
#include "coding_agent/AgentSessionBridge.hpp"
#include "coding_agent/BoundedText.hpp"
#include "coding_agent/runtime/AgentSessionInteractiveAccess.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include "support/FakeUserShell.hpp"
#include "support/TempWorkspace.hpp"

#include "../../../third_party/catch2/catch_test_macros.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

class RecordingChatClient final : public ai::StreamingChatClient {
public:
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& request,
        ai::AssistantEventSink) override {
        requests.push_back(request);
        auto response = ai::assistant_text_message("provider reply");
        response.api = "fake";
        response.provider = "fake";
        response.model = request.model->id;
        co_return response;
    }

    std::vector<ai::StreamChatRequest> requests;
};

void drain_ready(boost::asio::io_context& io) {
    if (io.stopped()) io.restart();
    while (io.poll() != 0) {
    }
}

[[nodiscard]] std::string visible_screen(const tui::VirtualTerminal& terminal) {
    std::string text;
    for (const auto& line : terminal.screen()) {
        text += line;
        text.push_back('\n');
    }
    return text;
}

[[nodiscard]] bool has_bash_command(
    const std::vector<ai::MessageVariant>& messages,
    std::string_view command) {
    return std::any_of(messages.begin(), messages.end(), [command](const auto& message) {
        const auto* bash = std::get_if<ai::BashExecutionMessage>(&message);
        return bash != nullptr && bash->command == command;
    });
}

} // namespace

TEST_CASE(
    "focused User Bash commits included and excluded results through the private composition",
    "[coding_agent][tui][issue85]") {
    tests::TempWorkspace workspace;
    auto client = std::make_unique<RecordingChatClient>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"first update", "\nsecond update"},
        .result = {
            .output = "included output",
            .exit_code = 0,
            .full_output_path = std::nullopt,
            .artifact_error = std::nullopt,
        },
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });
    shell_pointer->enqueue({
        .updates = {"local update"},
        .result = {
            .output = "excluded output",
            .exit_code = 7,
            .full_output_path = std::nullopt,
            .artifact_error = std::nullopt,
        },
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });

    coding_agent::CreateAgentSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.chat_client = std::move(client);
    options.builtin_tools = {
        .read = false,
        .write = false,
        .edit_file = false,
        .bash = false,
    };
    auto created = coding_agent::create_agent_session(
        std::move(options), std::move(shell));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 72, .rows = 40});
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *created->session,
            terminal,
            {.agent_config_directory = workspace.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            REQUIRE(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(terminal.inject_input("!  echo included  \r"));
    drain_ready(io);
    REQUIRE(shell_pointer->commands.size() == 1);
    CHECK(shell_pointer->commands[0] == "echo included");
    const auto running_screen = visible_screen(terminal);
    const auto first_update = running_screen.find("first update");
    const auto second_update = running_screen.find("second update");
    REQUIRE(first_update != std::string::npos);
    CHECK(second_update > first_update);
    CHECK(created->session->snapshot().agent_state.messages.empty());

    shell_pointer->release();
    drain_ready(io);
    auto snapshot = created->session->snapshot();
    REQUIRE(snapshot.agent_state.messages.size() == 1);
    const auto* included = std::get_if<ai::BashExecutionMessage>(
        &snapshot.agent_state.messages[0]);
    REQUIRE(included != nullptr);
    CHECK(included->command == "echo included");
    CHECK(included->output == "included output");
    CHECK(included->exit_code == 0);
    CHECK_FALSE(included->exclude_from_context);
    CHECK(visible_screen(terminal).find("included output") != std::string::npos);

    REQUIRE(terminal.inject_input("!! echo excluded\r"));
    drain_ready(io);
    snapshot = created->session->snapshot();
    REQUIRE(snapshot.agent_state.messages.size() == 2);
    const auto* excluded = std::get_if<ai::BashExecutionMessage>(
        &snapshot.agent_state.messages[1]);
    REQUIRE(excluded != nullptr);
    CHECK(excluded->command == "echo excluded");
    CHECK(excluded->output == "excluded output");
    CHECK(excluded->exit_code == 7);
    CHECK(excluded->exclude_from_context);

    REQUIRE(terminal.inject_input("later prompt\r"));
    drain_ready(io);
    REQUIRE(client_pointer->requests.size() == 1);
    const auto& provider_messages = client_pointer->requests[0].context.messages;
    CHECK(has_bash_command(provider_messages, "echo included"));
    CHECK_FALSE(has_bash_command(provider_messages, "echo excluded"));
    CHECK(client_pointer->requests[0].context.tools.empty());

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "only non-empty focused editor prefixes enter the private User Shell path",
    "[coding_agent][tui][issue85]") {
    tests::TempWorkspace workspace;
    auto client = std::make_unique<RecordingChatClient>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();

    coding_agent::CreateAgentSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.chat_client = std::move(client);
    options.builtin_tools = {
        .read = false,
        .write = false,
        .edit_file = false,
        .bash = false,
    };
    auto created = coding_agent::create_agent_session(
        std::move(options), std::move(shell));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 72, .rows = 30});
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *created->session,
            terminal,
            {
                .agent_config_directory = workspace.path(),
                .initial_prompt = "! positional input",
            }),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            REQUIRE(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);
    REQUIRE(client_pointer->requests.size() == 1);
    CHECK(shell_pointer->commands.empty());

    REQUIRE(terminal.inject_input("!!\r"));
    drain_ready(io);
    REQUIRE(client_pointer->requests.size() == 2);
    CHECK(shell_pointer->commands.empty());

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "User Bash sanitizes retained values and preserves an artifact-failed outcome",
    "[coding_agent][tui][issue85]") {
    tests::TempWorkspace workspace;
    const auto session_path = workspace.path() / "user-bash.jsonl";
    auto client = std::make_unique<RecordingChatClient>();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    const std::string secret = "sk-abcdefghijklmnopqrstuvwxyz123456";
    shell_pointer->enqueue({
        .updates = {"\x1b[31mlive api_key=" + secret + "\rupdate\x1b[0m"},
        .result = {
            .output = std::string(60 * 1024, 'x') + "\r\napi_key=" + secret,
            .exit_code = 0,
            .cancelled = false,
            .truncated = true,
            .full_output_path = "/tmp/api_key=" + secret,
            .artifact_error = util::make_error(
                util::ErrorCode::Process,
                "artifact write failed",
                "api_key=" + secret),
        },
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });

    coding_agent::CreateAgentSessionOptions options;
    options.session_target = coding_agent::ExplicitNewSessionTarget{session_path};
    options.workspace = workspace.path();
    options.chat_client = std::move(client);
    options.builtin_tools = {
        .read = false,
        .write = false,
        .edit_file = false,
        .bash = false,
    };
    auto created = coding_agent::create_agent_session(
        std::move(options), std::move(shell));
    REQUIRE(created);

    std::size_t lifecycle_events = 0;
    auto subscription = created->session->subscribe(
        [&](const agent::AgentLifecycleEvent&) -> util::ExpectedVoid {
            ++lifecycle_events;
            return {};
        });
    REQUIRE(subscription);

    tui::VirtualTerminal terminal({.columns = 72, .rows = 40});
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *created->session,
            terminal,
            {.agent_config_directory = workspace.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            REQUIRE(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    const auto raw_command = "printf api_key=" + secret;
    REQUIRE(terminal.inject_input("! " + raw_command + "\r"));
    drain_ready(io);
    REQUIRE(shell_pointer->commands.size() == 1);
    CHECK(shell_pointer->commands[0] == raw_command);
    CHECK(lifecycle_events == 0);

    const auto snapshot = created->session->snapshot();
    REQUIRE(snapshot.agent_state.messages.size() == 1);
    const auto* bash = std::get_if<ai::BashExecutionMessage>(
        &snapshot.agent_state.messages[0]);
    REQUIRE(bash != nullptr);
    CHECK(bash->command.find(secret) == std::string::npos);
    CHECK(bash->command.find("[REDACTED]") != std::string::npos);
    CHECK(bash->output.find(secret) == std::string::npos);
    CHECK(bash->output.find("[REDACTED]") != std::string::npos);
    CHECK(bash->output.size() <= 50 * 1024);
    CHECK(bash->truncated);
    CHECK_FALSE(bash->full_output_path.has_value());
    const auto screen = visible_screen(terminal);
    CHECK(screen.find(secret) == std::string::npos);
    CHECK(screen.find("artifact write failed") != std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    REQUIRE(*run_result);

    std::ifstream persisted{session_path, std::ios::binary};
    const std::string persisted_text{
        std::istreambuf_iterator<char>(persisted),
        std::istreambuf_iterator<char>()};
    CHECK(persisted_text.find(secret) == std::string::npos);
    auto resumed = harness::session::resume_session(session_path);
    REQUIRE(resumed);
    REQUIRE(resumed->history.size() == 1);
    CHECK(std::get<ai::BashExecutionMessage>(resumed->history[0]).command == bash->command);
}

TEST_CASE(
    "User Shell infrastructure failure creates no Bash message and leaves the Session usable",
    "[coding_agent][tui][issue85]") {
    tests::TempWorkspace workspace;
    auto client = std::make_unique<RecordingChatClient>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    const std::string secret = "sk-abcdefghijklmnopqrstuvwxyz123456";
    shell->enqueue({
        .updates = {},
        .result = {
            .output = {},
            .exit_code = 0,
            .full_output_path = std::nullopt,
            .artifact_error = std::nullopt,
        },
        .infrastructure_failure = util::make_error(
            util::ErrorCode::Process,
            "spawn failed",
            "api_key=" + secret),
        .gated = false,
    });

    coding_agent::CreateAgentSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.chat_client = std::move(client);
    options.builtin_tools = {
        .read = false,
        .write = false,
        .edit_file = false,
        .bash = false,
    };
    auto created = coding_agent::create_agent_session(
        std::move(options), std::move(shell));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 72, .rows = 30});
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *created->session,
            terminal,
            {.agent_config_directory = workspace.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            REQUIRE(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(terminal.inject_input("! fail\r"));
    drain_ready(io);
    CHECK(created->session->snapshot().agent_state.messages.empty());
    auto screen = visible_screen(terminal);
    CHECK(screen.find("spawn failed") != std::string::npos);
    CHECK(screen.find(secret) == std::string::npos);

    REQUIRE(terminal.inject_input("\x03later prompt\r"));
    drain_ready(io);
    REQUIRE(client_pointer->requests.size() == 1);
    CHECK(created->session->snapshot().agent_state.messages.size() == 2);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "User Shell progress callback failure creates no Bash message and leaves the Session usable",
    "[coding_agent][runtime][issue85]") {
    tests::TempWorkspace workspace;
    auto client = std::make_unique<RecordingChatClient>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    const std::string secret = "sk-abcdefghijklmnopqrstuvwxyz123456";
    shell->enqueue({
        .updates = {"callback update api_key=" + secret},
        .result = {
            .output = "must not be committed",
            .exit_code = 0,
            .full_output_path = std::nullopt,
            .artifact_error = std::nullopt,
        },
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });

    coding_agent::CreateAgentSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.chat_client = std::move(client);
    options.builtin_tools = {
        .read = false,
        .write = false,
        .edit_file = false,
        .bash = false,
    };
    auto created = coding_agent::create_agent_session(
        std::move(options), std::move(shell));
    REQUIRE(created);

    boost::asio::io_context io;
    std::optional<util::Expected<coding_agent::runtime::UserBashCompletion>> completion;
    boost::asio::co_spawn(
        io,
        coding_agent::detail::AgentSessionInteractiveAccess::run_user_bash(
            *created->session,
            "callback failure",
            false,
            [&secret](const coding_agent::runtime::UserBashProgress& progress)
                -> util::ExpectedVoid {
                if (progress.output.empty()) return {};
                return std::unexpected(util::make_error(
                    util::ErrorCode::Process,
                    "progress rejected",
                    "api_key=" + secret + " " +
                        std::string(coding_agent::kMaxPresentationPayloadBytes * 2, 'x')));
            }),
        [&](std::exception_ptr exception,
            util::Expected<coding_agent::runtime::UserBashCompletion> result) {
            REQUIRE(exception == nullptr);
            completion.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(completion);
    REQUIRE_FALSE(*completion);
    CHECK(created->session->snapshot().agent_state.messages.empty());
    CHECK(completion->error().message == "progress rejected");
    CHECK(completion->error().detail.size() <=
        coding_agent::kMaxPresentationPayloadBytes);
    CHECK(completion->error().detail.find(secret) == std::string::npos);
    CHECK(completion->error().detail.find("[REDACTED]") != std::string::npos);

    std::optional<util::ExpectedVoid> prompt_result;
    boost::asio::co_spawn(
        io,
        created->session->prompt("later prompt"),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            REQUIRE(exception == nullptr);
            prompt_result.emplace(std::move(result));
        });
    drain_ready(io);
    REQUIRE(prompt_result);
    CHECK(*prompt_result);
    CHECK(client_pointer->requests.size() == 1);
    CHECK(created->session->snapshot().agent_state.messages.size() == 2);
}

TEST_CASE(
    "private User Bash cancellation commits one cancelled terminal outcome without events",
    "[coding_agent][runtime][issue85]") {
    tests::TempWorkspace workspace;
    auto client = std::make_unique<RecordingChatClient>();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"partial output"},
        .result = {
            .output = "partial output",
            .exit_code = 0,
            .full_output_path = std::nullopt,
            .artifact_error = std::nullopt,
        },
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    coding_agent::CreateAgentSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.chat_client = std::move(client);
    options.builtin_tools = {
        .read = false,
        .write = false,
        .edit_file = false,
        .bash = false,
    };
    auto created = coding_agent::create_agent_session(
        std::move(options), std::move(shell));
    REQUIRE(created);

    std::size_t lifecycle_events = 0;
    auto subscription = created->session->subscribe(
        [&](const agent::AgentLifecycleEvent&) -> util::ExpectedVoid {
            ++lifecycle_events;
            return {};
        });
    REQUIRE(subscription);

    boost::asio::io_context io;
    std::optional<util::Expected<coding_agent::runtime::UserBashCompletion>> completion;
    boost::asio::co_spawn(
        io,
        coding_agent::detail::AgentSessionInteractiveAccess::run_user_bash(
            *created->session,
            "long command",
            false,
            [](const coding_agent::runtime::UserBashProgress&) {
                return util::ExpectedVoid{};
            }),
        [&](std::exception_ptr exception,
            util::Expected<coding_agent::runtime::UserBashCompletion> result) {
            REQUIRE(exception == nullptr);
            completion.emplace(std::move(result));
        });
    drain_ready(io);
    CHECK(shell_pointer->started_count == 1);

    coding_agent::detail::AgentSessionInteractiveAccess::cancel_user_bash(
        *created->session);
    drain_ready(io);
    REQUIRE(completion);
    REQUIRE(*completion);
    CHECK(shell_pointer->cancellation_request_count == 1);
    CHECK(lifecycle_events == 0);

    const auto snapshot = created->session->snapshot();
    REQUIRE(snapshot.agent_state.messages.size() == 1);
    const auto& bash = std::get<ai::BashExecutionMessage>(
        snapshot.agent_state.messages[0]);
    CHECK(bash.cancelled);
    CHECK_FALSE(bash.exit_code.has_value());
    CHECK(bash.output == "partial output");
}
