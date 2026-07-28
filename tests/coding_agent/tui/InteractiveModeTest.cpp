#include "coding_agent/tui/InteractiveMode.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/ChatClient.hpp>
#include <cch/ai/Content.hpp>
#include <cch/coding_agent/Sdk.hpp>
#include <cch/harness/session/JsonlSessionStore.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "ai/providers/FakeChatClient.hpp"
#include "coding_agent/AgentSessionBridge.hpp"
#include "coding_agent/BoundedText.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"

#include "../../../third_party/catch2/catch_test_macros.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

using namespace cch;

namespace {

[[nodiscard]] coding_agent::CreateAgentSessionOptions session_options(
    const tests::TempWorkspace& workspace,
    std::unique_ptr<ai::StreamingChatClient> client) {
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
    return options;
}

void drain_ready(boost::asio::io_context& io) {
    if (io.stopped()) io.restart();
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

[[nodiscard]] std::size_t count_text(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

class FailOnceChatClient final : public ai::StreamingChatClient {
public:
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& request,
        ai::AssistantEventSink) override {
        if (calls++ == 0) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Provider,
                "provider failed",
                std::format(
                    "{} api_key=sk-abcdefghijklmnopqrstuvwxyz123456 {}",
                    std::string(7500, 'x'),
                    std::string(1500, 'y'))));
        }
        auto response = ai::assistant_text_message("recovered");
        response.provider = "fake";
        response.api = "fake";
        response.model = request.model->id;
        co_return response;
    }

    std::size_t calls{0};
};

class FailingStartTerminal final : public tui::Terminal {
public:
    [[nodiscard]] util::ExpectedVoid start(
        tui::TerminalInputSink,
        tui::TerminalResizeSink) override {
        return std::unexpected(util::make_error(
            util::ErrorCode::Process,
            "terminal acquisition failed",
            std::format(
                "api_key=sk-abcdefghijklmnopqrstuvwxyz123456 {}",
                std::string(9000, 'x'))));
    }
    [[nodiscard]] util::ExpectedVoid stop() override { return {}; }
    [[nodiscard]] tui::TerminalDimensions dimensions() const override { return {}; }
    [[nodiscard]] tui::TerminalCapabilities capabilities() const override { return {}; }
    [[nodiscard]] tui::TerminalModeState modes() const override { return {}; }
    [[nodiscard]] util::ExpectedVoid clear_screen() override { return {}; }
    [[nodiscard]] util::ExpectedVoid write(std::string_view) override { return {}; }
    [[nodiscard]] util::ExpectedVoid set_cursor(tui::CursorPosition) override { return {}; }
    [[nodiscard]] util::ExpectedVoid set_cursor_visible(bool) override { return {}; }
    [[nodiscard]] util::Expected<tui::TerminalImageHandle> place_image(
        const tui::TerminalImage&) override {
        return tui::TerminalImageHandle{};
    }
    [[nodiscard]] util::ExpectedVoid remove_image(
        tui::TerminalImageHandle,
        const tui::CellRegion&) override {
        return {};
    }
    [[nodiscard]] util::ExpectedVoid begin_synchronized_update() override { return {}; }
    [[nodiscard]] util::ExpectedVoid end_synchronized_update() override { return {}; }
};

class FailingCleanupTerminal final : public tui::Terminal {
public:
    [[nodiscard]] util::ExpectedVoid start(
        tui::TerminalInputSink,
        tui::TerminalResizeSink) override {
        modes_ = {
            .started = true,
            .raw_input = true,
            .bracketed_paste = true,
            .cursor_visible = true,
        };
        return {};
    }
    [[nodiscard]] util::ExpectedVoid stop() override {
        return std::unexpected(util::make_error(
            util::ErrorCode::Process,
            "terminal restoration failed",
            "api_key=sk-restoration-secret"));
    }
    [[nodiscard]] tui::TerminalDimensions dimensions() const override {
        return {.columns = 20, .rows = 4};
    }
    [[nodiscard]] tui::TerminalCapabilities capabilities() const override { return {}; }
    [[nodiscard]] tui::TerminalModeState modes() const override { return modes_; }
    [[nodiscard]] util::ExpectedVoid clear_screen() override { return {}; }
    [[nodiscard]] util::ExpectedVoid write(std::string_view) override {
        return std::unexpected(util::make_error(
            util::ErrorCode::Process,
            "initial render failed",
            "api_key=sk-render-secret"));
    }
    [[nodiscard]] util::ExpectedVoid set_cursor(tui::CursorPosition) override { return {}; }
    [[nodiscard]] util::ExpectedVoid set_cursor_visible(bool visible) override {
        modes_.cursor_visible = visible;
        return {};
    }
    [[nodiscard]] util::Expected<tui::TerminalImageHandle> place_image(
        const tui::TerminalImage&) override {
        return tui::TerminalImageHandle{};
    }
    [[nodiscard]] util::ExpectedVoid remove_image(
        tui::TerminalImageHandle,
        const tui::CellRegion&) override {
        return {};
    }
    [[nodiscard]] util::ExpectedVoid begin_synchronized_update() override { return {}; }
    [[nodiscard]] util::ExpectedVoid end_synchronized_update() override { return {}; }

private:
    tui::TerminalModeState modes_;
};

class GatedChatClient final : public ai::StreamingChatClient {
public:
    GatedChatClient() = default;
    GatedChatClient(GatedChatClient&&) = delete;
    GatedChatClient& operator=(GatedChatClient&&) = delete;
    ~GatedChatClient() override = default;
    GatedChatClient(const GatedChatClient&) = delete;
    GatedChatClient& operator=(const GatedChatClient&) = delete;

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& request,
        ai::AssistantEventSink sink) override {
        auto partial = ai::assistant_text_message("");
        partial.content.clear();
        partial.provider = "fake";
        partial.api = "fake";
        partial.model = request.model->id;
        if (auto emitted = sink(ai::AssistantStartEvent{partial}); !emitted) {
            co_return std::unexpected(emitted.error());
        }

        const auto executor = co_await boost::asio::this_coro::executor;
        gate_.emplace(executor);
        gate_->expires_at(std::chrono::steady_clock::time_point::max());
        started = true;
        boost::system::error_code error;
        co_await gate_->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));

        auto response = ai::assistant_text_message("released");
        response.provider = "fake";
        response.api = "fake";
        response.model = request.model->id;
        co_return response;
    }

    void release() {
        if (gate_) (void)gate_->cancel();
    }

    bool started{false};

private:
    std::optional<boost::asio::steady_timer> gate_;
};

class RepeatedReadCallChatClient final : public ai::StreamingChatClient {
public:
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& request,
        ai::AssistantEventSink sink) override {
        auto response = ai::assistant_text_message("tool cycle complete");
        response.provider = "tool-fake";
        response.api = "fake";
        response.model = request.model->id;
        if (!request.context.messages.empty() &&
            std::holds_alternative<ai::ToolResultMessage>(request.context.messages.back())) {
            co_return response;
        }

        std::string prompt;
        for (auto message = request.context.messages.rbegin();
             message != request.context.messages.rend();
             ++message) {
            if (const auto* user = std::get_if<ai::UserMessage>(&*message)) {
                prompt = ai::text_from_content(user->content);
                break;
            }
        }
        const auto path = prompt.starts_with("read ") ? prompt.substr(5) : prompt;
        util::JsonValue::object_t arguments;
        arguments.emplace("path", util::JsonValue{path});
        response.content.clear();
        response.content.emplace_back(ai::text_content("reading " + path));
        response.content.emplace_back(ai::tool_call_content(
            "fake-read-1",
            "read",
            std::format(R"({{"path":"{}"}})", path),
            util::JsonValue{std::move(arguments)}));
        response.stop_reason = ai::AssistantStopReason::ToolUse;

        auto partial = response;
        partial.content.clear();
        if (auto emitted = sink(ai::AssistantStartEvent{partial}); !emitted) {
            co_return std::unexpected(emitted.error());
        }
        partial.content.emplace_back(ai::tool_call_content(
            "fake-read-1",
            "read",
            {}));
        if (auto emitted = sink(ai::ToolCallStartEvent{
                .content_index = 0,
                .partial = partial,
            });
            !emitted) {
            co_return std::unexpected(emitted.error());
        }
        partial.content[0] = ai::tool_call_content(
            "fake-read-1",
            "read",
            R"({"path":"STALE ARGUMENT"})");
        if (auto emitted = sink(ai::ToolCallDeltaEvent{
                .content_index = 0,
                .delta = R"({"path":"STALE ARGUMENT"})",
                .partial = partial,
            });
            !emitted) {
            co_return std::unexpected(emitted.error());
        }
        partial.content[0] = response.content[1];
        if (auto emitted = sink(ai::ToolCallDeltaEvent{
                .content_index = 0,
                .delta = std::get<ai::ToolCallContent>(response.content[1]).raw_arguments,
                .partial = partial,
            });
            !emitted) {
            co_return std::unexpected(emitted.error());
        }
        const auto& completed_call = std::get<ai::ToolCallContent>(response.content[1]);
        if (auto emitted = sink(ai::ToolCallEndEvent{
                .content_index = 0,
                .tool_call = completed_call,
                .partial = response,
            });
            !emitted) {
            co_return std::unexpected(emitted.error());
        }
        co_return response;
    }
};

class GatedPartialReadTool final : public agent::AsyncAgentTool {
public:
    GatedPartialReadTool() {
        definition_.name = "read";
        definition_.description = "Stream a deterministic read result";
        definition_.parameters = util::JsonValue::object_t{{"type", "object"}};
    }

    [[nodiscard]] const ai::Tool& definition() const override {
        return definition_;
    }

    [[nodiscard]] boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> execute(
        agent::ToolInvocation invocation,
        std::stop_token) override {
        co_return final_result(invocation);
    }

    [[nodiscard]] boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>>
    execute_with_updates(
        agent::ToolInvocation invocation,
        std::stop_token,
        agent::ToolUpdateSink update_sink) override {
        late_update_sink_.emplace(std::move(update_sink));
        agent::AsyncToolExecutionResult partial;
        partial.content.emplace_back(ai::text_content(
            "partial tool output\nSTALE SNAPSHOT"));
        if (auto updated = (*late_update_sink_)(partial); !updated) {
            co_return std::unexpected(updated.error());
        }
        partial.content.clear();
        partial.content.emplace_back(ai::text_content(
            "partial tool output\nLATEST SNAPSHOT"));
        if (auto updated = (*late_update_sink_)(partial); !updated) {
            co_return std::unexpected(updated.error());
        }

        const auto executor = co_await boost::asio::this_coro::executor;
        gate_.emplace(executor);
        gate_->expires_at(std::chrono::steady_clock::time_point::max());
        started = true;
        boost::system::error_code error;
        co_await gate_->async_wait(
            boost::asio::redirect_error(boost::asio::use_awaitable, error));
        co_return final_result(invocation);
    }

    void release() {
        if (gate_) (void)gate_->cancel();
    }

    [[nodiscard]] util::ExpectedVoid emit_late_update() {
        if (!late_update_sink_) return {};
        agent::AsyncToolExecutionResult late;
        late.content.emplace_back(ai::text_content("LATE TOOL UPDATE"));
        return (*late_update_sink_)(late);
    }

    bool started{false};

private:
    [[nodiscard]] static agent::AsyncToolExecutionResult final_result(
        const agent::ToolInvocation& invocation) {
        agent::AsyncToolExecutionResult result;
        if (invocation.raw_arguments.find("missing.txt") != std::string::npos) {
            result.content.emplace_back(ai::text_content("final tool failure"));
            result.is_error = true;
            return result;
        }
        result.content.emplace_back(ai::text_content(
            "line 1\nline 2\nline 3\nline 4\nline 5\nline 6\nTOOL OUTPUT END\n"));
        return result;
    }

    ai::Tool definition_;
    std::optional<boost::asio::steady_timer> gate_;
    std::optional<agent::ToolUpdateSink> late_update_sink_;
};

class AcceptedOutcomeChatClient final : public ai::StreamingChatClient {
public:
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& request,
        ai::AssistantEventSink) override {
        std::string prompt;
        for (auto message = request.context.messages.rbegin();
             message != request.context.messages.rend();
             ++message) {
            if (const auto* user = std::get_if<ai::UserMessage>(&*message)) {
                prompt = ai::text_from_content(user->content);
                break;
            }
        }

        auto response = ai::assistant_text_message(
            prompt == "recover" ? "recovered after accepted outcomes" : "partial response");
        response.provider = "outcome-fake";
        response.api = "fake";
        response.model = request.model->id;
        if (prompt == "provider error") {
            response.stop_reason = ai::AssistantStopReason::Error;
            response.error_message = "accepted provider failure";
        } else if (prompt == "provider tool error") {
            response.content.clear();
            response.content.emplace_back(ai::tool_call_content(
                "failed-before-execution",
                "read",
                R"({"path":"never.txt"})"));
            response.stop_reason = ai::AssistantStopReason::Error;
            response.error_message = "accepted tool-call provider failure";
        } else if (prompt == "provider abort") {
            response.stop_reason = ai::AssistantStopReason::Aborted;
            response.error_message = "Request was aborted";
        } else if (prompt == "provider tool abort") {
            response.content.clear();
            response.content.emplace_back(ai::tool_call_content(
                "aborted-before-execution",
                "read",
                R"({"path":"never.txt"})"));
            response.stop_reason = ai::AssistantStopReason::Aborted;
            response.error_message = "custom provider abort detail";
        }
        co_return response;
    }
};

class IncrementalGatedChatClient final : public ai::StreamingChatClient {
public:
    IncrementalGatedChatClient() = default;
    IncrementalGatedChatClient(IncrementalGatedChatClient&&) = delete;
    IncrementalGatedChatClient& operator=(IncrementalGatedChatClient&&) = delete;
    ~IncrementalGatedChatClient() override = default;
    IncrementalGatedChatClient(const IncrementalGatedChatClient&) = delete;
    IncrementalGatedChatClient& operator=(const IncrementalGatedChatClient&) = delete;

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& request,
        ai::AssistantEventSink sink) override {
        auto partial = ai::assistant_text_message("");
        partial.content.clear();
        partial.provider = "fake";
        partial.api = "incremental-fake";
        partial.model = request.model->id;
        if (auto emitted = sink(ai::AssistantStartEvent{partial}); !emitted) {
            co_return std::unexpected(emitted.error());
        }

        partial.content.emplace_back(ai::text_content(""));
        if (auto emitted = sink(ai::TextStartEvent{0, partial}); !emitted) {
            co_return std::unexpected(emitted.error());
        }
        std::get<ai::TextContent>(partial.content[0]).text =
            "old line 1\nold line 2\nTAIL PARTIAL";
        if (auto emitted = sink(ai::TextDeltaEvent{
                0,
                "old line 1\nold line 2\nTAIL PARTIAL",
                partial});
            !emitted) {
            co_return std::unexpected(emitted.error());
        }

        const auto executor = co_await boost::asio::this_coro::executor;
        gate_.emplace(executor);
        gate_->expires_at(std::chrono::steady_clock::time_point::max());
        started = true;
        boost::system::error_code error;
        co_await gate_->async_wait(
            boost::asio::redirect_error(boost::asio::use_awaitable, error));

        std::get<ai::TextContent>(partial.content[0]).text =
            "new line 1\nnew line 2\nTAIL COMPLETE";
        if (auto emitted = sink(ai::TextDeltaEvent{
                0,
                "new line 1\nnew line 2\nTAIL COMPLETE",
                partial});
            !emitted) {
            co_return std::unexpected(emitted.error());
        }
        if (auto emitted = sink(ai::TextEndEvent{
                0,
                "new line 1\nnew line 2\nTAIL COMPLETE",
                partial});
            !emitted) {
            co_return std::unexpected(emitted.error());
        }
        if (auto emitted = sink(ai::AssistantDoneEvent{
                ai::AssistantStopReason::Stop,
                partial});
            !emitted) {
            co_return std::unexpected(emitted.error());
        }
        co_return partial;
    }

    void release() {
        if (gate_) (void)gate_->cancel();
    }

    bool started{false};

private:
    std::optional<boost::asio::steady_timer> gate_;
};

} // namespace

TEST_CASE(
    "Native TUI renders a resumed rich transcript before accepting continued input",
    "[coding_agent][tui][resume][issue59]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    config.write(
        "keybindings.json",
        R"({"app.thinking.toggle":"f8","app.tools.expand":[]})");
    const auto session_file = workspace.path() / "rich-session.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(
        session_file,
        {
            .session_id = "rich-session",
            .created_at = "2026-07-28T00:00:00Z",
            .workspace = workspace.path(),
            .provider = "fake",
            .model = "fake-model",
        });
    REQUIRE(store);
    REQUIRE(store->append(ai::MessageVariant{
        ai::user_text_message("before compaction", 1'700'000'000'000)}));
    REQUIRE(store->append(ai::MessageVariant{
        ai::user_text_message("resume request", 1'700'000'000'001)}));
    auto loaded = harness::session::JsonlSessionStore::load(session_file);
    REQUIRE(loaded);
    REQUIRE(loaded->entries.size() >= 3);
    const auto kept_entry_id = loaded->entries[2].entry_id;
    REQUIRE(store->append_compaction(
        std::nullopt,
        "compacted persisted context",
        kept_entry_id,
        1200,
        std::nullopt,
        std::nullopt));

    ai::AssistantMessage assistant;
    assistant.provider = "fake";
    assistant.api = "fake";
    assistant.model = "fake-model";
    assistant.stop_reason = ai::AssistantStopReason::ToolUse;
    assistant.timestamp = 1'700'000'000'002;
    const std::string thinking_text =
        "inspect the saved state\n"
        "compare the active path\n"
        "retain every correlation\n"
        "THINKING END";
    assistant.content.emplace_back(ai::thinking_content(thinking_text));
    assistant.content.emplace_back(ai::text_content("I will read the persisted file."));
    const auto persisted_arguments = std::format(
        R"({{"path":"saved.txt","padding":"{}"}})",
        std::string(700, 'A'));
    assistant.content.emplace_back(ai::tool_call_content(
        "resume-call-7",
        "read",
        persisted_arguments));
    assistant.content.emplace_back(ai::text_content("I will continue after the tool."));
    REQUIRE(store->append(ai::MessageVariant{assistant}));
    REQUIRE(store->append(ai::MessageVariant{ai::tool_result_message(
        "resume-call-7",
        "read",
        "persisted tool output",
        false,
        1'700'000'000'003)}));

    ai::CustomMessage custom;
    custom.custom_type = "notice";
    custom.content.emplace_back(ai::text_content("custom persisted content"));
    custom.timestamp = 1'700'000'000'004;
    REQUIRE(store->append(ai::MessageVariant{custom}));
    REQUIRE(store->append(ai::MessageVariant{ai::BranchSummaryMessage{
        .summary = "abandoned branch context",
        .from_id = "branch-entry",
        .timestamp = 1'700'000'000'006,
    }}));

    coding_agent::runtime::AgentSessionCreationRequest resume;
    resume.session_target = coding_agent::ExplicitResumeSessionTarget{session_file};
    resume.workspace = workspace.path();
    resume.workspace_explicit = true;
    resume.fake = true;
    resume.disable_project_skills = true;
    resume.disable_prompt_templates = true;
    auto resumed = coding_agent::create_agent_session(std::move(resume));
    REQUIRE(resumed);

    const auto authoritative_before = resumed->session->snapshot().agent_state.messages;
    REQUIRE(authoritative_before.size() == 6);
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
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    const auto screen = visible_screen(terminal);
    const auto user_position = screen.find("You: resume request");
    const auto thinking_position = screen.find("Thinking: inspect the saved state");
    const auto assistant_position = screen.find("Assistant: I will read the persisted file.");
    const auto tool_position = screen.find("Tool success: read#resume-call-7");
    const auto result_position = screen.find("Tool result: persisted tool output");
    const auto followup_position = screen.find("Assistant: I will continue after the tool.");
    const auto custom_position = screen.find("Custom notice: custom persisted content");
    const auto compaction_position = screen.find("Compaction summary: 1200 tokens");
    const auto branch_position = screen.find("Branch summary: abandoned branch context");
    REQUIRE(user_position != std::string::npos);
    REQUIRE(thinking_position != std::string::npos);
    REQUIRE(assistant_position != std::string::npos);
    REQUIRE(tool_position != std::string::npos);
    REQUIRE(result_position != std::string::npos);
    REQUIRE(followup_position != std::string::npos);
    REQUIRE(custom_position != std::string::npos);
    REQUIRE(compaction_position != std::string::npos);
    REQUIRE(branch_position != std::string::npos);
    CHECK(screen.find("Unbound to expand") != std::string::npos);
    CHECK(compaction_position < user_position);
    CHECK(user_position < thinking_position);
    CHECK(thinking_position < assistant_position);
    CHECK(assistant_position < tool_position);
    CHECK(tool_position < result_position);
    CHECK(result_position < followup_position);
    CHECK(followup_position < custom_position);
    CHECK(custom_position < branch_position);
    REQUIRE(terminal.inject_input("\x1b[19~"));
    drain_ready(io);
    CHECK(visible_screen(terminal).find("inspect the saved state") == std::string::npos);
    CHECK(visible_screen(terminal).find("THINKING END") == std::string::npos);
    CHECK(visible_screen(terminal).find("Thinking: (collapsed; f8 to expand)") !=
        std::string::npos);
    REQUIRE(terminal.inject_input("\x1b[19~"));
    drain_ready(io);
    CHECK(visible_screen(terminal).find("Thinking: inspect the saved state") !=
        std::string::npos);

    const auto unchanged = resumed->session->snapshot().agent_state.messages;
    REQUIRE(unchanged.size() == authoritative_before.size());
    CHECK(std::get<ai::ThinkingContent>(
        std::get<ai::AssistantMessage>(unchanged[2]).content[0]).thinking ==
        thinking_text);
    CHECK(std::get<ai::ToolResultMessage>(unchanged[3]).tool_call_id == "resume-call-7");
    CHECK(std::get<ai::CompactionSummaryMessage>(unchanged[0]).summary ==
        "compacted persisted context");

    REQUIRE(terminal.inject_input("continue here\r"));
    drain_ready(io);
    CHECK(resumed->session->message_count() == authoritative_before.size() + 2);
    CHECK(visible_screen(terminal).find("Assistant: fake: continue here") != std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI presents fresh and resumed persisted history equivalently",
    "[coding_agent][tui][persistence][issue59]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    const auto session_file = workspace.path() / "equivalent-session.jsonl";

    auto create = session_options(
        workspace,
        ai::providers::make_scripted_fake_chat_client());
    create.session_target = coding_agent::ExplicitNewSessionTarget{session_file};
    auto fresh = coding_agent::create_agent_session(std::move(create));
    REQUIRE(fresh);
    REQUIRE(fresh->session->prompt_blocking("equivalent prompt"));

    tui::VirtualTerminal fresh_terminal({.columns = 64, .rows = 10});
    boost::asio::io_context fresh_io;
    std::optional<util::ExpectedVoid> fresh_result;
    boost::asio::co_spawn(
        fresh_io,
        coding_agent::tui::run_interactive_mode(
            *fresh->session,
            fresh_terminal,
            {.agent_config_directory = config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            CHECK(exception == nullptr);
            fresh_result.emplace(std::move(result));
        });
    drain_ready(fresh_io);
    const auto fresh_screen = visible_screen(fresh_terminal);
    REQUIRE(fresh_terminal.inject_input("\x04"));
    drain_ready(fresh_io);
    REQUIRE(fresh_result);
    CHECK(*fresh_result);

    coding_agent::CreateAgentSessionOptions resume;
    resume.session_target = coding_agent::ExplicitResumeSessionTarget{session_file};
    resume.workspace = workspace.path();
    resume.chat_client = ai::providers::make_scripted_fake_chat_client();
    resume.builtin_tools = {
        .read = false,
        .write = false,
        .edit_file = false,
        .bash = false,
    };
    auto resumed = coding_agent::create_agent_session(std::move(resume));
    REQUIRE(resumed);

    tui::VirtualTerminal resumed_terminal({.columns = 64, .rows = 10});
    boost::asio::io_context resumed_io;
    std::optional<util::ExpectedVoid> resumed_result;
    boost::asio::co_spawn(
        resumed_io,
        coding_agent::tui::run_interactive_mode(
            *resumed->session,
            resumed_terminal,
            {.agent_config_directory = config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            CHECK(exception == nullptr);
            resumed_result.emplace(std::move(result));
        });
    drain_ready(resumed_io);
    CHECK(visible_screen(resumed_terminal) == fresh_screen);

    REQUIRE(resumed_terminal.inject_input("\x04"));
    drain_ready(resumed_io);
    REQUIRE(resumed_result);
    CHECK(*resumed_result);
}

TEST_CASE(
    "Native TUI correlates repeated Tool Call IDs and locally expands long output",
    "[coding_agent][tui][tools][issue59]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto options = session_options(
        workspace,
        std::make_unique<RepeatedReadCallChatClient>());
    auto tool = std::make_unique<GatedPartialReadTool>();
    auto* tool_pointer = tool.get();
    options.custom_tools.push_back(std::move(tool));
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 72, .rows = 32});
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *created->session,
            terminal,
            {.agent_config_directory = config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(terminal.inject_input("read large.txt\r"));
    drain_ready(io);
    REQUIRE(tool_pointer->started);
    auto screen = visible_screen(terminal);
    CHECK(count_text(screen, "Tool pending: read#fake-read-1") == 1);
    CHECK(count_text(screen, "Tool result: partial tool output") == 1);
    CHECK(screen.find("STALE ARGUMENT") == std::string::npos);
    CHECK(screen.find(R"(Tool arguments: {"path":"large.txt"})") != std::string::npos);
    CHECK(screen.find("STALE SNAPSHOT") == std::string::npos);
    CHECK(count_text(screen, "LATEST SNAPSHOT") == 1);

    tool_pointer->release();
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(count_text(screen, "Tool success: read#fake-read-1") == 1);
    CHECK(screen.find("Tool result: line 1") != std::string::npos);
    CHECK(screen.find("TOOL OUTPUT END") == std::string::npos);
    REQUIRE(tool_pointer->emit_late_update());
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(count_text(screen, "Tool success: read#fake-read-1") == 1);
    CHECK(screen.find("LATE TOOL UPDATE") == std::string::npos);

    REQUIRE(terminal.inject_input("\x0f"));
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(screen.find("TOOL OUTPUT END") != std::string::npos);

    tool_pointer->started = false;
    REQUIRE(terminal.inject_input("read missing.txt\r"));
    drain_ready(io);
    REQUIRE(tool_pointer->started);
    screen = visible_screen(terminal);
    CHECK(count_text(screen, "Tool pending: read#fake-read-1") == 1);
    CHECK(count_text(screen, "Tool result: partial tool output") == 1);
    CHECK(screen.find("STALE SNAPSHOT") == std::string::npos);
    CHECK(count_text(screen, "LATEST SNAPSHOT") == 1);

    tool_pointer->release();
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(count_text(screen, "read#fake-read-1") == 2);
    CHECK(count_text(screen, "Tool success: read#fake-read-1") == 1);
    CHECK(count_text(screen, "Tool failed: read#fake-read-1") == 1);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI renders accepted provider outcomes once and remains usable",
    "[coding_agent][tui][provider-outcome][issue59]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto created = coding_agent::create_agent_session(session_options(
        workspace,
        std::make_unique<AcceptedOutcomeChatClient>()));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 72, .rows = 20});
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *created->session,
            terminal,
            {.agent_config_directory = config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(terminal.inject_input("provider error\r"));
    drain_ready(io);
    auto screen = visible_screen(terminal);
    CHECK(count_text(screen, "Provider error: accepted provider failure") == 1);

    REQUIRE(terminal.inject_input("provider abort\r"));
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(count_text(screen, "Provider error: accepted provider failure") == 1);
    CHECK(count_text(screen, "Provider aborted: Operation aborted") == 1);

    REQUIRE(terminal.inject_input("provider tool error\r"));
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(count_text(screen, "accepted tool-call provider failure") == 1);
    CHECK(count_text(screen, "Provider error: accepted tool-call provider failure") == 0);
    CHECK(count_text(screen, "Tool failed: read#failed-before-execution") == 1);

    REQUIRE(terminal.inject_input("provider tool abort\r"));
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(count_text(screen, "custom provider abort detail") == 1);
    CHECK(count_text(screen, "Provider aborted: custom provider abort detail") == 0);
    CHECK(count_text(screen, "Tool failed: read#aborted-before-execution") == 1);

    REQUIRE(terminal.inject_input("recover\r"));
    drain_ready(io);
    CHECK(created->session->message_count() == 10);
    CHECK(visible_screen(terminal).find("Assistant: recovered after accepted outcomes") !=
        std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI redacts startup failures and leaves the Session closed",
    "[coding_agent][tui][failure][issue58]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto created = coding_agent::create_agent_session(session_options(
        workspace,
        ai::providers::make_scripted_fake_chat_client()));
    REQUIRE(created);

    FailingStartTerminal terminal;
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *created->session,
            terminal,
            {.agent_config_directory = config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(run_result);
    REQUIRE_FALSE(*run_result);
    CHECK(run_result->error().message == "Native TUI startup failed");
    CHECK(run_result->error().detail.find("sk-abcdefghijklmnopqrstuvwxyz123456") == std::string::npos);
    CHECK(run_result->error().detail.find("[REDACTED]") != std::string::npos);
    CHECK(run_result->error().detail.size() <= coding_agent::kMaxPresentationPayloadBytes);
    CHECK_FALSE(created->session->is_open());
    CHECK(terminal.modes() == tui::TerminalModeState{});
}

TEST_CASE(
    "Native TUI reports both post-acquisition startup and restoration failures",
    "[coding_agent][tui][failure][issue58]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto created = coding_agent::create_agent_session(session_options(
        workspace,
        ai::providers::make_scripted_fake_chat_client()));
    REQUIRE(created);

    FailingCleanupTerminal terminal;
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *created->session,
            terminal,
            {.agent_config_directory = config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(run_result);
    REQUIRE_FALSE(*run_result);
    CHECK(run_result->error().message ==
        "Native TUI startup and terminal restoration failed");
    CHECK(run_result->error().detail.find("initial render failed") != std::string::npos);
    CHECK(run_result->error().detail.find("terminal restoration failed") != std::string::npos);
    CHECK(run_result->error().detail.find("sk-render-secret") == std::string::npos);
    CHECK(run_result->error().detail.find("sk-restoration-secret") == std::string::npos);
    CHECK(run_result->error().detail.find("[REDACTED]") != std::string::npos);
    CHECK_FALSE(created->session->is_open());
}

TEST_CASE(
    "Native TUI submits two fresh prompts and replaces streamed assistant state",
    "[coding_agent][tui][issue58]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto created = coding_agent::create_agent_session(session_options(
        workspace,
        ai::providers::make_scripted_fake_chat_client()));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 60, .rows = 10});
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *created->session,
            terminal,
            {.agent_config_directory = config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);
    CHECK(terminal.modes().started);

    REQUIRE(terminal.inject_input("first prompt\r"));
    drain_ready(io);
    CHECK(created->session->message_count() == 2);
    auto screen = visible_screen(terminal);
    CHECK(count_text(screen, "You: first prompt") == 1);
    CHECK(count_text(screen, "Assistant: fake: first prompt") == 1);

    REQUIRE(terminal.inject_input("second prompt\r"));
    drain_ready(io);
    CHECK(created->session->message_count() == 4);
    screen = visible_screen(terminal);
    CHECK(count_text(screen, "Assistant: fake: first prompt") == 1);
    CHECK(count_text(screen, "Assistant: fake: second prompt") == 1);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
    CHECK_FALSE(terminal.modes().started);
    CHECK(terminal.modes().cursor_visible);
}

TEST_CASE(
    "Native TUI preserves batched submissions across deferred prompt dispatch",
    "[coding_agent][tui][async][issue58]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto client = std::make_unique<GatedChatClient>();
    auto* client_pointer = client.get();
    auto created = coding_agent::create_agent_session(session_options(
        workspace,
        std::move(client)));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 60, .rows = 12});
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *created->session,
            terminal,
            {.agent_config_directory = config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(terminal.inject_input("first batched\rsecond batched\r"));
    drain_ready(io);
    REQUIRE(client_pointer->started);
    CHECK(created->session->message_count() == 1);
    auto screen = visible_screen(terminal);
    CHECK(screen.find("You: first batched") != std::string::npos);
    CHECK(screen.find("second batched") != std::string::npos);
    CHECK(screen.find("A prompt is already in flight") != std::string::npos);

    client_pointer->release();
    drain_ready(io);
    CHECK(created->session->message_count() == 2);
    REQUIRE(terminal.inject_input("\r"));
    drain_ready(io);
    CHECK(created->session->message_count() == 3);
    client_pointer->release();
    drain_ready(io);
    CHECK(created->session->message_count() == 4);
    screen = visible_screen(terminal);
    CHECK(screen.find("You: first batched") != std::string::npos);
    CHECK(screen.find("You: second batched") != std::string::npos);
    CHECK(count_text(screen, "Assistant: released") == 2);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI replaces one visible assistant entry during incremental streaming",
    "[coding_agent][tui][streaming][issue58]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto client = std::make_unique<IncrementalGatedChatClient>();
    auto* client_pointer = client.get();
    auto created = coding_agent::create_agent_session(session_options(workspace, std::move(client)));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 30, .rows = 3});
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *created->session,
            terminal,
            {.agent_config_directory = config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(terminal.inject_input("stream\r"));
    drain_ready(io);
    REQUIRE(client_pointer->started);
    auto screen = visible_screen(terminal);
    CHECK(count_text(screen, "TAIL PARTIAL") == 1);
    CHECK(screen.find("old line 1") == std::string::npos);

    client_pointer->release();
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(count_text(screen, "TAIL COMPLETE") == 1);
    CHECK(screen.find("TAIL PARTIAL") == std::string::npos);
    CHECK(screen.find("new line 1") == std::string::npos);
    CHECK(created->session->message_count() == 2);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
    CHECK_FALSE(terminal.modes().started);
}

TEST_CASE(
    "Native TUI keeps input and resize responsive while a prompt is suspended",
    "[coding_agent][tui][async][issue58]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto client = std::make_unique<GatedChatClient>();
    auto* client_pointer = client.get();
    auto created = coding_agent::create_agent_session(session_options(workspace, std::move(client)));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 40, .rows = 8});
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *created->session,
            terminal,
            {.agent_config_directory = config.path()}),
        [&](std::exception_ptr, util::ExpectedVoid result) {
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(terminal.inject_input("wait\r"));
    drain_ready(io);
    REQUIRE(client_pointer->started);
    REQUIRE(terminal.inject_resize({.columns = 30, .rows = 6}));
    REQUIRE(terminal.inject_input("draft"));
    drain_ready(io);
    const tui::TerminalDimensions expected_dimensions{.columns = 30, .rows = 6};
    CHECK(terminal.dimensions() == expected_dimensions);
    CHECK(visible_screen(terminal).find("draft") != std::string::npos);

    REQUIRE(terminal.inject_input("\x03\x04"));
    drain_ready(io);
    CHECK(terminal.modes().started);
    CHECK_FALSE(run_result.has_value());

    client_pointer->release();
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
    CHECK_FALSE(terminal.modes().started);
}

TEST_CASE(
    "Native TUI dispatches its effective commands without changing Agent Session history",
    "[coding_agent][tui][commands][issue60]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto created = coding_agent::create_agent_session(session_options(
        workspace,
        ai::providers::make_scripted_fake_chat_client()));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 100, .rows = 30});
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *created->session,
            terminal,
            {.agent_config_directory = config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(terminal.inject_input("/help\r"));
    drain_ready(io);
    auto screen = visible_screen(terminal);
    CHECK(screen.find("Available commands:") != std::string::npos);
    CHECK(screen.find("/settings") != std::string::npos);
    CHECK(screen.find("/hotkeys") != std::string::npos);
    CHECK(screen.find("/new") == std::string::npos);
    CHECK(screen.find("/resume") == std::string::npos);
    CHECK(screen.find("User Bash") == std::string::npos);
    CHECK(created->session->message_count() == 0);

    REQUIRE(terminal.inject_input("/session\r"));
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(screen.find("Session: " + created->session->session_id()) != std::string::npos);
    CHECK(screen.find("File: In-memory") != std::string::npos);
    CHECK(created->session->message_count() == 0);

    REQUIRE(terminal.inject_input("/commands session\r"));
    drain_ready(io);
    CHECK(visible_screen(terminal).find("Command: /session") != std::string::npos);
    CHECK(created->session->message_count() == 0);

    (void)terminal.check_clear_screen_called();
    REQUIRE(terminal.inject_input("/clear\r"));
    drain_ready(io);
    CHECK(terminal.check_clear_screen_called());
    CHECK(created->session->message_count() == 0);
    screen = visible_screen(terminal);
    CHECK(screen.find("Available commands:") == std::string::npos);
    CHECK(screen.find("Session: " + created->session->session_id()) == std::string::npos);

    REQUIRE(terminal.inject_input("/missing\r"));
    drain_ready(io);
    CHECK(created->session->message_count() == 2);
    screen = visible_screen(terminal);
    CHECK(screen.find("Assistant: fake: /missing") != std::string::npos);
    CHECK(screen.find("Available commands:") == std::string::npos);

    REQUIRE(terminal.inject_input("/exit\r"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
    CHECK_FALSE(terminal.modes().started);
}

TEST_CASE(
    "Native TUI command autocomplete includes effective commands and project resources",
    "[coding_agent][tui][autocomplete][issue60]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    workspace.write(
        ".cpp-harness/skills/project-skill/SKILL.md",
        "---\n"
        "name: project-skill\n"
        "description: Project skill completion.\n"
        "---\n"
        "Project skill body.\n");
    workspace.write(
        ".cpp-harness/prompts/project-prompt.md",
        "---\n"
        "description: Project prompt completion.\n"
        "---\n"
        "Expanded project prompt: $ARGUMENTS\n");
    auto options = session_options(
        workspace,
        ai::providers::make_scripted_fake_chat_client());
    options.load_project_resources = true;
    options.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 100, .rows = 5});
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *created->session,
            terminal,
            {.agent_config_directory = config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(terminal.inject_input("/"));
    drain_ready(io);
    for (std::size_t index = 0; index < 8; ++index) {
        REQUIRE(terminal.inject_input("\x1b[B"));
        drain_ready(io);
    }
    auto screen = visible_screen(terminal);
    CHECK(screen.find("> /settings") != std::string::npos);
    REQUIRE(terminal.inject_input("\t"));
    drain_ready(io);
    CHECK(visible_screen(terminal).find("/settings") != std::string::npos);

    REQUIRE(terminal.inject_input("\x03/set"));
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(screen.find("settings") != std::string::npos);

    REQUIRE(terminal.inject_input("\x03/hot"));
    drain_ready(io);
    CHECK(visible_screen(terminal).find("hotkeys") != std::string::npos);

    REQUIRE(terminal.inject_input("\x03/project"));
    drain_ready(io);
    CHECK(visible_screen(terminal).find("project-prompt") != std::string::npos);

    REQUIRE(terminal.inject_input("\x03/skill:project"));
    drain_ready(io);
    CHECK(visible_screen(terminal).find("skill:project-skill") != std::string::npos);

    REQUIRE(terminal.inject_input("\x03/project-prompt target\r"));
    drain_ready(io);
    CHECK(visible_screen(terminal).find("Expanded project prompt: target") != std::string::npos);

    REQUIRE(terminal.inject_input("/skill:project-skill extra\r"));
    drain_ready(io);
    CHECK(visible_screen(terminal).find("Project skill body.") != std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI settings and hotkeys commands open only supported overlays",
    "[coding_agent][tui][overlays][issue60]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    config.write("keybindings.json", R"({"app.exit":"f6"})");
    auto created = coding_agent::create_agent_session(session_options(
        workspace,
        ai::providers::make_scripted_fake_chat_client()));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 100, .rows = 30});
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *created->session,
            terminal,
            {.agent_config_directory = config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(terminal.inject_input("/settings\r"));
    drain_ready(io);
    auto screen = visible_screen(terminal);
    CHECK(screen.find("Theme") != std::string::npos);
    CHECK(screen.find("Select the active Native TUI theme") != std::string::npos);
    CHECK(created->session->message_count() == 0);

    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    drain_ready(io);
    REQUIRE(terminal.inject_input("/hotkeys\r"));
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(screen.find("Hotkeys") != std::string::npos);
    CHECK(screen.find("f6") != std::string::npos);
    CHECK(screen.find("app.exit") != std::string::npos);
    CHECK(screen.find("app.session.new") == std::string::npos);
    CHECK(screen.find("app.suspend") == std::string::npos);
    CHECK(screen.find("app.clipboard.pasteImage") == std::string::npos);
    CHECK(created->session->message_count() == 0);

    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    drain_ready(io);
    REQUIRE(terminal.inject_input("\x1b[17~"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
    CHECK_FALSE(terminal.modes().started);
}

TEST_CASE(
    "Native TUI redacts and bounds prompt failures before allowing retry",
    "[coding_agent][tui][failure][issue58]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto created = coding_agent::create_agent_session(session_options(
        workspace,
        std::make_unique<FailOnceChatClient>()));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 100, .rows = 12});
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *created->session,
            terminal,
            {.agent_config_directory = config.path()}),
        [&](std::exception_ptr, util::ExpectedVoid result) {
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(terminal.inject_input("fail\r"));
    drain_ready(io);
    auto screen = visible_screen(terminal);
    CHECK(screen.find("sk-abcdefghijklmnopqrstuvwxyz123456") == std::string::npos);
    CHECK(screen.find("[REDACTED]") != std::string::npos);
    CHECK(screen.size() < 12000);

    REQUIRE(terminal.inject_input("\x03retry\r"));
    drain_ready(io);
    CHECK(created->session->message_count() == 3);
    CHECK(visible_screen(terminal).find("Assistant: recovered") != std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
    CHECK_FALSE(terminal.modes().started);
}
