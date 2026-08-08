#include "support/ModelsFixture.hpp"
#include <cch/ai/Content.hpp>
#include <cch/agent/AgentTool.hpp>
#include "coding_agent/AgentSession.hpp"
#include <cch/harness/session/JsonlSessionStore.hpp>
#include <cch/harness/session/SessionResume.hpp>
#include <cch/tui/VirtualTerminal.hpp>
#include "coding_agent/BoundedText.hpp"
#include "coding_agent/runtime/AgentSessionInteractiveAccess.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/Theme.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/FakeUserShell.hpp"
#include "support/GatedChatProvider.hpp"
#include "support/TempWorkspace.hpp"
#include "support/TextHelpers.hpp"
#include "util/Redactor.hpp"

#include "../../../third_party/catch2/catch_test_macros.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <set>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

using tests::count_occurrences;

class RecordingChatProvider final : public tests::ScriptedProvider {
public:
    RecordingChatProvider() : ScriptedProvider("sdk-host") {}
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink) override {
        requests.push_back(tests::RecordedProviderRequest{model, context, options});
        auto response = ai::assistant_text_message("provider reply");
        response.api = "fake";
        response.provider = "fake";
        response.model = model.id;
        co_return response;
    }

    std::vector<tests::RecordedProviderRequest> requests;
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

/// Extracts the leading SGR parameter list from a themed probe string.
[[nodiscard]] std::string sgr_params(const std::string& styled) {
    const auto begin = styled.find("\x1b[");
    REQUIRE(begin != std::string::npos);
    const auto end = styled.find('m', begin);
    REQUIRE(end != std::string::npos);
    return styled.substr(begin + 2, end - begin - 2);
}

[[nodiscard]] std::string fg_at_text(
    const tui::VirtualTerminal& terminal,
    std::string_view text) {
    const auto screen = terminal.screen();
    for (std::size_t row = 0; row < screen.size(); ++row) {
        const auto column = screen[row].find(text);
        if (column != std::string::npos) {
            REQUIRE(column < terminal.cells()[row].size());
            return terminal.cells()[row][column].style.fg_color;
        }
    }
    REQUIRE(false);
    return {};
}

/// First request emits partial output, gates, and answers cancellation with an
/// aborted outcome; later requests answer immediately so recovery is visible.
class AbortAwareGatedChatProvider final : public tests::ScriptedProvider {
public:
    AbortAwareGatedChatProvider() : ScriptedProvider("sdk-host") {}
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink sink) override {
        ++request_count;
        if (request_count > 1) {
            auto recovered = ai::assistant_text_message("recovery reply");
            recovered.provider = "abort-gated-fake";
            recovered.api = "fake";
            recovered.model = model.id;
            co_return recovered;
        }

        const auto stop_token = options.stop_token;
        auto partial = ai::assistant_text_message("");
        partial.provider = "abort-gated-fake";
        partial.api = "fake";
        partial.model = model.id;
        partial.content.clear();
        if (auto emitted = sink(ai::AssistantStartEvent{partial}); !emitted) {
            co_return std::unexpected(emitted.error());
        }
        partial.content.emplace_back(ai::text_content(""));
        if (auto emitted = sink(ai::TextStartEvent{
                .content_index = 0,
                .partial = partial,
            });
            !emitted) {
            co_return std::unexpected(emitted.error());
        }
        std::get<ai::TextContent>(partial.content[0]).text = "partial assistant output";
        if (auto emitted = sink(ai::TextDeltaEvent{
                .content_index = 0,
                .delta = "partial assistant output",
                .partial = partial,
            });
            !emitted) {
            co_return std::unexpected(emitted.error());
        }

        const auto executor = co_await boost::asio::this_coro::executor;
        gate_.emplace(executor);
        gate_->expires_at(std::chrono::steady_clock::time_point::max());
        started = true;
        std::stop_callback cancellation{stop_token, [this] {
            ++stop_callback_count;
            if (gate_) (void)gate_->cancel();
        }};
        boost::system::error_code error;
        if (!stop_token.stop_requested()) {
            co_await gate_->async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, error));
        }
        gate_.reset();

        if (stop_token.stop_requested()) {
            partial.stop_reason = ai::AssistantStopReason::Aborted;
            partial.error_message = "prompt aborted";
            if (auto emitted = sink(ai::AssistantErrorEvent{
                    .reason = ai::AssistantStopReason::Aborted,
                    .error = partial,
                });
                !emitted) {
                co_return std::unexpected(emitted.error());
            }
            co_return partial;
        }
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "abort-gated fake released without cancellation"));
    }

    bool started{false};
    std::size_t request_count{0};
    std::size_t stop_callback_count{0};

private:
    std::optional<boost::asio::steady_timer> gate_;
};

/// First request calls the gated "delayed" tool; later requests answer the
/// aborted settle path so a closing run can unwind deterministically.
class ToolCallThenAbortChatProvider final : public tests::ScriptedProvider {
public:
    ToolCallThenAbortChatProvider() : ScriptedProvider("sdk-host") {}
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink sink) override {
        ++request_count;
        if (options.stop_token.stop_requested() || request_count > 1) {
            auto aborted = ai::assistant_text_message("");
            aborted.provider = "tool-abort-fake";
            aborted.api = "fake";
            aborted.model = model.id;
            aborted.stop_reason = ai::AssistantStopReason::Aborted;
            aborted.error_message = "close prompt aborted";
            if (auto emitted = sink(ai::AssistantErrorEvent{
                    .reason = ai::AssistantStopReason::Aborted,
                    .error = aborted,
                });
                !emitted) {
                co_return std::unexpected(emitted.error());
            }
            co_return aborted;
        }

        auto response = ai::assistant_text_message("");
        response.provider = "tool-abort-fake";
        response.api = "fake";
        response.model = model.id;
        response.content.clear();
        response.content.emplace_back(ai::tool_call_content(
            "delayed-call",
            "delayed",
            "{}",
            util::JsonValue::object_t{}));
        response.stop_reason = ai::AssistantStopReason::ToolUse;
        co_return response;
    }

    std::size_t request_count{0};
};

/// Gates inside the tool until release() and records stop-token delivery.
class GatedCloseTool final : public agent::AsyncAgentTool {
public:
    GatedCloseTool() {
        definition_.name = "delayed";
        definition_.description = "Wait until close cancellation is observable";
        definition_.parameters = util::JsonValue::object_t{{"type", "object"}};
    }

    [[nodiscard]] const ai::Tool& definition() const override {
        return definition_;
    }

    [[nodiscard]] boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> execute(
        agent::ToolInvocation,
        std::stop_token) override {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Tool,
            "gated tool requires the update path"));
    }

    [[nodiscard]] boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>>
    execute_with_updates(
        agent::ToolInvocation,
        std::stop_token stop_token,
        agent::ToolUpdateSink) override {
        const auto executor = co_await boost::asio::this_coro::executor;
        gate_.emplace(executor);
        gate_->expires_at(std::chrono::steady_clock::time_point::max());
        started = true;
        std::stop_callback cancellation{stop_token, [this] {
            ++stop_callback_count;
        }};
        boost::system::error_code error;
        co_await gate_->async_wait(
            boost::asio::redirect_error(boost::asio::use_awaitable, error));
        gate_.reset();

        if (stop_token.stop_requested()) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Cancelled,
                "gated tool aborted by close"));
        }
        co_return agent::AsyncToolExecutionResult{};
    }

    void release() {
        if (gate_) (void)gate_->cancel();
    }

    bool started{false};
    std::size_t stop_callback_count{0};

private:
    ai::Tool definition_;
    std::optional<boost::asio::steady_timer> gate_;
};

} // namespace

TEST_CASE(
    "focused User Bash commits included and excluded results through the private composition",
    "[coding_agent][tui][issue85]") {
    tests::TempWorkspace workspace;
    auto client = std::make_shared<RecordingChatProvider>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"first update", "\nsecond update\nincluded output"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });
    shell_pointer->enqueue({
        .updates = {"excluded output"},
        .result = {.exit_code = 7},
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
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
    CHECK(included->output == "first update\nsecond update\nincluded output");
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
    // The fixed #331 tool set (read/write/edit/bash) is always registered.
    const auto& tools = client_pointer->requests[0].context.tools;
    REQUIRE(tools.size() == 4);
    std::set<std::string> tool_names;
    for (const auto& tool : tools) {
        tool_names.insert(tool.name);
    }
    CHECK((tool_names ==
           std::set<std::string>{"bash", "edit", "read", "write"}));

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "only non-empty focused editor prefixes enter the private User Shell path",
    "[coding_agent][tui][issue85]") {
    tests::TempWorkspace workspace;
    auto client = std::make_shared<RecordingChatProvider>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
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
    "User Bash sanitizes and bounds retained output and spills the complete sanitized stream",
    "[coding_agent][tui][issue85][issue86][issue96][issue97][issue99]") {
    tests::TempWorkspace workspace;
    const auto spill_dir = workspace.path() / "spill";
    std::filesystem::create_directories(spill_dir);
    tests::EnvVarGuard tmpdir{"TMPDIR", spill_dir.string()};
    const auto session_path = workspace.path() / "user-bash.jsonl";
    auto client = std::make_shared<RecordingChatProvider>();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    const std::string secret = "sk-abcdefghijklmnopqrstuvwxyz123456";
    const std::string bulk(60 * 1024, 'x');
    shell_pointer->enqueue({
        .updates = {
            "\x1b[31mlive api_",
            "key=" + secret.substr(0, 10),
            secret.substr(10) + "\rupdate\x1b[0m\n",
            bulk,
            "\r\napi_key=" + secret,
        },
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::ExplicitOpenOrCreateSessionTarget{session_path};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
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
    CHECK(bash->command == raw_command);
    // ADR 0028: output follows pi's recipe — no redaction; a secret-bearing
    // output passes through unchanged.
    CHECK(bash->output.find(secret) != std::string::npos);
    CHECK(bash->output.size() <= 50 * 1024);
    CHECK(bash->truncated);

    // #99: the rendered block header shows the raw command exactly as it
    // arrives from the runtime — no render-time redaction. (The raw output
    // display is pinned at end-to-end by the streaming block test below; this
    // output's tail sits beyond the block's presentation bound.)
    const auto rendered_screen = visible_screen(terminal);
    CHECK(rendered_screen.find("$ " + raw_command) != std::string::npos);
    CHECK(rendered_screen.find(util::kRedactionMarker) == std::string::npos);

    // The spill artifact holds the complete sanitized stream and is never
    // substituted for the bounded model-context value.
    REQUIRE(bash->full_output_path.has_value());
    const std::filesystem::path spill_path{*bash->full_output_path};
    CHECK(spill_path.parent_path() == spill_dir);
    std::ifstream spill_input{spill_path, std::ios::binary};
    const std::string spilled{
        std::istreambuf_iterator<char>(spill_input),
        std::istreambuf_iterator<char>()};
    CHECK(spilled.size() > 60 * 1024);
    CHECK(spilled.find(secret) != std::string::npos);
    CHECK(spilled.find("live api_key=" + secret + "update\n") != std::string::npos);
    CHECK(spilled.ends_with("\napi_key=" + secret));
    const auto permissions = std::filesystem::status(spill_path).permissions();
    CHECK((permissions & std::filesystem::perms::group_all) == std::filesystem::perms::none);
    CHECK((permissions & std::filesystem::perms::others_all) == std::filesystem::perms::none);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    REQUIRE(*run_result);

    std::ifstream persisted{session_path, std::ios::binary};
    const std::string persisted_text{
        std::istreambuf_iterator<char>(persisted),
        std::istreambuf_iterator<char>()};
    CHECK(persisted_text.find(secret) != std::string::npos);
    auto resumed = harness::session::resume_session(session_path);
    REQUIRE(resumed);
    REQUIRE(resumed->history.size() == 1);
    const auto& resumed_bash = std::get<ai::BashExecutionMessage>(resumed->history[0]);
    CHECK(resumed_bash.command == bash->command);
    CHECK(resumed_bash.full_output_path == bash->full_output_path);

    // The recorded path remains valid after Session Close.
    created->session->close();
    CHECK(std::filesystem::exists(spill_path));
    std::filesystem::remove(spill_path);
}

TEST_CASE(
    "User Shell infrastructure failure creates no Bash message and leaves the Session usable",
    "[coding_agent][tui][issue85][issue98]") {
    tests::TempWorkspace workspace;
    auto client = std::make_shared<RecordingChatProvider>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    const std::string secret = "sk-abcdefghijklmnopqrstuvwxyz123456";
    shell->enqueue({
        .updates = {},
        .result = {.exit_code = 0},
        .infrastructure_failure = util::make_error(
            util::ErrorCode::Process,
            "spawn failed",
            "api_key=" + secret),
        .gated = false,
    });

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
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

    REQUIRE(terminal.inject_input("!  fail\r"));
    drain_ready(io);
    CHECK(created->session->snapshot().agent_state.messages.empty());
    auto screen = visible_screen(terminal);
    CHECK(screen.find("spawn failed") != std::string::npos);
    // ADR 0028: shell-error diagnostics pass through without redaction, and
    // the rejected submission returns to the editor verbatim — the doubled
    // space after the prefix survives (pi setText(text)).
    CHECK(screen.find(secret) != std::string::npos);
    CHECK(screen.find("!  fail") != std::string::npos);

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
    "[coding_agent][runtime][issue85][issue96]") {
    tests::TempWorkspace workspace;
    auto client = std::make_shared<RecordingChatProvider>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    const std::string secret = "sk-abcdefghijklmnopqrstuvwxyz123456";
    shell->enqueue({
        .updates = {"callback update api_key=" + secret},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
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
    CHECK(completion->error().detail ==
        "api_key=" + secret + " " +
            std::string(coding_agent::kMaxPresentationPayloadBytes * 2, 'x'));

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
    auto client = std::make_shared<RecordingChatProvider>();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"partial output"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
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

TEST_CASE(
    "User Bash output spill failure preserves the bounded truncated result and a safe diagnostic",
    "[coding_agent][runtime][issue86][issue97]") {
    tests::TempWorkspace workspace;
    tests::EnvVarGuard tmpdir{
        "TMPDIR",
        (workspace.path() / "missing" / "deeper").string()};
    auto client = std::make_shared<RecordingChatProvider>();
    auto shell = std::make_unique<tests::FakeUserShell>();
    const std::string secret = "sk-abcdefghijklmnopqrstuvwxyz123456";
    const std::string bulk(60 * 1024, 'y');
    shell->enqueue({
        .updates = {bulk + "\napi_key=" + secret},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
    auto created = coding_agent::create_agent_session(
        std::move(options), std::move(shell));
    REQUIRE(created);

    boost::asio::io_context io;
    std::optional<util::Expected<coding_agent::runtime::UserBashCompletion>> completion;
    std::string streamed_tail;
    boost::asio::co_spawn(
        io,
        coding_agent::detail::AgentSessionInteractiveAccess::run_user_bash(
            *created->session,
            "huge output",
            false,
            [&streamed_tail](const coding_agent::runtime::UserBashProgress& progress)
                -> util::ExpectedVoid {
                if (!progress.output.empty()) streamed_tail = progress.output;
                return {};
            }),
        [&](std::exception_ptr exception,
            util::Expected<coding_agent::runtime::UserBashCompletion> result) {
            REQUIRE(exception == nullptr);
            completion.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(completion);
    REQUIRE(*completion);
    const auto& bash = (*completion)->message;
    CHECK(bash.truncated);
    CHECK(bash.output.size() <= 50 * 1024);
    // ADR 0028: no redaction — the streaming tail and the committed output
    // carry the secret-bearing output unchanged.
    CHECK(streamed_tail.find(secret) != std::string::npos);
    CHECK(bash.output.find(secret) != std::string::npos);
    CHECK_FALSE(bash.full_output_path.has_value());
    REQUIRE((*completion)->diagnostic.has_value());
    CHECK((*completion)->diagnostic->message.size() <=
        coding_agent::kMaxPresentationPayloadBytes);
    CHECK((*completion)->diagnostic->detail.size() <=
        coding_agent::kMaxPresentationPayloadBytes);
    CHECK((*completion)->diagnostic->detail.find(secret) == std::string::npos);
}

TEST_CASE(
    "User Bash overlaps an active Agent run through the Native TUI",
    "[coding_agent][tui][issue87]") {
    tests::TempWorkspace workspace;
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
    shell_pointer->enqueue({
        .updates = {"later output"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
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

    REQUIRE(terminal.inject_input("prompt one\r"));
    drain_ready(io);
    REQUIRE(client_pointer->requests.size() == 1);

    // User Bash is admitted while the Agent run is active.
    REQUIRE(terminal.inject_input("! during run\r"));
    drain_ready(io);
    REQUIRE(shell_pointer->commands.size() == 1);
    CHECK(shell_pointer->commands[0] == "during run");

    // A second User Bash is rejected, restored to the editor, and diagnosed.
    REQUIRE(terminal.inject_input("! second\r"));
    drain_ready(io);
    CHECK(shell_pointer->commands.size() == 1);
    auto screen = visible_screen(terminal);
    CHECK(screen.find("already in flight") != std::string::npos);
    CHECK(screen.find("! second") != std::string::npos);
    REQUIRE(terminal.inject_input("\x03"));
    drain_ready(io);

    shell_pointer->release();
    drain_ready(io);
    // Completed during the run: the pending block shows the completed form
    // (no loader) and is not yet committed.
    CHECK_FALSE(has_bash_command(
        created->session->snapshot().agent_state.messages, "during run"));
    screen = visible_screen(terminal);
    CHECK(screen.find("$ during run") != std::string::npos);
    CHECK(screen.find("overlap output") != std::string::npos);
    CHECK(screen.find("Running...") == std::string::npos);

    client_pointer->release();
    drain_ready(io);
    // The run settled: committed exactly once after the run's messages.
    auto snapshot = created->session->snapshot();
    REQUIRE(snapshot.agent_state.messages.size() == 3);
    CHECK(std::holds_alternative<ai::UserMessage>(snapshot.agent_state.messages[0]));
    CHECK(std::holds_alternative<ai::AssistantMessage>(snapshot.agent_state.messages[1]));
    CHECK(has_bash_command(snapshot.agent_state.messages, "during run"));
    CHECK(visible_screen(terminal).find("overlap output") != std::string::npos);

    // Reverse direction: an ordinary Prompt is admitted during active Bash.
    REQUIRE(terminal.inject_input("! later bash\r"));
    drain_ready(io);
    REQUIRE(shell_pointer->commands.size() == 2);
    REQUIRE(terminal.inject_input("prompt during bash\r"));
    drain_ready(io);
    CHECK(client_pointer->requests.size() == 2);

    shell_pointer->release();
    drain_ready(io);
    CHECK_FALSE(has_bash_command(
        created->session->snapshot().agent_state.messages, "later bash"));
    client_pointer->release();
    drain_ready(io);
    snapshot = created->session->snapshot();
    REQUIRE(snapshot.agent_state.messages.size() == 6);
    CHECK(has_bash_command(snapshot.agent_state.messages, "later bash"));
    CHECK(visible_screen(terminal).find("later output") != std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI interrupt cancels an active Agent run before an overlapping User Bash",
    "[coding_agent][tui][issue88]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto client = std::make_shared<AbortAwareGatedChatProvider>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"partial bash output"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });
    shell_pointer->enqueue({
        .updates = {"second command output"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
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
            {.agent_config_directory = config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            REQUIRE(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(terminal.inject_input("stream\r"));
    drain_ready(io);
    REQUIRE(client_pointer->started);

    // User Bash overlaps the active Agent run.
    REQUIRE(terminal.inject_input("!echo overlap\r"));
    drain_ready(io);
    REQUIRE(shell_pointer->commands.size() == 1);
    auto running_screen = visible_screen(terminal);
    CHECK(running_screen.find("$ echo overlap") != std::string::npos);
    CHECK(running_screen.find("Running...") != std::string::npos);

    // Autocomplete cancellation precedes both active operations.
    REQUIRE(terminal.inject_input("/"));
    drain_ready(io);
    CHECK(visible_screen(terminal).find("/clear") != std::string::npos);
    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    drain_ready(io);
    CHECK(visible_screen(terminal).find("/clear") == std::string::npos);
    CHECK(client_pointer->stop_callback_count == 0);
    CHECK(shell_pointer->cancellation_request_count == 0);
    REQUIRE(terminal.inject_input("\x03"));
    drain_ready(io);

    // The first applicable interrupt cancels the Agent run, not the Bash.
    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    drain_ready(io);
    CHECK(client_pointer->stop_callback_count == 1);
    CHECK(shell_pointer->cancellation_request_count == 0);
    CHECK(visible_screen(terminal).find("prompt aborted") !=
        std::string::npos);
    CHECK(created->session->is_busy());

    // A later interrupt reaches the User Bash once the Agent is idle.
    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    drain_ready(io);
    CHECK(shell_pointer->cancellation_request_count == 1);
    auto snapshot = created->session->snapshot();
    REQUIRE(snapshot.agent_state.messages.size() == 3);
    const auto* bash = std::get_if<ai::BashExecutionMessage>(
        &snapshot.agent_state.messages[2]);
    REQUIRE(bash != nullptr);
    CHECK(bash->cancelled);
    CHECK_FALSE(bash->exit_code.has_value());
    CHECK(bash->output == "partial bash output");
    const auto cancelled_screen = visible_screen(terminal);
    CHECK(count_occurrences(cancelled_screen, "$ echo overlap") == 1);
    CHECK(cancelled_screen.find("(cancelled)") != std::string::npos);
    CHECK_FALSE(created->session->is_busy());

    // Recovery: a later command and an ordinary Prompt still work.
    REQUIRE(terminal.inject_input("!echo two\r"));
    drain_ready(io);
    snapshot = created->session->snapshot();
    REQUIRE(snapshot.agent_state.messages.size() == 4);
    const auto* second = std::get_if<ai::BashExecutionMessage>(
        &snapshot.agent_state.messages[3]);
    REQUIRE(second != nullptr);
    CHECK(second->exit_code == 0);

    REQUIRE(terminal.inject_input("later\r"));
    drain_ready(io);
    CHECK(client_pointer->request_count == 2);
    CHECK(visible_screen(terminal).find("recovery reply") != std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
    CHECK_FALSE(terminal.modes().started);
}

TEST_CASE(
    "idle Bash-mode interrupt clears the editor without creating a command or message",
    "[coding_agent][tui][issue88][issue93][issue94]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto client = std::make_shared<RecordingChatProvider>();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"done output"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
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
            {.agent_config_directory = config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            REQUIRE(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    // Unsubmitted Bash-mode input: interrupt clears it without side effects.
    REQUIRE(terminal.inject_input("!draft"));
    drain_ready(io);
    CHECK(visible_screen(terminal).find("!draft") != std::string::npos);
    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    drain_ready(io);
    CHECK(visible_screen(terminal).find("draft") == std::string::npos);
    CHECK(shell_pointer->commands.empty());
    CHECK(created->session->message_count() == 0);

    // Text entered after the key-time decision survives while the sampled
    // excluded Bash input is cleared.
    REQUIRE(terminal.inject_input("!!local only"));
    drain_ready(io);
    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    REQUIRE(terminal.inject_input("\x01x"));
    drain_ready(io);
    auto screen = visible_screen(terminal);
    CHECK(screen.find("local only") == std::string::npos);
    CHECK(screen.find("x") != std::string::npos);
    CHECK(shell_pointer->commands.empty());
    CHECK(created->session->message_count() == 0);
    REQUIRE(terminal.inject_input("\x03"));
    drain_ready(io);

    // Submission decoded immediately after the interrupt cannot race the
    // key-time pending-Bash decision and launch the cleared command.
    REQUIRE(terminal.inject_input("!must not run"));
    drain_ready(io);
    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    REQUIRE(terminal.inject_input("\r"));
    REQUIRE(terminal.flush_input());
    drain_ready(io);
    CHECK(shell_pointer->commands.empty());
    CHECK(created->session->message_count() == 0);

    // Ordinary unsubmitted text keeps the existing idle behavior: untouched.
    REQUIRE(terminal.inject_input("plain draft"));
    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    drain_ready(io);
    CHECK(visible_screen(terminal).find("plain draft") != std::string::npos);
    REQUIRE(terminal.inject_input("\x03"));
    drain_ready(io);

    // Bash mode remains fully functional afterwards.
    REQUIRE(terminal.inject_input("!echo works\r"));
    drain_ready(io);
    REQUIRE(shell_pointer->commands.size() == 1);
    CHECK(shell_pointer->commands[0] == "echo works");
    CHECK(created->session->message_count() == 1);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "repeated User Bash interrupts coalesce and recovery still works",
    "[coding_agent][tui][issue88]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto client = std::make_shared<RecordingChatProvider>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"held output"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });
    shell_pointer->enqueue({
        .updates = {"next output"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
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
            {.agent_config_directory = config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            REQUIRE(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(terminal.inject_input("!echo hold\r"));
    drain_ready(io);
    REQUIRE(shell_pointer->commands.size() == 1);

    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    drain_ready(io);
    // Both interrupts coalesce into one Shell cancellation and one message.
    CHECK(shell_pointer->cancellation_request_count == 1);
    auto snapshot = created->session->snapshot();
    REQUIRE(snapshot.agent_state.messages.size() == 1);
    const auto* bash = std::get_if<ai::BashExecutionMessage>(
        &snapshot.agent_state.messages[0]);
    REQUIRE(bash != nullptr);
    CHECK(bash->cancelled);
    CHECK(bash->output == "held output");
    const auto close_screen = visible_screen(terminal);
    CHECK(count_occurrences(close_screen, "$ echo hold") == 1);
    CHECK(close_screen.find("(cancelled)") != std::string::npos);

    // A later command and an ordinary Prompt still work after cancellation.
    REQUIRE(terminal.inject_input("!echo next\r"));
    drain_ready(io);
    CHECK(shell_pointer->commands.size() == 2);
    CHECK(created->session->message_count() == 2);

    REQUIRE(terminal.inject_input("ordinary\r"));
    drain_ready(io);
    CHECK(client_pointer->requests.size() == 1);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Session Close through the exit command cancels gated provider, tool, and User Shell work",
    "[coding_agent][tui][issue88]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto client = std::make_shared<ToolCallThenAbortChatProvider>();
    auto tool = std::make_unique<GatedCloseTool>();
    auto* tool_pointer = tool.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"close bash partial"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
    options.custom_tools.push_back(std::move(tool));
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
            {.agent_config_directory = config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            REQUIRE(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(terminal.inject_input("use the tool\r"));
    drain_ready(io);
    REQUIRE(tool_pointer->started);
    REQUIRE(terminal.inject_input("!echo close\r"));
    drain_ready(io);
    REQUIRE(shell_pointer->commands.size() == 1);

    // Close requests both cancellations and waits for quiescence: the run
    // cannot finish while the tool callback is still gated.
    REQUIRE(terminal.inject_input("/exit\r"));
    drain_ready(io);
    CHECK(tool_pointer->stop_callback_count == 1);
    CHECK(shell_pointer->cancellation_request_count == 1);
    CHECK(created->session->is_busy());
    REQUIRE_FALSE(run_result.has_value());

    tool_pointer->release();
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
    CHECK_FALSE(created->session->is_open());
    CHECK_FALSE(created->session->is_busy());

    // The cancelled Bash committed exactly once; cleanup and terminal
    // restoration happened exactly once with no late presentation.
    const auto screen = visible_screen(terminal);
    CHECK(count_occurrences(screen, "$ echo close") == 1);
    CHECK(screen.find("(cancelled)") != std::string::npos);
    CHECK(screen.find("close bash partial") != std::string::npos);
    CHECK_FALSE(terminal.modes().started);
    CHECK(terminal.modes().cursor_visible);
}

TEST_CASE(
    "Session Close through the effective exit keybinding cancels gated provider, tool, and User Shell work",
    "[coding_agent][tui][issue88]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    config.write("keybindings.json", R"({"app.exit":"f6"})");
    auto client = std::make_shared<ToolCallThenAbortChatProvider>();
    auto tool = std::make_unique<GatedCloseTool>();
    auto* tool_pointer = tool.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"keybinding close partial"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
    options.custom_tools.push_back(std::move(tool));
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
            {.agent_config_directory = config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            REQUIRE(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(terminal.inject_input("use the tool\r"));
    drain_ready(io);
    REQUIRE(tool_pointer->started);
    REQUIRE(terminal.inject_input("!echo f6 close\r"));
    drain_ready(io);
    REQUIRE(shell_pointer->commands.size() == 1);

    REQUIRE(terminal.inject_input("\x1b[17~"));
    drain_ready(io);
    CHECK(tool_pointer->stop_callback_count == 1);
    CHECK(shell_pointer->cancellation_request_count == 1);
    CHECK(created->session->is_busy());
    REQUIRE_FALSE(run_result.has_value());

    tool_pointer->release();
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
    CHECK_FALSE(created->session->is_open());
    CHECK_FALSE(created->session->is_busy());

    const auto screen = visible_screen(terminal);
    CHECK(count_occurrences(screen, "$ echo f6 close") == 1);
    CHECK(screen.find("(cancelled)") != std::string::npos);
    CHECK(screen.find("keybinding close partial") != std::string::npos);
    CHECK_FALSE(terminal.modes().started);
    CHECK(terminal.modes().cursor_visible);
}

TEST_CASE(
    "committed User Bash blocks preview the output tail and expand through the effective action",
    "[coding_agent][tui][issue89]") {
    tests::TempWorkspace workspace;
    auto client = std::make_shared<RecordingChatProvider>();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    std::string many_lines;
    for (int index = 1; index <= 25; ++index) {
        if (index > 1) many_lines.push_back('\n');
        many_lines += std::format("line-{:02}", index);
    }
    shell_pointer->enqueue({
        .updates = {many_lines},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });
    shell_pointer->enqueue({
        .updates = {"failing output"},
        .result = {.exit_code = 3},
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
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

    REQUIRE(terminal.inject_input("!make lines\r"));
    drain_ready(io);

    // The collapsed block derives from the last 20 logical lines.
    auto screen = visible_screen(terminal);
    CHECK(screen.find("$ make lines") != std::string::npos);
    CHECK(screen.find("line-01") == std::string::npos);
    CHECK(screen.find("line-05") == std::string::npos);
    CHECK(screen.find("line-06") != std::string::npos);
    CHECK(screen.find("line-25") != std::string::npos);
    CHECK(screen.find("... 5 more lines (ctrl+o to expand)") != std::string::npos);
    CHECK(screen.find("(exit") == std::string::npos);
    CHECK(screen.find("Bash success") == std::string::npos);
    CHECK(count_occurrences(screen, "$ make lines") == 1);

    // The existing effective tool-expansion action expands and collapses.
    REQUIRE(terminal.inject_input("\x0f"));
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(screen.find("line-01") != std::string::npos);
    CHECK(screen.find("line-25") != std::string::npos);
    CHECK(screen.find("(ctrl+o to collapse)") != std::string::npos);

    REQUIRE(terminal.inject_input("\x0f"));
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(screen.find("line-01") == std::string::npos);

    // A non-zero exit stays one concise status without losing the output.
    REQUIRE(terminal.inject_input("!fails\r"));
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(screen.find("$ fails") != std::string::npos);
    CHECK(screen.find("failing output") != std::string::npos);
    CHECK(screen.find("(exit 3)") != std::string::npos);
    CHECK(screen.find("Bash failed") == std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "resumed User Bash messages render in original order with their recorded meanings",
    "[coding_agent][tui][issue89]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    const auto session_file = workspace.path() / "bash-resume.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(
        session_file,
        {
            .session_id = "bash-resume",
            .created_at = "2026-07-28T00:00:00Z",
            .workspace = workspace.path(),
            .provider = "fake",
            .model = "fake-model",
        });
    REQUIRE(store);

    ai::UserMessage before;
    before.timestamp = 1'700'000'000'000;
    before.content = std::vector<ai::Content>{ai::text_content("before bash")};
    REQUIRE(store->append(ai::MessageVariant{before}));

    ai::BashExecutionMessage included;
    included.command = "echo resumed";
    included.output = "resumed output";
    included.exit_code = 0;
    included.timestamp = 1'700'000'000'001;
    REQUIRE(store->append(ai::MessageVariant{included}));

    ai::BashExecutionMessage excluded;
    excluded.command = "secret local";
    excluded.output = "local output";
    excluded.exit_code = 9;
    excluded.exclude_from_context = true;
    excluded.timestamp = 1'700'000'000'002;
    REQUIRE(store->append(ai::MessageVariant{excluded}));

    ai::BashExecutionMessage cancelled;
    cancelled.command = "sleep forever";
    cancelled.output = "partial output";
    cancelled.cancelled = true;
    cancelled.timestamp = 1'700'000'000'003;
    REQUIRE(store->append(ai::MessageVariant{cancelled}));

    ai::UserMessage after;
    after.timestamp = 1'700'000'000'004;
    after.content = std::vector<ai::Content>{ai::text_content("after bash")};
    REQUIRE(store->append(ai::MessageVariant{after}));

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::ExplicitResumeSessionTarget{session_file};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::make_shared<RecordingChatProvider>());
    auto resumed = coding_agent::create_agent_session(std::move(options));
    REQUIRE(resumed);

    tui::VirtualTerminal terminal({.columns = 72, .rows = 40});
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *resumed->session,
            terminal,
            {.agent_config_directory = config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            REQUIRE(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    const auto screen = visible_screen(terminal);
    const auto before_position = screen.find("before bash");
    const auto included_position = screen.find("$ echo resumed");
    const auto excluded_position = screen.find("$ secret local");
    const auto cancelled_position = screen.find("$ sleep forever");
    const auto after_position = screen.find("after bash");
    REQUIRE(before_position != std::string::npos);
    REQUIRE(included_position != std::string::npos);
    REQUIRE(excluded_position != std::string::npos);
    REQUIRE(cancelled_position != std::string::npos);
    REQUIRE(after_position != std::string::npos);
    CHECK(before_position < included_position);
    CHECK(included_position < excluded_position);
    CHECK(excluded_position < cancelled_position);
    CHECK(cancelled_position < after_position);
    CHECK(screen.find("resumed output") != std::string::npos);
    CHECK(screen.find("local output") != std::string::npos);
    CHECK(screen.find("(exit 9)") != std::string::npos);
    CHECK(screen.find("(cancelled)") != std::string::npos);
    CHECK(screen.find("partial output") != std::string::npos);
    CHECK(screen.find("Bash success") == std::string::npos);
    CHECK(screen.find("Bash failed") == std::string::npos);
    CHECK(count_occurrences(screen, "$ echo resumed") == 1);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "live User Bash blocks stream through one status block with a loader and effective hints",
    "[coding_agent][tui][issue89][issue99]") {
    tests::TempWorkspace workspace;
    auto client = std::make_shared<RecordingChatProvider>();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    const std::string secret = "sk-abcdefghijklmnopqrstuvwxyz123456";
    shell_pointer->enqueue({
        .updates = {"streamed partial api_key=" + secret + "\n"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });
    shell_pointer->enqueue({
        .updates = {},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
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

    REQUIRE(terminal.inject_input("!watch out\r"));
    drain_ready(io);
    REQUIRE(shell_pointer->commands.size() == 1);

    // One streaming block: a $ command header, streamed output, and a running
    // loader with the effective interruption hint; nothing committed yet.
    // #99: the streamed output renders raw — no render-time redaction.
    auto screen = visible_screen(terminal);
    CHECK(screen.find("$ watch out") != std::string::npos);
    CHECK(screen.find("streamed partial api_key=" + secret) != std::string::npos);
    CHECK(screen.find(util::kRedactionMarker) == std::string::npos);
    CHECK(screen.find("Running...") != std::string::npos);
    CHECK(screen.find("(escape to cancel)") != std::string::npos);
    CHECK(created->session->snapshot().agent_state.messages.empty());

    // Commitment reconciles the pending block into one transcript entry
    // without duplicating history; the committed block keeps the raw output.
    shell_pointer->release();
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(count_occurrences(screen, "$ watch out") == 1);
    CHECK(screen.find("streamed partial api_key=" + secret) != std::string::npos);
    CHECK(screen.find(util::kRedactionMarker) == std::string::npos);
    CHECK(screen.find("Running...") == std::string::npos);
    CHECK(screen.find("(exit") == std::string::npos);
    CHECK(created->session->snapshot().agent_state.messages.size() == 1);

    // Excluded execution uses the same block form.
    REQUIRE(terminal.inject_input("!!quiet\r"));
    drain_ready(io);
    REQUIRE(shell_pointer->commands.size() == 2);
    screen = visible_screen(terminal);
    CHECK(screen.find("$ quiet") != std::string::npos);
    CHECK(screen.find("Running...") != std::string::npos);
    shell_pointer->release();
    drain_ready(io);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "User Bash blocks style model-context inclusion through theme tokens",
    "[coding_agent][tui][issue89]") {
    tests::TempWorkspace workspace;
    auto client = std::make_shared<RecordingChatProvider>();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {"styled output"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });
    shell_pointer->enqueue({
        .updates = {"hidden output"},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
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

    REQUIRE(terminal.inject_input("!echo styled\r"));
    drain_ready(io);
    REQUIRE(terminal.inject_input("!!echo hidden\r"));
    drain_ready(io);
    REQUIRE(shell_pointer->commands.size() == 2);

    // The included block uses the bashMode token; the excluded block is dimmed.
    coding_agent::tui::LiveTheme probe_theme(
        coding_agent::tui::select_builtin_theme(terminal.capabilities()),
        terminal.capabilities().color);
    const auto styled_params = sgr_params(
        probe_theme.foreground(coding_agent::tui::ThemeToken::BashMode, "x"));
    const auto dimmed_params = sgr_params(
        probe_theme.foreground(coding_agent::tui::ThemeToken::Dim, "x"));
    const auto muted_params = sgr_params(
        probe_theme.foreground(coding_agent::tui::ThemeToken::Muted, "x"));
    CHECK(fg_at_text(terminal, "$ echo styled") == styled_params);
    CHECK(fg_at_text(terminal, "$ echo hidden") == dimmed_params);
    CHECK(fg_at_text(terminal, "styled output") == muted_params);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "the editor enters Bash mode on trimmed ! input and recalls the original submission verbatim",
    "[coding_agent][tui][issue89][issue94][issue98]") {
    tests::TempWorkspace workspace;
    const std::string secret = "sk-abcdefghijklmnopqrstuvwxyz123456";
    auto client = std::make_shared<RecordingChatProvider>();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
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

    coding_agent::tui::LiveTheme probe_theme(
        coding_agent::tui::select_builtin_theme(terminal.capabilities()),
        terminal.capabilities().color);
    const auto bash_params = sgr_params(
        probe_theme.foreground(coding_agent::tui::ThemeToken::BashMode, "x"));
    const auto text_params = sgr_params(
        probe_theme.foreground(coding_agent::tui::ThemeToken::Text, "x"));

    // Bash mode starts as soon as the trimmed input begins with !.
    REQUIRE(terminal.inject_input("  !echo marker"));
    drain_ready(io);
    CHECK(fg_at_text(terminal, "!echo marker") == bash_params);

    // Ordinary text keeps the ordinary editor styling.
    REQUIRE(terminal.inject_input("\x03"));
    drain_ready(io);
    REQUIRE(terminal.inject_input("plain text"));
    drain_ready(io);
    CHECK(fg_at_text(terminal, "plain text") == text_params);

    // A rejected second command is recalled to the editor verbatim (pi
    // setText(text), ADR 0028): no redaction, no re-serialized prefix.
    REQUIRE(terminal.inject_input("\x03"));
    drain_ready(io);
    REQUIRE(terminal.inject_input("!first wait\r"));
    drain_ready(io);
    REQUIRE(shell_pointer->commands.size() == 1);
    REQUIRE(terminal.inject_input("!api_key=" + secret + " recall\r"));
    drain_ready(io);
    CHECK(shell_pointer->commands.size() == 1);
    const auto screen = visible_screen(terminal);
    CHECK(screen.find("already in flight") != std::string::npos);
    CHECK(screen.find("!api_key=" + secret + " recall") != std::string::npos);

    REQUIRE(terminal.inject_input("\x03"));
    drain_ready(io);
    shell_pointer->release();
    drain_ready(io);
    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "User Bash hints follow effective remapped interruption and expansion bindings",
    "[coding_agent][tui][issue89]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    config.write("keybindings.json", R"({"app.interrupt":"f5","app.tools.expand":"f7"})");
    auto client = std::make_shared<RecordingChatProvider>();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    std::string many_lines;
    for (int index = 1; index <= 25; ++index) {
        if (index > 1) many_lines.push_back('\n');
        many_lines += std::format("remap-{:02}", index);
    }
    shell_pointer->enqueue({
        .updates = {many_lines},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });
    shell_pointer->enqueue({
        .updates = {},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = true,
    });

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
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
            {.agent_config_directory = config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            REQUIRE(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(terminal.inject_input("!remap lines\r"));
    drain_ready(io);
    auto screen = visible_screen(terminal);
    CHECK(screen.find("... 5 more lines (f7 to expand)") != std::string::npos);

    REQUIRE(terminal.inject_input("!remap loader\r"));
    drain_ready(io);
    REQUIRE(shell_pointer->commands.size() == 2);
    screen = visible_screen(terminal);
    CHECK(screen.find("(f5 to cancel)") != std::string::npos);

    shell_pointer->release();
    drain_ready(io);
    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "the collapsed User Bash block renders at most 20 visual lines on a narrow terminal",
    "[coding_agent][tui][issue89]") {
    tests::TempWorkspace workspace;
    auto client = std::make_shared<RecordingChatProvider>();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    std::string wide_output;
    for (int index = 1; index <= 5; ++index) {
        if (index > 1) wide_output.push_back('\n');
        wide_output += std::format("L{}-", index);
        wide_output += std::string(190, static_cast<char>('a' + index - 1));
    }
    shell_pointer->enqueue({
        .updates = {wide_output},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
    auto created = coding_agent::create_agent_session(
        std::move(options), std::move(shell));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 40, .rows = 60});
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

    REQUIRE(terminal.inject_input("!narrow\r"));
    drain_ready(io);

    // Five logical lines fit the 20-line logical preview, but each wraps to
    // five visual lines at this width: the visual tail keeps the last 20.
    const auto screen = visible_screen(terminal);
    CHECK(screen.find("$ narrow") != std::string::npos);
    // pi bash-execution.ts renders the preview with the content pad (1), so
    // at this width the 20-line visual tail starts inside the second logical
    // line: the first two prefixes are cut, the last line stays.
    CHECK(screen.find("L1-") == std::string::npos);
    CHECK(screen.find("L2-") == std::string::npos);
    CHECK(screen.find("L5-") != std::string::npos);
    CHECK(screen.find("more lines") == std::string::npos);

    // The effective tool-expansion action still reveals the full output.
    REQUIRE(terminal.inject_input("\x0f"));
    drain_ready(io);
    CHECK(visible_screen(terminal).find("L1-") != std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "focused User Bash dispatch trims, parses prefixes, and falls through like the pi baseline",
    "[coding_agent][tui][issue89]") {
    tests::TempWorkspace workspace;
    auto client = std::make_shared<RecordingChatProvider>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();
    shell_pointer->enqueue({
        .updates = {},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });
    shell_pointer->enqueue({
        .updates = {},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });
    shell_pointer->enqueue({
        .updates = {},
        .result = {.exit_code = 0},
        .infrastructure_failure = std::nullopt,
        .gated = false,
    });

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
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

    // Surrounding whitespace does not alter dispatch; the remainder is trimmed.
    REQUIRE(terminal.inject_input("   !   echo spaced   \r"));
    drain_ready(io);
    REQUIRE(shell_pointer->commands.size() == 1);
    CHECK(shell_pointer->commands[0] == "echo spaced");

    // A multiline remainder runs as one trimmed script.
    REQUIRE(terminal.inject_input("!  \x1b[200~printf one\nprintf two  \x1b[201~\r"));
    drain_ready(io);
    REQUIRE(shell_pointer->commands.size() == 2);
    CHECK(shell_pointer->commands[1] == "printf one\nprintf two");

    // More than two leading exclamation marks follow the baseline prefix
    // rule: excluded User Bash running "!foo".
    REQUIRE(terminal.inject_input("!!!foo\r"));
    drain_ready(io);
    REQUIRE(shell_pointer->commands.size() == 3);
    CHECK(shell_pointer->commands[2] == "!foo");
    auto snapshot = created->session->snapshot();
    REQUIRE(snapshot.agent_state.messages.size() == 3);
    const auto* third = std::get_if<ai::BashExecutionMessage>(
        &snapshot.agent_state.messages[2]);
    REQUIRE(third != nullptr);
    CHECK(third->command == "!foo");
    CHECK(third->exclude_from_context);

    // Bare prefixes fall through to ordinary Agent Prompt processing.
    REQUIRE(terminal.inject_input("! \r"));
    drain_ready(io);
    REQUIRE(client_pointer->requests.size() == 1);
    REQUIRE(terminal.inject_input("  !!  \r"));
    drain_ready(io);
    REQUIRE(client_pointer->requests.size() == 2);
    CHECK(shell_pointer->commands.size() == 3);
    for (const auto& request : client_pointer->requests) {
        const auto& user = std::get<ai::UserMessage>(request.context.messages.back());
        CHECK(ai::text_from_user_message(user).starts_with("!"));
    }

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Skill and Prompt Template expansions beginning with ! stay ordinary Agent Prompt text",
    "[coding_agent][tui][issue89]") {
    tests::TempWorkspace workspace;
    workspace.write(
        ".pi/skills/bang-skill/SKILL.md",
        "---\n"
        "name: bang-skill\n"
        "description: Skill whose body begins with a bang.\n"
        "---\n"
        "!echo skill-body\n");
    workspace.write(
        ".pi/prompts/bang-prompt.md",
        "---\n"
        "description: Template whose expansion begins with a bang.\n"
        "---\n"
        "!echo template-body $ARGUMENTS\n");
    auto client = std::make_shared<RecordingChatProvider>();
    auto* client_pointer = client.get();
    auto shell = std::make_unique<tests::FakeUserShell>();
    auto* shell_pointer = shell.get();

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
    options.project_trust_override = true;
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

    REQUIRE(terminal.inject_input("/skill:bang-skill\r"));
    drain_ready(io);
    REQUIRE(client_pointer->requests.size() == 1);
    const auto& skill_user = std::get<ai::UserMessage>(
        client_pointer->requests[0].context.messages.back());
    CHECK(ai::text_from_user_message(skill_user).find("!echo skill-body") !=
        std::string::npos);
    CHECK(shell_pointer->commands.empty());

    REQUIRE(terminal.inject_input("/bang-prompt target\r"));
    drain_ready(io);
    REQUIRE(client_pointer->requests.size() == 2);
    const auto& template_user = std::get<ai::UserMessage>(
        client_pointer->requests[1].context.messages.back());
    CHECK(ai::text_from_user_message(template_user) == "!echo template-body target");
    CHECK(shell_pointer->commands.empty());

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "/help documents User Bash input prefixes without a pseudo-command or hotkey action",
    "[coding_agent][tui][issue89]") {
    tests::TempWorkspace workspace;
    auto client = std::make_shared<RecordingChatProvider>();
    auto shell = std::make_unique<tests::FakeUserShell>();

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
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

    // A separate input-prefix section documents ! and !!.
    REQUIRE(terminal.inject_input("/help\r"));
    drain_ready(io);
    auto screen = visible_screen(terminal);
    CHECK(screen.find("Available commands:") != std::string::npos);
    CHECK(screen.find("Input prefixes:") != std::string::npos);
    CHECK(screen.find("! <command>") != std::string::npos);
    CHECK(screen.find("!! <command>") != std::string::npos);

    // The prefixes are not slash commands.
    REQUIRE(terminal.inject_input("/help !\r"));
    drain_ready(io);
    CHECK(visible_screen(terminal).find("Unknown command: /!") != std::string::npos);

    // Typing a prefix opens no slash autocomplete.
    REQUIRE(terminal.inject_input("!"));
    drain_ready(io);
    CHECK(count_occurrences(visible_screen(terminal), "> /") == 0);
    REQUIRE(terminal.inject_input("\x03"));
    drain_ready(io);

    // Hotkey help registers no User Bash action.
    REQUIRE(terminal.inject_input("/hotkeys\r"));
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(screen.find("Hotkeys") != std::string::npos);
    CHECK(screen.find("bash") == std::string::npos);
    CHECK(screen.find("Bash") == std::string::npos);

    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    drain_ready(io);
    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}
