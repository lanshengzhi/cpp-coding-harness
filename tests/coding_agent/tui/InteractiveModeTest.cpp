#include "coding_agent/tui/InteractiveMode.hpp"
#include "support/ModelsFixture.hpp"
#include "support/ImageFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/Content.hpp>
#include <cch/harness/session/JsonlSessionStore.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "ai/providers/FakeProvider.hpp"
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/BoundedText.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "harness/session/SessionJournalTestHooks.hpp"

#include "../../../third_party/catch2/catch_test_macros.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] tests::ModelsSessionOptions session_options(
    const tests::TempWorkspace& workspace,
    std::shared_ptr<ai::Provider> client) {
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
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

[[nodiscard]] std::string read_binary_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

/// One resumed session with a user message and an assistant message carrying
/// a thinking block, mirroring the `[issue59]` rich-transcript fixture.
struct RichThinkingSession {
    tests::TempWorkspace workspace;
    std::filesystem::path session_file;

    void create() {
        session_file = workspace.path() / "rich-session.jsonl";
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
            ai::user_text_message("resume request", 1'700'000'000'000)}));
        ai::AssistantMessage assistant;
        assistant.provider = "fake";
        assistant.api = "fake";
        assistant.model = "fake-model";
        assistant.stop_reason = ai::AssistantStopReason::ToolUse;
        assistant.timestamp = 1'700'000'000'001;
        assistant.content.emplace_back(ai::thinking_content(
            "inspect the saved state\nTHINKING END"));
        assistant.content.emplace_back(ai::text_content(
            "I will read the persisted file."));
        REQUIRE(store->append(ai::MessageVariant{assistant}));
    }

    [[nodiscard]] auto resume() const {
        coding_agent::runtime::AgentSessionCreationRequest request;
        request.session_target = coding_agent::ExplicitResumeSessionTarget{session_file};
        request.workspace = workspace.path();
        request.no_skills = true;
        request.no_prompt_templates = true;
        auto resumed = coding_agent::create_agent_session_for_testing(
            std::move(request), ai::providers::make_scripted_fake_models());
        REQUIRE(resumed);
        return resumed;
    }
};

/// The column (0-based) at which `needle` starts in `screen`, counting from
/// the beginning of its line.
[[nodiscard]] std::size_t column_of(
    std::string_view screen,
    std::string_view needle) {
    const auto position = screen.find(needle);
    REQUIRE(position != std::string_view::npos);
    const auto line_start = screen.rfind('\n', position);
    return position - (line_start == std::string_view::npos ? 0 : line_start + 1);
}

class FakeClipboardReader final : public coding_agent::tui::AsyncClipboardReader {
public:
    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<coding_agent::tui::ClipboardImage>>>
    read_image() override {
        if (image_error) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Process,
                "clipboard image unavailable"));
        }
        if (next_image >= images.size()) co_return std::nullopt;
        co_return std::optional<coding_agent::tui::ClipboardImage>{images[next_image++]};
    }

    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<std::string>>>
    read_text() override {
        if (text_error) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Process,
                "clipboard text unavailable"));
        }
        co_return text;
    }

    std::vector<coding_agent::tui::ClipboardImage> images;
    std::optional<std::string> text{std::nullopt};
    std::size_t next_image{0};
    bool image_error{false};
    bool text_error{false};
};

class FailOnceChatProvider final : public tests::ScriptedProvider {
public:
    FailOnceChatProvider() : ScriptedProvider("sdk-host") {}
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
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
        response.model = model.id;
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
    [[nodiscard]] util::ExpectedVoid set_title(std::string_view) override { return {}; }
    [[nodiscard]] util::ExpectedVoid set_progress(bool) override { return {}; }
    [[nodiscard]] util::ExpectedVoid drain_input(
        std::chrono::milliseconds,
        std::chrono::milliseconds) override {
        return {};
    }
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
    [[nodiscard]] util::ExpectedVoid set_title(std::string_view) override { return {}; }
    [[nodiscard]] util::ExpectedVoid set_progress(bool) override { return {}; }
    [[nodiscard]] util::ExpectedVoid drain_input(
        std::chrono::milliseconds,
        std::chrono::milliseconds) override {
        return {};
    }

private:
    tui::TerminalModeState modes_;
};

class AbortAwareInteractiveChatProvider final : public tests::ScriptedProvider {
public:
    AbortAwareInteractiveChatProvider() : ScriptedProvider("sdk-host") {}
    AbortAwareInteractiveChatProvider(AbortAwareInteractiveChatProvider&&) = delete;
    AbortAwareInteractiveChatProvider& operator=(AbortAwareInteractiveChatProvider&&) = delete;
    ~AbortAwareInteractiveChatProvider() override = default;
    AbortAwareInteractiveChatProvider(const AbortAwareInteractiveChatProvider&) = delete;
    AbortAwareInteractiveChatProvider& operator=(const AbortAwareInteractiveChatProvider&) = delete;

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink sink) override {
        ++request_count;
        if (request_count > 1) {
            recovery_uses_fresh_token = first_stop_token && options.stop_token != *first_stop_token;
            recovery_stop_requested = options.stop_token.stop_requested();
            auto recovered = ai::assistant_text_message("recovered after TUI abort");
            recovered.provider = "abort-aware-fake";
            recovered.api = "fake";
            recovered.model = model.id;
            co_return recovered;
        }

        const auto stop_token = options.stop_token;
        first_stop_token = stop_token;
        auto partial = ai::assistant_text_message("");
        partial.provider = "abort-aware-fake";
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
            "abort-aware fake released without cancellation"));
    }

    bool started{false};
    bool recovery_uses_fresh_token{false};
    bool recovery_stop_requested{true};
    std::size_t request_count{0};
    std::size_t stop_callback_count{0};
    std::optional<std::stop_token> first_stop_token;

private:
    std::optional<boost::asio::steady_timer> gate_;
};

class GatedChatProvider final : public tests::ScriptedProvider {
public:
    GatedChatProvider() : ScriptedProvider("sdk-host") {}
    GatedChatProvider(GatedChatProvider&&) = delete;
    GatedChatProvider& operator=(GatedChatProvider&&) = delete;
    ~GatedChatProvider() override = default;
    GatedChatProvider(const GatedChatProvider&) = delete;
    GatedChatProvider& operator=(const GatedChatProvider&) = delete;

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink sink) override {
        auto partial = ai::assistant_text_message("");
        partial.content.clear();
        partial.provider = "fake";
        partial.api = "fake";
        partial.model = model.id;
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
        response.model = model.id;
        co_return response;
    }

    void release() {
        if (gate_) (void)gate_->cancel();
    }

    bool started{false};

private:
    std::optional<boost::asio::steady_timer> gate_;
};

class TurnGatedChatProvider final : public tests::ScriptedProvider {
public:
    TurnGatedChatProvider() : ScriptedProvider("sdk-host") {}
    TurnGatedChatProvider(TurnGatedChatProvider&&) = delete;
    TurnGatedChatProvider& operator=(TurnGatedChatProvider&&) = delete;
    ~TurnGatedChatProvider() override = default;
    TurnGatedChatProvider(const TurnGatedChatProvider&) = delete;
    TurnGatedChatProvider& operator=(const TurnGatedChatProvider&) = delete;

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink) override {
        ++request_count;
        std::vector<std::string> users;
        for (const auto& message : context.messages) {
            if (const auto* user = std::get_if<ai::UserMessage>(&message)) {
                users.push_back(ai::text_from_user_message(*user));
            }
        }
        observed_users.push_back(std::move(users));

        const auto executor = co_await boost::asio::this_coro::executor;
        gate_.emplace(executor);
        gate_->expires_at(std::chrono::steady_clock::time_point::max());
        boost::system::error_code error;
        co_await gate_->async_wait(
            boost::asio::redirect_error(boost::asio::use_awaitable, error));

        auto response = ai::assistant_text_message(std::format("turn {}", request_count));
        response.provider = "turn-gated-fake";
        response.api = "fake";
        response.model = model.id;
        if (first_request_errors && request_count == 1) {
            response.stop_reason = ai::AssistantStopReason::Error;
            response.error_message = "accepted queued error";
        }
        co_return response;
    }

    void release() {
        if (gate_) (void)gate_->cancel();
    }

    bool first_request_errors{false};
    std::size_t request_count{0};
    std::vector<std::vector<std::string>> observed_users;

private:
    std::optional<boost::asio::steady_timer> gate_;
};

class RepeatedReadCallChatProvider final : public tests::ScriptedProvider {
public:
    RepeatedReadCallChatProvider() : ScriptedProvider("sdk-host") {}
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink sink) override {
        auto response = ai::assistant_text_message("tool cycle complete");
        response.provider = "tool-fake";
        response.api = "fake";
        response.model = model.id;
        if (!context.messages.empty() &&
            std::holds_alternative<ai::ToolResultMessage>(context.messages.back())) {
            co_return response;
        }

        std::string prompt;
        for (auto message = context.messages.rbegin();
             message != context.messages.rend();
             ++message) {
            if (const auto* user = std::get_if<ai::UserMessage>(&*message)) {
                prompt = ai::text_from_user_message(*user);
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
            "probe-read",
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
            "probe-read",
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
            "probe-read",
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
        definition_.name = "probe-read";
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

class ImageReadTool final : public agent::AsyncAgentTool {
public:
    explicit ImageReadTool(std::string image_data)
        : image_data_(std::move(image_data)) {
        definition_.name = "probe-read";
        definition_.description = "Return deterministic text and image content";
        definition_.parameters = util::JsonValue::object_t{{"type", "object"}};
    }

    [[nodiscard]] const ai::Tool& definition() const override {
        return definition_;
    }

    [[nodiscard]] boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> execute(
        agent::ToolInvocation,
        std::stop_token) override {
        agent::AsyncToolExecutionResult result;
        result.content = {
            ai::text_content("live tool before"),
            ai::image_content(image_data_, "image/png"),
            ai::text_content("live tool after"),
        };
        co_return result;
    }

private:
    ai::Tool definition_;
    std::string image_data_;
};

class DelayedCancellationTool final : public agent::AsyncAgentTool {
public:
    DelayedCancellationTool() {
        definition_.name = "delayed";
        definition_.description = "Wait until cancellation is allowed to quiesce";
        definition_.parameters = util::JsonValue::object_t{{"type", "object"}};
    }
    DelayedCancellationTool(DelayedCancellationTool&&) = delete;
    DelayedCancellationTool& operator=(DelayedCancellationTool&&) = delete;
    ~DelayedCancellationTool() override = default;
    DelayedCancellationTool(const DelayedCancellationTool&) = delete;
    DelayedCancellationTool& operator=(const DelayedCancellationTool&) = delete;

    [[nodiscard]] const ai::Tool& definition() const override {
        return definition_;
    }

    [[nodiscard]] boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> execute(
        agent::ToolInvocation,
        std::stop_token) override {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Tool,
            "delayed tool requires the update path"));
    }

    [[nodiscard]] boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>>
    execute_with_updates(
        agent::ToolInvocation,
        std::stop_token stop_token,
        agent::ToolUpdateSink update_sink) override {
        observed_stop_token = stop_token;
        agent::AsyncToolExecutionResult partial;
        partial.content.emplace_back(ai::text_content("partial tool output before abort"));
        if (auto updated = update_sink(partial); !updated) {
            co_return std::unexpected(updated.error());
        }

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

        if (stop_token.stop_requested()) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Cancelled,
                "delayed tool aborted"));
        }
        co_return agent::AsyncToolExecutionResult{};
    }

    void release() {
        if (gate_) (void)gate_->cancel();
    }

    bool started{false};
    std::size_t stop_callback_count{0};
    std::optional<std::stop_token> observed_stop_token;

private:
    ai::Tool definition_;
    std::optional<boost::asio::steady_timer> gate_;
};

class AbortThroughToolChatProvider final : public tests::ScriptedProvider {
public:
    AbortThroughToolChatProvider() : ScriptedProvider("sdk-host") {}
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink sink) override {
        ++request_count;
        if (options.stop_token.stop_requested()) {
            completion_stop_token = options.stop_token;
            auto aborted = ai::assistant_text_message("");
            aborted.provider = "tool-abort-fake";
            aborted.api = "fake";
            aborted.model = model.id;
            aborted.stop_reason = ai::AssistantStopReason::Aborted;
            aborted.error_message = "tool prompt aborted";
            if (auto emitted = sink(ai::AssistantErrorEvent{
                    .reason = ai::AssistantStopReason::Aborted,
                    .error = aborted,
                });
                !emitted) {
                co_return std::unexpected(emitted.error());
            }
            co_return aborted;
        }

        std::string prompt;
        for (auto message = context.messages.rbegin();
             message != context.messages.rend();
             ++message) {
            if (const auto* user = std::get_if<ai::UserMessage>(&*message)) {
                prompt = ai::text_from_user_message(*user);
                break;
            }
        }
        if (prompt == "recover after tool abort") {
            auto recovered = ai::assistant_text_message("recovered after tool abort");
            recovered.provider = "tool-abort-fake";
            recovered.api = "fake";
            recovered.model = model.id;
            co_return recovered;
        }

        first_stop_token = options.stop_token;
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
    std::optional<std::stop_token> first_stop_token;
    std::optional<std::stop_token> completion_stop_token;
};

class AcceptedOutcomeChatProvider final : public tests::ScriptedProvider {
public:
    AcceptedOutcomeChatProvider() : ScriptedProvider("sdk-host") {}
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink) override {
        std::string prompt;
        for (auto message = context.messages.rbegin();
             message != context.messages.rend();
             ++message) {
            if (const auto* user = std::get_if<ai::UserMessage>(&*message)) {
                prompt = ai::text_from_user_message(*user);
                break;
            }
        }

        auto response = ai::assistant_text_message(
            prompt == "recover" ? "recovered after accepted outcomes" : "partial response");
        response.provider = "outcome-fake";
        response.api = "fake";
        response.model = model.id;
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

class IncrementalGatedChatProvider final : public tests::ScriptedProvider {
public:
    IncrementalGatedChatProvider() : ScriptedProvider("sdk-host") {}
    IncrementalGatedChatProvider(IncrementalGatedChatProvider&&) = delete;
    IncrementalGatedChatProvider& operator=(IncrementalGatedChatProvider&&) = delete;
    ~IncrementalGatedChatProvider() override = default;
    IncrementalGatedChatProvider(const IncrementalGatedChatProvider&) = delete;
    IncrementalGatedChatProvider& operator=(const IncrementalGatedChatProvider&) = delete;

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink sink) override {
        auto partial = ai::assistant_text_message("");
        partial.content.clear();
        partial.provider = "fake";
        partial.api = "incremental-fake";
        partial.model = model.id;
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
    "Native TUI renders resumed and live message images in source order without mutating content",
    "[coding_agent][tui][image][issue63]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    const auto session_file = workspace.path() / "image-session.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(
        session_file,
        {
            .session_id = "image-session",
            .created_at = "2026-07-28T00:00:00Z",
            .workspace = workspace.path(),
            .provider = "fake",
            .model = "fake-model",
        });
    REQUIRE(store);

    const auto png_data = std::string{tests::kTinyPngBase64};
    ai::UserMessage user;
    user.timestamp = 1'700'000'000'000;
    user.content = std::vector<ai::Content>{
        ai::text_content("user before"),
        ai::image_content(png_data, "image/png"),
        ai::text_content("user after"),
    };
    REQUIRE(store->append(ai::MessageVariant{user}));

    ai::AssistantMessage assistant;
    assistant.provider = "fake";
    assistant.api = "fake";
    assistant.model = "fake-model";
    assistant.stop_reason = ai::AssistantStopReason::ToolUse;
    assistant.timestamp = 1'700'000'000'001;
    assistant.content.emplace_back(ai::tool_call_content("image-call", "read", "{}"));
    REQUIRE(store->append(ai::MessageVariant{assistant}));

    ai::ToolResultMessage tool_result;
    tool_result.tool_call_id = "image-call";
    tool_result.tool_name = "read";
    tool_result.timestamp = 1'700'000'000'002;
    tool_result.content = {
        ai::text_content("tool before"),
        ai::image_content(png_data, "image/png"),
        ai::text_content("tool after"),
    };
    REQUIRE(store->append(ai::MessageVariant{tool_result}));

    ai::CustomMessage custom;
    custom.custom_type = "image-notice";
    custom.timestamp = 1'700'000'000'003;
    custom.content = {
        ai::text_content("custom before"),
        ai::image_content(png_data, "image/png"),
        ai::text_content("after png"),
        ai::image_content(std::string{tests::kTinyJpegBase64}, "image/jpeg"),
        ai::text_content("after jpeg"),
        ai::image_content(std::string{tests::kTinyGifBase64}, "image/gif"),
        ai::text_content("after gif"),
        ai::image_content(std::string{tests::kTinyWebpBase64}, "image/webp"),
        ai::text_content("custom after"),
    };
    REQUIRE(store->append(ai::MessageVariant{custom}));

    tests::ModelsSessionOptions resume_options;
    resume_options.session_target = coding_agent::ExplicitResumeSessionTarget{session_file};
    resume_options.workspace = workspace.path();
    resume_options.models = ai::providers::make_scripted_fake_models();
    auto resumed = coding_agent::create_agent_session(std::move(resume_options));
    REQUIRE(resumed);
    const auto authoritative_before = resumed->session->snapshot().agent_state.messages;

    tui::VirtualTerminal terminal({
        .columns = 72,
        .rows = 300,
        .capabilities = {
            .synchronized_output = true,
            .inline_images = tui::InlineImageProtocol::ITerm2,
            .cell_pixels = tui::CellPixelDimensions{.width = 9, .height = 18},
        },
    });
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

    REQUIRE(terminal.images().size() == 6);
    std::array<std::uint64_t, 6> resumed_resource_ids{};
    for (std::size_t index = 0; index < resumed_resource_ids.size(); ++index) {
        resumed_resource_ids[index] = terminal.images()[index].resource_id;
        if (index > 0) {
            CHECK(terminal.images()[index - 1].region.row < terminal.images()[index].region.row);
        }
    }
    const std::array<std::string_view, 6> resumed_mime_types{
        "image/png", "image/png", "image/png", "image/jpeg", "image/gif", "image/webp"};
    for (std::size_t index = 0; index < resumed_mime_types.size(); ++index) {
        CHECK(terminal.images()[index].mime_type == resumed_mime_types[index]);
    }
    const auto resumed_screen = visible_screen(terminal);
    CHECK(resumed_screen.find("user before") < resumed_screen.find("user after"));
    CHECK(resumed_screen.find("tool before") < resumed_screen.find("tool after"));
    CHECK(resumed_screen.find("custom before") < resumed_screen.find("custom after"));

    coding_agent::PromptOptions live_options;
    live_options.images.push_back(ai::image_content(png_data, "image/png"));
    std::optional<util::ExpectedVoid> prompt_result;
    boost::asio::co_spawn(
        io,
        resumed->session->prompt("live user text", std::move(live_options)),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            CHECK(exception == nullptr);
            prompt_result.emplace(std::move(result));
        });
    drain_ready(io);
    REQUIRE(prompt_result);
    CHECK(*prompt_result);
    REQUIRE(terminal.images().size() == 7);
    for (std::size_t index = 0; index < resumed_resource_ids.size(); ++index) {
        CHECK(terminal.images()[index].resource_id == resumed_resource_ids[index]);
    }
    CHECK(visible_screen(terminal).find("live user text") != std::string::npos);

    const auto authoritative_after = resumed->session->snapshot().agent_state.messages;
    REQUIRE(authoritative_after.size() == authoritative_before.size() + 2);
    const auto* live_user = std::get_if<ai::UserMessage>(
        &authoritative_after[authoritative_before.size()]);
    REQUIRE(live_user != nullptr);
    REQUIRE(std::get<std::vector<ai::Content>>(live_user->content).size() == 2);
    CHECK(std::get<ai::ImageContent>(std::get<std::vector<ai::Content>>(live_user->content)[1]).data == png_data);
    CHECK(std::get<ai::ImageContent>(
        std::get<std::vector<ai::Content>>(
            std::get<ai::UserMessage>(authoritative_after[0]).content)[1])
        .data == png_data);
    CHECK(std::get<ai::ImageContent>(
        std::get<ai::ToolResultMessage>(authoritative_after[2]).content[1]).data == png_data);
    CHECK(std::get<ai::ImageContent>(
        std::get<ai::CustomMessage>(authoritative_after[3]).content[1]).data == png_data);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);

    tests::ModelsSessionOptions fallback_resume;
    fallback_resume.session_target = coding_agent::ExplicitResumeSessionTarget{session_file};
    fallback_resume.workspace = workspace.path();
    fallback_resume.models = ai::providers::make_scripted_fake_models();
    auto fallback_session = coding_agent::create_agent_session(std::move(fallback_resume));
    REQUIRE(fallback_session);
    const auto fallback_before = fallback_session->session->snapshot().agent_state.messages;
    tui::VirtualTerminal fallback_terminal({.columns = 72, .rows = 50});
    boost::asio::io_context fallback_io;
    std::optional<util::ExpectedVoid> fallback_result;
    boost::asio::co_spawn(
        fallback_io,
        coding_agent::tui::run_interactive_mode(
            *fallback_session->session,
            fallback_terminal,
            {.agent_config_directory = config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            CHECK(exception == nullptr);
            fallback_result.emplace(std::move(result));
        });
    drain_ready(fallback_io);

    const auto fallback_screen = visible_screen(fallback_terminal);
    CHECK(count_text(fallback_screen, "[Image:") == 7);
    CHECK(fallback_screen.find("[image/png]") != std::string::npos);
    CHECK(fallback_screen.find("[image/jpeg]") != std::string::npos);
    CHECK(fallback_screen.find("[image/gif]") != std::string::npos);
    CHECK(fallback_screen.find("[image/webp]") != std::string::npos);
    CHECK(fallback_terminal.images().empty());
    const auto fallback_after = fallback_session->session->snapshot().agent_state.messages;
    REQUIRE(fallback_after.size() == fallback_before.size());
    CHECK(std::get<ai::ImageContent>(
        std::get<std::vector<ai::Content>>(
            std::get<ai::UserMessage>(fallback_after[0]).content)[1])
        .data == png_data);

    REQUIRE(fallback_terminal.inject_input("\x04"));
    drain_ready(fallback_io);
    REQUIRE(fallback_result);
    CHECK(*fallback_result);
}

TEST_CASE(
    "Native TUI renders a live image-bearing tool result through the transcript path",
    "[coding_agent][tui][image][tool][issue63]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config_directory;
    auto options = session_options(
        workspace,
        std::make_shared<RepeatedReadCallChatProvider>());
    options.custom_tools.push_back(std::make_unique<ImageReadTool>(
        std::string{tests::kTinyPngBase64}));
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created);

    tui::VirtualTerminal terminal({
        .columns = 80,
        .rows = 80,
        .capabilities = {
            .synchronized_output = true,
            .inline_images = tui::InlineImageProtocol::ITerm2,
            .cell_pixels = tui::CellPixelDimensions{.width = 9, .height = 18},
        },
    });
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *created->session,
            terminal,
            {.agent_config_directory = config_directory.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);
    REQUIRE(terminal.inject_input("read live.png\r"));
    drain_ready(io);

    REQUIRE(terminal.images().size() == 1);
    CHECK(terminal.images()[0].mime_type == "image/png");
    const auto screen = visible_screen(terminal);
    CHECK(screen.find("live tool before") < screen.find("live tool after"));
    const auto snapshot = created->session->snapshot();
    const auto result = std::find_if(
        snapshot.agent_state.messages.begin(),
        snapshot.agent_state.messages.end(),
        [](const ai::MessageVariant& message) {
            return std::holds_alternative<ai::ToolResultMessage>(message);
        });
    REQUIRE(result != snapshot.agent_state.messages.end());
    const auto& tool_result = std::get<ai::ToolResultMessage>(*result);
    REQUIRE(tool_result.content.size() == 3);
    CHECK(std::get<ai::ImageContent>(tool_result.content[1]).data ==
        tests::kTinyPngBase64);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI keeps tail image sidecars inside a cropped transcript viewport",
    "[coding_agent][tui][image][viewport][issue63]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config_directory;
    const auto session_file = workspace.path() / "viewport-images.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(
        session_file,
        {
            .session_id = "viewport-images",
            .created_at = "2026-07-28T00:00:00Z",
            .workspace = workspace.path(),
            .provider = "fake",
            .model = "fake-model",
        });
    REQUIRE(store);
    for (std::size_t index = 0; index < 30; ++index) {
        REQUIRE(store->append(ai::MessageVariant{
            ai::user_text_message("old transcript line " + std::to_string(index),
                1'700'000'000'000 + static_cast<ai::TimestampMs>(index))}));
    }
    ai::CustomMessage tail;
    tail.custom_type = "tail";
    tail.content = {
        ai::text_content("tail before"),
        ai::image_content(std::string{tests::kTinyPngBase64}, "image/png"),
        ai::text_content("tail after"),
    };
    tail.timestamp = 1'700'000'000'100;
    REQUIRE(store->append(ai::MessageVariant{tail}));

    tests::ModelsSessionOptions resume;
    resume.session_target = coding_agent::ExplicitResumeSessionTarget{session_file};
    resume.workspace = workspace.path();
    resume.models = ai::providers::make_scripted_fake_models();
    auto created = coding_agent::create_agent_session(std::move(resume));
    REQUIRE(created);
    tui::VirtualTerminal terminal({
        .columns = 72,
        .rows = 42,
        .capabilities = {
            .synchronized_output = true,
            .inline_images = tui::InlineImageProtocol::ITerm2,
            .cell_pixels = tui::CellPixelDimensions{.width = 9, .height = 18},
        },
    });
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *created->session,
            terminal,
            {.agent_config_directory = config_directory.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    REQUIRE(terminal.images().size() == 1);
    const auto& image = terminal.images()[0];
    CHECK(image.region.row + image.region.rows < terminal.dimensions().rows);
    const auto screen = visible_screen(terminal);
    CHECK(screen.find("tail before") < screen.find("tail after"));
    CHECK(screen.find("old transcript line 0") == std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI clipboard images become distinct supported temporary paths",
    "[coding_agent][tui][clipboard][issue63]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config_directory;
    auto options = session_options(
        workspace,
        ai::providers::make_scripted_fake_provider());
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created);

    auto reader = std::make_unique<FakeClipboardReader>();
    const std::array source_bytes{
        tests::decode_base64(tests::kTinyPngBase64),
        tests::decode_base64(tests::kTinyJpegBase64),
        tests::decode_base64(tests::kTinyGifBase64),
        tests::decode_base64(tests::kTinyWebpBase64),
    };
    for (const auto& bytes : source_bytes) {
        reader->images.push_back({
            .bytes = bytes,
            .mime_type = "application/octet-stream",
        });
    }

    coding_agent::tui::InteractiveModeConfig config;
    config.agent_config_directory = config_directory.path();
    config.clipboard_reader = std::move(reader);
    tui::VirtualTerminal terminal({.columns = 120, .rows = 16});
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *created->session,
            terminal,
            std::move(config)),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    for (std::size_t index = 0; index < source_bytes.size(); ++index) {
        REQUIRE(terminal.inject_input("\x16"));
        drain_ready(io);
        if (index + 1 < source_bytes.size()) {
            REQUIRE(terminal.inject_input(" "));
            drain_ready(io);
        }
    }
    REQUIRE(terminal.inject_input("\r"));
    drain_ready(io);

    const auto snapshot = created->session->snapshot();
    REQUIRE(snapshot.agent_state.messages.size() >= 2);
    const auto* user = std::get_if<ai::UserMessage>(&snapshot.agent_state.messages[0]);
    REQUIRE(user != nullptr);
    std::istringstream paths{ai::text_from_user_message(*user)};
    std::vector<std::filesystem::path> inserted_paths;
    for (std::filesystem::path path; paths >> path;) inserted_paths.push_back(std::move(path));
    REQUIRE(inserted_paths.size() == 4);
    const std::array<std::string_view, 4> extensions{".png", ".jpg", ".gif", ".webp"};
    for (std::size_t index = 0; index < inserted_paths.size(); ++index) {
        CHECK(inserted_paths[index].extension() == extensions[index]);
        CHECK(read_binary_file(inserted_paths[index]) == std::string(
            reinterpret_cast<const char*>(source_bytes[index].data()),
            source_bytes[index].size()));
        if (index > 0) CHECK(inserted_paths[index] != inserted_paths[index - 1]);
    }

    REQUIRE(terminal.inject_input("/hotkeys\r"));
    drain_ready(io);
    CHECK(visible_screen(terminal).find("app.clipboard.pasteImage") != std::string::npos);
    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    drain_ready(io);
    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);

    for (const auto& path : inserted_paths) {
        std::error_code remove_error;
        CHECK(std::filesystem::remove(path, remove_error));
        CHECK_FALSE(remove_error);
    }
}

TEST_CASE(
    "Native TUI clipboard falls back to text at the cursor and ignores read failures",
    "[coding_agent][tui][clipboard][issue63]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config_directory;
    auto options = session_options(
        workspace,
        ai::providers::make_scripted_fake_provider());
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created);

    auto reader = std::make_unique<FakeClipboardReader>();
    reader->images.push_back({
        .bytes = {0x01, 0x02, 0x03},
        .mime_type = "image/png",
    });
    reader->text = "clipboard text";
    coding_agent::tui::InteractiveModeConfig config;
    config.agent_config_directory = config_directory.path();
    config.clipboard_reader = std::move(reader);

    tui::VirtualTerminal terminal({.columns = 80, .rows = 12});
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode(
            *created->session,
            terminal,
            std::move(config)),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);
    REQUIRE(terminal.inject_input("AB\x1b[D\x16"));
    drain_ready(io);
    REQUIRE(terminal.inject_input("\r"));
    drain_ready(io);

    const auto snapshot = created->session->snapshot();
    const auto& user = std::get<ai::UserMessage>(snapshot.agent_state.messages[0]);
    const auto pasted_text = ai::text_from_user_message(user);
    // Typed uppercase letters keep their case (pi's "shift+letter produces
    // uppercase" contract; the decoder canonicalizes identifiers to
    // lowercase but inserted text preserves the typed case).
    CHECK(pasted_text == "Aclipboard textB");
    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);

    auto failed_options = session_options(
        workspace,
        ai::providers::make_scripted_fake_provider());
    auto failed_session = coding_agent::create_agent_session(std::move(failed_options));
    REQUIRE(failed_session);
    auto failed_reader = std::make_unique<FakeClipboardReader>();
    failed_reader->image_error = true;
    failed_reader->text_error = true;
    coding_agent::tui::InteractiveModeConfig failed_config;
    failed_config.agent_config_directory = config_directory.path();
    failed_config.clipboard_reader = std::move(failed_reader);
    tui::VirtualTerminal failed_terminal({.columns = 80, .rows = 12});
    boost::asio::io_context failed_io;
    std::optional<util::ExpectedVoid> failed_result;
    boost::asio::co_spawn(
        failed_io,
        coding_agent::tui::run_interactive_mode(
            *failed_session->session,
            failed_terminal,
            std::move(failed_config)),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            CHECK(exception == nullptr);
            failed_result.emplace(std::move(result));
        });
    drain_ready(failed_io);
    REQUIRE(failed_terminal.inject_input("unchanged\x16"));
    drain_ready(failed_io);
    REQUIRE(failed_terminal.inject_input("\r"));
    drain_ready(failed_io);
    const auto failed_snapshot = failed_session->session->snapshot();
    CHECK(ai::text_from_user_message(
        std::get<ai::UserMessage>(failed_snapshot.agent_state.messages[0])) ==
        "unchanged");
    CHECK(visible_screen(failed_terminal).find("clipboard image unavailable") == std::string::npos);
    REQUIRE(failed_terminal.inject_input("\x04"));
    drain_ready(failed_io);
    REQUIRE(failed_result);
    CHECK(*failed_result);
}

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
    resume.no_skills = true;
    resume.no_prompt_templates = true;
    auto resumed = coding_agent::create_agent_session_for_testing(
        std::move(resume), ai::providers::make_scripted_fake_models());
    REQUIRE(resumed);

    const auto authoritative_before = resumed->session->snapshot().agent_state.messages;
    REQUIRE(authoritative_before.size() == 6);
    // The pi-shaped composition reserves fixed rows for the status, the
    // editor borders, and the footer; 52 rows keep the full rich transcript
    // (compaction summary through branch summary) in the chat viewport.
    tui::VirtualTerminal terminal({.columns = 72, .rows = 52});
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
    const auto user_position = screen.find("resume request");
    const auto thinking_position = screen.find("inspect the saved state");
    const auto assistant_position = screen.find("I will read the persisted file.");
    const auto tool_position = screen.find("read saved.txt");
    const auto result_position = screen.find("persisted tool output");
    const auto followup_position = screen.find("I will continue after the tool.");
    const auto custom_position = screen.find("[notice]");
    const auto compaction_position = screen.find("Compacted from 1,200 tokens");
    const auto branch_position = screen.find("[branch]");
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
    // pi renders tool components after the whole assistant message.
    CHECK(assistant_position < followup_position);
    CHECK(followup_position < tool_position);
    CHECK(tool_position < result_position);
    CHECK(result_position < custom_position);
    CHECK(custom_position < branch_position);
    REQUIRE(terminal.inject_input("\x1b[19~"));
    drain_ready(io);
    CHECK(visible_screen(terminal).find("inspect the saved state") == std::string::npos);
    CHECK(visible_screen(terminal).find("THINKING END") == std::string::npos);
    // pi assistant-message.ts hidden-thinking label.
    CHECK(visible_screen(terminal).find("Thinking...") != std::string::npos);
    REQUIRE(terminal.inject_input("\x1b[19~"));
    drain_ready(io);
    CHECK(visible_screen(terminal).find("inspect the saved state") != std::string::npos);

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
    CHECK(visible_screen(terminal).find("fake: continue here") != std::string::npos);

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
        ai::providers::make_scripted_fake_provider());
    create.session_target = coding_agent::ExplicitOpenOrCreateSessionTarget{session_file};
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

    tests::ModelsSessionOptions resume;
    resume.session_target = coding_agent::ExplicitResumeSessionTarget{session_file};
    resume.workspace = workspace.path();
    resume.models = ai::providers::make_scripted_fake_models();
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
        std::make_shared<RepeatedReadCallChatProvider>());
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
    // pi tool-execution: one title per tool component; the call id is not
    // presented.
    CHECK(count_text(screen, "probe-read") == 1);
    CHECK(count_text(screen, "partial tool output") == 1);
    CHECK(screen.find("STALE ARGUMENT") == std::string::npos);
    CHECK(screen.find(R"({"path":"large.txt"})") != std::string::npos);
    CHECK(screen.find("STALE SNAPSHOT") == std::string::npos);
    CHECK(count_text(screen, "LATEST SNAPSHOT") == 1);

    tool_pointer->release();
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(count_text(screen, "probe-read") == 1);
    CHECK(screen.find("line 1") != std::string::npos);
    CHECK(screen.find("TOOL OUTPUT END") == std::string::npos);
    REQUIRE(tool_pointer->emit_late_update());
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(count_text(screen, "probe-read") == 1);
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
    CHECK(count_text(screen, "probe-read") == 1);
    CHECK(count_text(screen, "partial tool output") == 1);
    CHECK(screen.find("STALE SNAPSHOT") == std::string::npos);
    CHECK(count_text(screen, "LATEST SNAPSHOT") == 1);

    tool_pointer->release();
    drain_ready(io);
    screen = visible_screen(terminal);
    // The repeated call id settles one component; the failure renders its
    // error text in the error-colored box.
    CHECK(count_text(screen, "probe-read") == 1);
    CHECK(screen.find("final tool failure") != std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI interrupt cancels autocomplete before aborting active work and recovers",
    "[coding_agent][tui][abort][issue61]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto client = std::make_shared<AbortAwareInteractiveChatProvider>();
    auto* client_pointer = client.get();
    auto created = coding_agent::create_agent_session(session_options(
        workspace,
        std::move(client)));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 72, .rows = 16});
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

    REQUIRE(terminal.inject_input("cancel me\r"));
    drain_ready(io);
    REQUIRE(client_pointer->started);
    CHECK(created->session->is_busy());
    CHECK(visible_screen(terminal).find("partial assistant output") != std::string::npos);

    REQUIRE(terminal.inject_input("/"));
    drain_ready(io);
    CHECK(visible_screen(terminal).find("/clear") != std::string::npos);
    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    drain_ready(io);
    CHECK(created->session->is_busy());
    CHECK(client_pointer->stop_callback_count == 0);
    CHECK(visible_screen(terminal).find("/clear") == std::string::npos);

    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    drain_ready(io);

    CHECK_FALSE(created->session->is_busy());
    CHECK(client_pointer->stop_callback_count == 1);
    auto screen = visible_screen(terminal);
    CHECK(count_text(screen, "partial assistant output") == 1);
    CHECK(count_text(screen, "prompt aborted") == 1);

    REQUIRE(terminal.inject_input("\x03recover\r"));
    drain_ready(io);
    CHECK_FALSE(client_pointer->recovery_stop_requested);
    CHECK(client_pointer->recovery_uses_fresh_token);
    CHECK(visible_screen(terminal).find("recovered after TUI abort") !=
        std::string::npos);

    REQUIRE(terminal.inject_input("draft"));
    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    drain_ready(io);
    CHECK(visible_screen(terminal).find("draft") != std::string::npos);
    CHECK(created->session->message_count() == 4);

    REQUIRE(terminal.inject_input("\x03\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
    CHECK_FALSE(terminal.modes().started);
}

TEST_CASE(
    "Native TUI repeated interrupt and shutdown input restores the terminal once",
    "[coding_agent][tui][abort][shutdown][issue61]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    config.write(
        "keybindings.json",
        R"({"app.interrupt":"f7","app.exit":"f6"})");
    auto client = std::make_shared<AbortAwareInteractiveChatProvider>();
    auto* client_pointer = client.get();
    auto created = coding_agent::create_agent_session(session_options(
        workspace,
        std::move(client)));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 72, .rows = 12});
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

    REQUIRE(terminal.inject_input("stop and close\r"));
    drain_ready(io);
    REQUIRE(client_pointer->started);
    REQUIRE(terminal.inject_input("\x1b[18~\x1b[18~\x1b[17~\x1b[17~"));
    drain_ready(io);

    REQUIRE(run_result);
    CHECK(*run_result);
    CHECK(client_pointer->stop_callback_count == 1);
    CHECK(count_text(visible_screen(terminal), "prompt aborted") == 1);
    CHECK_FALSE(created->session->is_open());
    CHECK_FALSE(created->session->is_busy());
    CHECK_FALSE(terminal.modes().started);
    CHECK(terminal.modes().cursor_visible);
}

TEST_CASE(
    "Native TUI waits for cancelled tool quiescence before accepting the next prompt",
    "[coding_agent][tui][abort][tools][issue61]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    config.write("keybindings.json", R"({"app.interrupt":"f7"})");
    auto client = std::make_shared<AbortThroughToolChatProvider>();
    auto* client_pointer = client.get();
    auto options = session_options(workspace, std::move(client));
    auto tool = std::make_unique<DelayedCancellationTool>();
    auto* tool_pointer = tool.get();
    options.custom_tools.push_back(std::move(tool));
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 72, .rows = 18});
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

    REQUIRE(terminal.inject_input("use delayed tool\r"));
    drain_ready(io);
    REQUIRE(tool_pointer->started);
    REQUIRE(tool_pointer->observed_stop_token.has_value());
    REQUIRE(client_pointer->first_stop_token.has_value());
    CHECK(*tool_pointer->observed_stop_token == *client_pointer->first_stop_token);
    CHECK(visible_screen(terminal).find("partial tool output before abort") !=
        std::string::npos);

    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    drain_ready(io);
    CHECK(created->session->is_busy());
    CHECK(tool_pointer->stop_callback_count == 0);

    REQUIRE(terminal.inject_input("/"));
    drain_ready(io);
    CHECK(visible_screen(terminal).find("/clear") != std::string::npos);
    REQUIRE(terminal.inject_input("\x1b[18~\x1b[18~"));
    drain_ready(io);
    CHECK(created->session->is_busy());
    CHECK(tool_pointer->stop_callback_count == 1);
    CHECK(tool_pointer->observed_stop_token->stop_requested());

    REQUIRE(terminal.inject_input("\x03recover after tool abort\r"));
    drain_ready(io);
    CHECK(created->session->message_count() == 2);
    CHECK(visible_screen(terminal).find("A prompt is already in flight") !=
        std::string::npos);

    tool_pointer->release();
    drain_ready(io);
    CHECK_FALSE(created->session->is_busy());
    auto screen = visible_screen(terminal);
    CHECK(count_text(screen, "tool prompt aborted") == 1);
    // The cancelled tool's failure renders inside its tool-execution block.
    CHECK(count_text(screen, "delayed tool aborted") == 1);
    REQUIRE(client_pointer->completion_stop_token.has_value());
    CHECK(*client_pointer->completion_stop_token == *client_pointer->first_stop_token);

    REQUIRE(terminal.inject_input("\r"));
    drain_ready(io);
    CHECK(created->session->message_count() == 6);
    CHECK(visible_screen(terminal).find("recovered after tool abort") !=
        std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI distinguishes subscriber diagnostics and remains usable",
    "[coding_agent][tui][diagnostics][issue61]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto created = coding_agent::create_agent_session(session_options(
        workspace,
        ai::providers::make_scripted_fake_provider()));
    REQUIRE(created);
    std::vector<coding_agent::EventSubscription> failing_subscriptions;
    for (std::size_t index = 0; index < 16; ++index) {
        auto subscription = created->session->subscribe(
            [index](const agent::AgentLifecycleEvent&) -> util::ExpectedVoid {
                return std::unexpected(util::make_error(
                    util::ErrorCode::Unknown,
                    "subscriber failed",
                    std::format(
                        "api_key=sk-subscriber-secret-123456 initial subscriber {}",
                        index)));
            });
        REQUIRE(subscription);
        failing_subscriptions.push_back(std::move(*subscription));
    }

    tui::VirtualTerminal terminal({.columns = 100, .rows = 40});
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

    REQUIRE(terminal.inject_input("survive subscriber failure\r"));
    drain_ready(io);
    auto screen = visible_screen(terminal);
    CHECK(screen.find("fake: survive subscriber failure") != std::string::npos);
    CHECK(screen.find("agent event observer failed") != std::string::npos);
    CHECK(screen.find("sk-subscriber-secret-123456") == std::string::npos);
    CHECK(screen.find("[REDACTED]") != std::string::npos);

    auto rollover_subscription = created->session->subscribe(
        [](const agent::AgentLifecycleEvent&) -> util::ExpectedVoid {
            return std::unexpected(util::make_error(
                util::ErrorCode::Unknown,
                "late subscriber rollover"));
        });
    REQUIRE(rollover_subscription);
    REQUIRE(terminal.inject_input("recover subscriber\r"));
    drain_ready(io);
    CHECK(created->session->message_count() == 4);
    CHECK(visible_screen(terminal).find("late subscriber rollover") !=
        std::string::npos);
    CHECK(visible_screen(terminal).find("fake: recover subscriber") !=
        std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI distinguishes persistence failures and remains usable",
    "[coding_agent][tui][diagnostics][persistence][issue61]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    const auto session_file = workspace.path() / "persistence-recovery.jsonl";
    auto options = session_options(
        workspace,
        ai::providers::make_scripted_fake_provider());
    options.session_target = coding_agent::ExplicitOpenOrCreateSessionTarget{session_file};
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 100, .rows = 18});
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

    harness::session::testing::fail_nth_append_for_test(session_file, 2);
    REQUIRE(terminal.inject_input("fail persistence\r"));
    drain_ready(io);
    auto screen = visible_screen(terminal);
    CHECK(screen.find("fake: fail persistence") != std::string::npos);
    CHECK(screen.find("could not persist session entry") != std::string::npos);
    CHECK(screen.find("fail persistence") != std::string::npos);

    REQUIRE(terminal.inject_input("\x03recover persistence\r"));
    drain_ready(io);
    CHECK(created->session->message_count() == 4);
    CHECK(visible_screen(terminal).find("fake: recover persistence") !=
        std::string::npos);

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
        std::make_shared<AcceptedOutcomeChatProvider>()));
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
    CHECK(count_text(screen, "Error: accepted provider failure") == 1);

    REQUIRE(terminal.inject_input("provider abort\r"));
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(count_text(screen, "Error: accepted provider failure") == 1);
    CHECK(count_text(screen, "Operation aborted") == 1);

    REQUIRE(terminal.inject_input("provider tool error\r"));
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(count_text(screen, "accepted tool-call provider failure") == 1);
    // Provider outcomes suppress the assistant notice when tool calls render
    // separately (pi assistant-message.ts); the settled tool block shows the
    // failure detail.
    CHECK(count_text(screen, "Error: accepted tool-call provider failure") == 0);
    CHECK(screen.find("read never.txt") != std::string::npos);

    REQUIRE(terminal.inject_input("provider tool abort\r"));
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(count_text(screen, "custom provider abort detail") == 1);
    CHECK(count_text(screen, "Error: custom provider abort detail") == 0);
    CHECK(screen.find("read never.txt") != std::string::npos);

    REQUIRE(terminal.inject_input("recover\r"));
    drain_ready(io);
    CHECK(created->session->message_count() == 10);
    CHECK(visible_screen(terminal).find("recovered after accepted outcomes") !=
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
        ai::providers::make_scripted_fake_provider()));
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
        ai::providers::make_scripted_fake_provider()));
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
        ai::providers::make_scripted_fake_provider()));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 60, .rows = 19});
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
    // The user box and the assistant reply both carry the prompt text.
    CHECK(count_text(screen, "first prompt") == 2);
    CHECK(count_text(screen, "fake: first prompt") == 1);

    REQUIRE(terminal.inject_input("second prompt\r"));
    drain_ready(io);
    CHECK(created->session->message_count() == 4);
    screen = visible_screen(terminal);
    CHECK(count_text(screen, "fake: first prompt") == 1);
    CHECK(count_text(screen, "fake: second prompt") == 1);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
    CHECK_FALSE(terminal.modes().started);
    CHECK(terminal.modes().cursor_visible);
}

TEST_CASE(
    "Native TUI steers and follows up in pi-compatible turn order",
    "[coding_agent][tui][queues][issue62]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto client = std::make_shared<TurnGatedChatProvider>();
    auto* client_pointer = client.get();
    auto options = session_options(workspace, std::move(client));
    options.max_queued_messages = 3;
    options.max_queued_bytes = 64;
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 72, .rows = 18});
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

    REQUIRE(terminal.inject_input("initial\x1b[13;3u"));
    drain_ready(io);
    CHECK(client_pointer->request_count == 1);

    REQUIRE(terminal.inject_input("steer one\rsteer two\r"));
    REQUIRE(terminal.inject_input("follow one\x1b[13;3u"));
    drain_ready(io);

    auto snapshot = created->session->snapshot();
    REQUIRE(snapshot.agent_state.input_queues.steering.messages.size() == 2);
    REQUIRE(snapshot.agent_state.input_queues.follow_up.messages.size() == 1);
    auto screen = visible_screen(terminal);
    const auto steer_one = screen.find("Steering: steer one");
    const auto steer_two = screen.find("Steering: steer two");
    const auto follow_up = screen.find("Follow-up: follow one");
    REQUIRE(steer_one != std::string::npos);
    REQUIRE(steer_two != std::string::npos);
    REQUIRE(follow_up != std::string::npos);
    // pi updatePendingMessagesDisplay: steering messages render before
    // follow-up messages, then the dequeue hint.
    CHECK(steer_one < steer_two);
    CHECK(steer_two < follow_up);
    const auto dequeue_hint = screen.find("Alt+Up to edit all queued messages");
    REQUIRE(dequeue_hint != std::string::npos);
    CHECK(follow_up < dequeue_hint);

    client_pointer->release();
    drain_ready(io);
    CHECK(client_pointer->request_count == 2);
    REQUIRE(client_pointer->observed_users.size() == 2);
    CHECK((client_pointer->observed_users[1] ==
        std::vector<std::string>{"initial", "steer one"}));

    client_pointer->release();
    drain_ready(io);
    CHECK(client_pointer->request_count == 3);
    REQUIRE(client_pointer->observed_users.size() == 3);
    CHECK((client_pointer->observed_users[2] ==
        std::vector<std::string>{"initial", "steer one", "steer two"}));

    client_pointer->release();
    drain_ready(io);
    CHECK(client_pointer->request_count == 4);
    REQUIRE(client_pointer->observed_users.size() == 4);
    CHECK((client_pointer->observed_users[3] ==
        std::vector<std::string>{"initial", "steer one", "steer two", "follow one"}));

    client_pointer->release();
    drain_ready(io);
    CHECK_FALSE(created->session->is_busy());
    CHECK(created->session->snapshot().agent_state.input_queues.steering.messages.empty());
    CHECK(created->session->snapshot().agent_state.input_queues.follow_up.messages.empty());

    REQUIRE(terminal.inject_input("continued\r"));
    drain_ready(io);
    CHECK(client_pointer->request_count == 5);
    client_pointer->release();
    drain_ready(io);
    CHECK_FALSE(created->session->is_busy());

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI preserves bounded rejected input and dequeues pending text",
    "[coding_agent][tui][queues][limits][issue62]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto client = std::make_shared<TurnGatedChatProvider>();
    auto* client_pointer = client.get();
    auto options = session_options(workspace, std::move(client));
    options.max_queued_messages = 2;
    options.max_queued_bytes = 4;
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 80, .rows = 24});
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

    REQUIRE(terminal.inject_input("initial\r"));
    drain_ready(io);
    CHECK(client_pointer->request_count == 1);

    REQUIRE(terminal.inject_input("line one\x1b[13;2uline two"));
    drain_ready(io);
    auto screen = visible_screen(terminal);
    CHECK(screen.find("line one") != std::string::npos);
    CHECK(screen.find("line two") != std::string::npos);
    CHECK(created->session->snapshot().agent_state.input_queues.steering.messages.empty());
    REQUIRE(terminal.inject_input("\x03"));
    drain_ready(io);

    REQUIRE(terminal.inject_input("é\ré\rx\r"));
    drain_ready(io);
    auto snapshot = created->session->snapshot();
    CHECK(snapshot.agent_state.input_queues.max_messages == 2);
    CHECK(snapshot.agent_state.input_queues.max_bytes == 4);
    REQUIRE(snapshot.agent_state.input_queues.steering.messages.size() == 2);
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(
        snapshot.agent_state.input_queues.steering.messages[0])) == "é");
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(
        snapshot.agent_state.input_queues.steering.messages[1])) == "é");
    screen = visible_screen(terminal);
    CHECK(screen.find("Unable to queue steering input: too many queued messages") !=
        std::string::npos);
    CHECK(screen.find("Steering: é") != std::string::npos);
    CHECK(screen.find("x") != std::string::npos);

    REQUIRE(terminal.inject_input("\r"));
    drain_ready(io);
    snapshot = created->session->snapshot();
    REQUIRE(snapshot.agent_state.input_queues.steering.messages.size() == 2);
    CHECK(visible_screen(terminal).find("Unable to queue steering input") !=
        std::string::npos);

    REQUIRE(terminal.inject_input("\x1b[1;3A"));
    drain_ready(io);
    snapshot = created->session->snapshot();
    CHECK(snapshot.agent_state.input_queues.steering.messages.empty());
    screen = visible_screen(terminal);
    const auto first = screen.find("é");
    const auto second = screen.find("é", first + 1);
    const auto draft = screen.find("x", second + 1);
    REQUIRE(first != std::string::npos);
    REQUIRE(second != std::string::npos);
    REQUIRE(draft != std::string::npos);
    CHECK(first < second);
    CHECK(second < draft);
    CHECK(screen.find("Restored 2 queued messages to editor") != std::string::npos);
    CHECK(screen.find("Steering: é") == std::string::npos);

    REQUIRE(terminal.inject_input("\x03éé\rx\r"));
    drain_ready(io);
    snapshot = created->session->snapshot();
    REQUIRE(snapshot.agent_state.input_queues.steering.messages.size() == 1);
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(
        snapshot.agent_state.input_queues.steering.messages[0])) == "éé");
    screen = visible_screen(terminal);
    CHECK(screen.find("Unable to queue steering input: queued messages too large") !=
        std::string::npos);
    CHECK(screen.find("x") != std::string::npos);

    REQUIRE(terminal.inject_input("\x1b[1;3A"));
    drain_ready(io);
    REQUIRE(terminal.inject_input("\x03ok\r"));
    drain_ready(io);
    snapshot = created->session->snapshot();
    REQUIRE(snapshot.agent_state.input_queues.steering.messages.size() == 1);
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(
        snapshot.agent_state.input_queues.steering.messages[0])) == "ok");

    client_pointer->release();
    drain_ready(io);
    CHECK(client_pointer->request_count == 2);
    client_pointer->release();
    drain_ready(io);
    CHECK_FALSE(created->session->is_busy());

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI preserves follow-up after an accepted error and remains usable",
    "[coding_agent][tui][queues][error][issue62]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto client = std::make_shared<TurnGatedChatProvider>();
    auto* client_pointer = client.get();
    client_pointer->first_request_errors = true;
    auto created = coding_agent::create_agent_session(session_options(
        workspace,
        std::move(client)));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 72, .rows = 16});
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

    REQUIRE(terminal.inject_input("initial\rfollow after error\x1b[13;3u"));
    drain_ready(io);
    CHECK(client_pointer->request_count == 1);
    REQUIRE(created->session->snapshot().agent_state.input_queues.follow_up.messages.size() == 1);

    client_pointer->release();
    drain_ready(io);
    CHECK(client_pointer->request_count == 1);
    CHECK_FALSE(created->session->is_busy());
    REQUIRE(created->session->snapshot().agent_state.input_queues.follow_up.messages.size() == 1);
    auto screen = visible_screen(terminal);
    CHECK(count_text(screen, "Error: accepted queued error") == 1);
    CHECK(screen.find("Follow-up: follow after error") != std::string::npos);

    REQUIRE(terminal.inject_input("\x1b[1;3A"));
    drain_ready(io);
    REQUIRE(terminal.inject_input("\r"));
    drain_ready(io);
    CHECK(client_pointer->request_count == 2);
    CHECK(created->session->snapshot().agent_state.input_queues.follow_up.messages.empty());
    client_pointer->release();
    drain_ready(io);
    CHECK_FALSE(created->session->is_busy());
    CHECK(visible_screen(terminal).find("turn 2") != std::string::npos);

    REQUIRE(terminal.inject_input("continued\r"));
    drain_ready(io);
    CHECK(client_pointer->request_count == 3);
    client_pointer->release();
    drain_ready(io);
    CHECK_FALSE(created->session->is_busy());

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI restores pending input before abort and accepts it after quiescence",
    "[coding_agent][tui][queues][abort][issue62]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto client = std::make_shared<AbortAwareInteractiveChatProvider>();
    auto* client_pointer = client.get();
    auto created = coding_agent::create_agent_session(session_options(
        workspace,
        std::move(client)));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 72, .rows = 18});
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

    REQUIRE(terminal.inject_input("abort queues\rsteer later\rfollow later\x1b[13;3udraft"));
    drain_ready(io);
    REQUIRE(client_pointer->started);
    auto snapshot = created->session->snapshot();
    REQUIRE(snapshot.agent_state.input_queues.steering.messages.size() == 1);
    REQUIRE(snapshot.agent_state.input_queues.follow_up.messages.size() == 1);

    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    drain_ready(io);
    CHECK_FALSE(created->session->is_busy());
    snapshot = created->session->snapshot();
    CHECK(snapshot.agent_state.input_queues.steering.messages.empty());
    CHECK(snapshot.agent_state.input_queues.follow_up.messages.empty());
    auto screen = visible_screen(terminal);
    CHECK(screen.find("Steering: steer later") == std::string::npos);
    CHECK(screen.find("Follow-up: follow later") == std::string::npos);
    const auto steering = screen.find("steer later");
    const auto follow_up = screen.find("follow later", steering + 1);
    const auto draft = screen.find("draft", follow_up + 1);
    REQUIRE(steering != std::string::npos);
    REQUIRE(follow_up != std::string::npos);
    REQUIRE(draft != std::string::npos);
    CHECK(steering < follow_up);
    CHECK(follow_up < draft);
    CHECK(count_text(screen, "prompt aborted") == 1);

    REQUIRE(terminal.inject_input("\x03recover\r"));
    drain_ready(io);
    CHECK_FALSE(created->session->is_busy());
    CHECK(visible_screen(terminal).find("recovered after TUI abort") !=
        std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI records accepted follow-up in editor history and queues slash text while a run is active",
    "[coding_agent][tui][queues][issue401]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto client = std::make_shared<TurnGatedChatProvider>();
    auto* client_pointer = client.get();
    auto created = coding_agent::create_agent_session(session_options(
        workspace,
        std::move(client)));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 72, .rows = 18});
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

    REQUIRE(terminal.inject_input("initial\r"));
    drain_ready(io);
    CHECK(client_pointer->request_count == 1);

    // Alt+Enter admits follow-up input; the accepted text leaves the editor
    // and is recorded in editor history (pi handleFollowUp addToHistory).
    REQUIRE(terminal.inject_input("follow one\x1b[13;3u"));
    drain_ready(io);
    auto snapshot = created->session->snapshot();
    REQUIRE(snapshot.agent_state.input_queues.follow_up.messages.size() == 1);
    auto screen = visible_screen(terminal);
    CHECK(screen.find("Follow-up: follow one") != std::string::npos);

    // Up in the empty editor recalls the accepted follow-up text (the
    // cursor sits at the start of the recalled line, like the Enter path).
    REQUIRE(terminal.inject_input("\x1b[A"));
    drain_ready(io);
    REQUIRE(terminal.inject_input("x"));
    drain_ready(io);
    CHECK(visible_screen(terminal).find("xfollow one") != std::string::npos);

    // pi handleFollowUp: while a run is active, Alt+Enter queues even slash
    // text directly as follow-up input — the command chain does not run.
    REQUIRE(terminal.inject_input("\x03"));
    drain_ready(io);
    REQUIRE(terminal.inject_input("/hotkeys\x1b[13;3u"));
    drain_ready(io);
    snapshot = created->session->snapshot();
    REQUIRE(snapshot.agent_state.input_queues.follow_up.messages.size() == 2);
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(
        snapshot.agent_state.input_queues.follow_up.messages[1])) == "/hotkeys");
    screen = visible_screen(terminal);
    CHECK(screen.find("Follow-up: /hotkeys") != std::string::npos);
    CHECK(screen.find("Hotkeys") == std::string::npos);

    client_pointer->release();
    drain_ready(io);
    CHECK(client_pointer->request_count == 2);
    REQUIRE(client_pointer->observed_users.size() == 2);
    CHECK((client_pointer->observed_users[1] ==
        std::vector<std::string>{"initial", "follow one"}));
    client_pointer->release();
    drain_ready(io);
    CHECK(client_pointer->request_count == 3);
    REQUIRE(client_pointer->observed_users.size() == 3);
    CHECK((client_pointer->observed_users[2] ==
        std::vector<std::string>{"initial", "follow one", "/hotkeys"}));
    client_pointer->release();
    drain_ready(io);
    CHECK_FALSE(created->session->is_busy());
    CHECK(created->session->snapshot().agent_state.input_queues.follow_up.messages.empty());

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI rejects follow-up admission at capacity and restores the text unchanged",
    "[coding_agent][tui][queues][limits][issue401]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto client = std::make_shared<TurnGatedChatProvider>();
    auto* client_pointer = client.get();
    auto options = session_options(workspace, std::move(client));
    options.max_queued_messages = 1;
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 80, .rows = 20});
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

    REQUIRE(terminal.inject_input("initial\r"));
    drain_ready(io);
    CHECK(client_pointer->request_count == 1);

    REQUIRE(terminal.inject_input("fu one\x1b[13;3u"));
    drain_ready(io);
    auto snapshot = created->session->snapshot();
    REQUIRE(snapshot.agent_state.input_queues.follow_up.messages.size() == 1);

    // Second follow-up admission is rejected at capacity: the submitted text
    // is restored unchanged to the editor, the diagnostic is bounded and
    // redacted, and the queue plus the active run stay untouched.
    REQUIRE(terminal.inject_input("fu two\x1b[13;3u"));
    drain_ready(io);
    snapshot = created->session->snapshot();
    REQUIRE(snapshot.agent_state.input_queues.follow_up.messages.size() == 1);
    auto screen = visible_screen(terminal);
    CHECK(screen.find("Unable to queue follow-up input: too many queued messages") !=
        std::string::npos);
    CHECK(screen.find("Follow-up: fu one") != std::string::npos);
    CHECK(screen.find("Follow-up: fu two") == std::string::npos);
    // The rejected text is back in the editor, unchanged.
    CHECK(screen.find("fu two") != std::string::npos);
    CHECK(client_pointer->request_count == 1);

    client_pointer->release();
    drain_ready(io);
    CHECK(client_pointer->request_count == 2);
    CHECK(created->session->snapshot().agent_state.input_queues.follow_up.messages.empty());
    client_pointer->release();
    drain_ready(io);
    CHECK_FALSE(created->session->is_busy());

    // The rejected text still occupies the editor; clear before exit.
    REQUIRE(terminal.inject_input("\x03"));
    drain_ready(io);
    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI dequeue restores steering, then follow-up, then draft with blank lines",
    "[coding_agent][tui][queues][issue401]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto client = std::make_shared<TurnGatedChatProvider>();
    auto* client_pointer = client.get();
    auto created = coding_agent::create_agent_session(session_options(
        workspace,
        std::move(client)));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 72, .rows = 18});
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

    REQUIRE(terminal.inject_input("initial\r"));
    drain_ready(io);
    CHECK(client_pointer->request_count == 1);

    // One steering message, one follow-up message, and an unsubmitted draft.
    REQUIRE(terminal.inject_input("s1\r"));
    REQUIRE(terminal.inject_input("f1\x1b[13;3u"));
    REQUIRE(terminal.inject_input("draft"));
    drain_ready(io);
    auto snapshot = created->session->snapshot();
    REQUIRE(snapshot.agent_state.input_queues.steering.messages.size() == 1);
    REQUIRE(snapshot.agent_state.input_queues.follow_up.messages.size() == 1);

    // Alt+Up (app.message.dequeue) restores steering, then follow-up, then
    // the draft, each separated by a blank line (pi restoreQueuedMessages-
    // ToEditor join semantics), and clears the Agent-owned queues.
    REQUIRE(terminal.inject_input("\x1b[1;3A"));
    drain_ready(io);
    snapshot = created->session->snapshot();
    CHECK(snapshot.agent_state.input_queues.steering.messages.empty());
    CHECK(snapshot.agent_state.input_queues.follow_up.messages.empty());
    auto screen = visible_screen(terminal);
    CHECK(screen.find("Restored 2 queued messages to editor") != std::string::npos);
    CHECK(screen.find("Steering: s1") == std::string::npos);
    CHECK(screen.find("Follow-up: f1") == std::string::npos);

    std::vector<std::string> lines;
    {
        std::stringstream stream(screen);
        std::string line;
        while (std::getline(stream, line)) lines.push_back(std::move(line));
    }
    const auto find_line = [&lines](std::string_view needle) {
        for (std::size_t index = 0; index < lines.size(); ++index) {
            if (lines[index].find(needle) != std::string::npos) return index;
        }
        return lines.size();
    };
    const auto steering_line = find_line("s1");
    const auto follow_up_line = find_line("f1");
    const auto draft_line = find_line("draft");
    REQUIRE(steering_line != lines.size());
    REQUIRE(follow_up_line != lines.size());
    REQUIRE(draft_line != lines.size());
    CHECK(steering_line < follow_up_line);
    CHECK(follow_up_line < draft_line);
    // pi joins queued messages and the current text with "\n\n".
    CHECK(follow_up_line == steering_line + 2);
    CHECK(draft_line == follow_up_line + 2);

    client_pointer->release();
    drain_ready(io);
    // The queues were emptied by the dequeue, so the gated run completes
    // without a follow-up request.
    CHECK(client_pointer->request_count == 1);
    CHECK_FALSE(created->session->is_busy());

    REQUIRE(terminal.inject_input("\x03"));
    drain_ready(io);
    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI expanded header hints assemble followUp and dequeue with pi's keys",
    "[coding_agent][tui][hints][issue401]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto created = coding_agent::create_agent_session(session_options(
        workspace,
        ai::providers::make_scripted_fake_provider()));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 90, .rows = 26});
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

    // app.tools.expand (ctrl+o) reveals the expanded startup instructions
    // over the assembled subset, including the two queue actions.
    REQUIRE(terminal.inject_input("\x0f"));
    drain_ready(io);
    auto screen = visible_screen(terminal);
    // pi keyHint: the header renders keyText (lowercase); the pending
    // display hint uses keyDisplayText (capitalized) — both assembled here.
    CHECK(screen.find("alt+enter to queue follow-up") != std::string::npos);
    CHECK(screen.find("alt+up to edit all queued messages") != std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI queues batched submissions across deferred prompt dispatch",
    "[coding_agent][tui][async][issue58][issue62]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto client = std::make_shared<GatedChatProvider>();
    auto* client_pointer = client.get();
    auto created = coding_agent::create_agent_session(session_options(
        workspace,
        std::move(client)));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 60, .rows = 19});
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
    CHECK(screen.find("first batched") != std::string::npos);
    CHECK(screen.find("Steering: second batched") != std::string::npos);
    REQUIRE(created->session->snapshot().agent_state.input_queues.steering.messages.size() == 1);

    client_pointer->release();
    drain_ready(io);
    CHECK(created->session->message_count() == 3);
    CHECK(created->session->snapshot().agent_state.input_queues.steering.messages.empty());
    client_pointer->release();
    drain_ready(io);
    CHECK(created->session->message_count() == 4);
    screen = visible_screen(terminal);
    CHECK(screen.find("first batched") != std::string::npos);
    CHECK(screen.find("second batched") != std::string::npos);
    CHECK(count_text(screen, "released") == 2);

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
    auto client = std::make_shared<IncrementalGatedChatProvider>();
    auto* client_pointer = client.get();
    auto created = coding_agent::create_agent_session(session_options(workspace, std::move(client)));
    REQUIRE(created);

    // The pi-shaped composition reserves fixed rows for the status, the
    // editor borders, and the footer; 12 rows keep the chat tail anchored so
    // the streaming message's head scrolls out.
    tui::VirtualTerminal terminal({.columns = 30, .rows = 12});
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
    auto client = std::make_shared<GatedChatProvider>();
    auto* client_pointer = client.get();
    auto created = coding_agent::create_agent_session(session_options(workspace, std::move(client)));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 40, .rows = 16});
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
    REQUIRE(terminal.inject_resize({.columns = 30, .rows = 12}));
    REQUIRE(terminal.inject_input("draft"));
    drain_ready(io);
    const tui::TerminalDimensions expected_dimensions{.columns = 30, .rows = 12};
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
        ai::providers::make_scripted_fake_provider()));
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
    CHECK(screen.find("fake: /missing") != std::string::npos);
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
        ".pi/skills/project-skill/SKILL.md",
        "---\n"
        "name: project-skill\n"
        "description: Project skill completion.\n"
        "---\n"
        "Project skill body.\n");
    workspace.write(
        ".pi/prompts/project-prompt.md",
        "---\n"
        "description: Project prompt completion.\n"
        "---\n"
        "Expanded project prompt: $ARGUMENTS\n");
    auto options = session_options(
        workspace,
        ai::providers::make_scripted_fake_provider());
    options.project_trust_override = true;
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 100, .rows = 18});
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
    for (std::size_t index = 0; index < 12; ++index) {
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
        ai::providers::make_scripted_fake_provider()));
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
    // The settings selector renders the #327 subset items plus the two
    // graduated render settings with pi's settings-selector labels
    // (settings-selector.ts), including the Theme submenu item.
    CHECK(screen.find("Output padding") != std::string::npos);
    CHECK(screen.find("Hide thinking") != std::string::npos);
    CHECK(screen.find("Thinking level") != std::string::npos);
    CHECK(screen.find("Default project trust") != std::string::npos);
    CHECK(screen.find("Theme") != std::string::npos);
    CHECK(created->session->message_count() == 0);

    // The Theme item opens the single-mode theme submenu with the `(current)`
    // marker on the active theme.
    REQUIRE(terminal.inject_input("\x1b[B\x1b[B\x1b[B\x1b[B\r"));
    drain_ready(io);
    screen = visible_screen(terminal);
    // The `(current)` marker renders as the styled description of the active
    // theme row, so ANSI may separate it from the theme name.
    CHECK(screen.find("(current)") != std::string::npos);
    CHECK(screen.find("dark") != std::string::npos);

    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    drain_ready(io);
    // Esc closes the theme submenu; a second Esc closes the settings selector.
    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    drain_ready(io);
    REQUIRE(terminal.inject_input("/hotkeys\r"));
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(screen.find("Hotkeys") != std::string::npos);
    CHECK(screen.find("f6") != std::string::npos);
    CHECK(screen.find("app.exit") != std::string::npos);
    // The assembled queue actions appear in help with pi's default keys.
    CHECK(screen.find("app.message.followUp") != std::string::npos);
    CHECK(screen.find("alt+enter") != std::string::npos);
    CHECK(screen.find("app.message.dequeue") != std::string::npos);
    CHECK(screen.find("alt+up") != std::string::npos);
    CHECK(screen.find("app.session.new") == std::string::npos);
    // app.suspend and app.editor.external assembled with P15's editor chrome.
    CHECK(screen.find("app.suspend") != std::string::npos);
    CHECK(screen.find("app.editor.external") != std::string::npos);
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
    "Native TUI retains bounded terminal provider failures and allows retry",
    "[coding_agent][tui][failure][issue58]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto created = coding_agent::create_agent_session(session_options(
        workspace,
        std::make_shared<FailOnceChatProvider>()));
    REQUIRE(created);

    tui::VirtualTerminal terminal({.columns = 100, .rows = 20});
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
    CHECK(screen.find("Error: provider failed") != std::string::npos);
    CHECK(screen.size() < 12000);
    CHECK(created->session->message_count() == 2);

    REQUIRE(terminal.inject_input("retry\r"));
    drain_ready(io);
    CHECK(created->session->message_count() == 4);
    CHECK(visible_screen(terminal).find("recovered") != std::string::npos);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
    CHECK_FALSE(terminal.modes().started);
}

TEST_CASE(
    "Native TUI app.thinking.toggle persists hideThinkingBlock and reports pi status",
    "[coding_agent][tui][render-settings][issue408]") {
    RichThinkingSession fixture;
    fixture.create();
    tests::TempWorkspace config;
    config.write(
        "keybindings.json",
        R"({"app.thinking.toggle":"f8","app.exit":"f6"})");
    auto resumed = fixture.resume();

    tui::VirtualTerminal terminal({.columns = 72, .rows = 30});
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

    // The thinking block renders at boot (default hideThinkingBlock false).
    CHECK(visible_screen(terminal).find("inspect the saved state") != std::string::npos);

    // pi `toggleThinkingBlockVisibility`: toggle, persist, rebuild, status.
    REQUIRE(terminal.inject_input("\x1b[19~"));
    drain_ready(io);
    auto screen = visible_screen(terminal);
    CHECK(screen.find("inspect the saved state") == std::string::npos);
    CHECK(screen.find("Thinking...") != std::string::npos);
    CHECK(screen.find("Thinking blocks: hidden") != std::string::npos);
    const auto settings_path = config.path() / "settings.json";
    REQUIRE(std::filesystem::exists(settings_path));
    CHECK(read_binary_file(settings_path).find("\"hideThinkingBlock\": true") !=
        std::string::npos);

    REQUIRE(terminal.inject_input("\x1b[19~"));
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(screen.find("inspect the saved state") != std::string::npos);
    CHECK(screen.find("Thinking blocks: visible") != std::string::npos);
    CHECK(read_binary_file(settings_path).find("\"hideThinkingBlock\": false") !=
        std::string::npos);

    REQUIRE(terminal.inject_input("\x1b[17~"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI boots with the persisted hideThinkingBlock and outputPad render settings",
    "[coding_agent][tui][render-settings][issue408]") {
    RichThinkingSession fixture;
    fixture.create();
    tests::TempWorkspace config;
    config.write(
        "settings.json",
        R"({"hideThinkingBlock": true, "outputPad": 0})");
    auto resumed = fixture.resume();

    tui::VirtualTerminal terminal({.columns = 72, .rows = 30});
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
    // hideThinkingBlock true hides the thinking block behind the pi label.
    CHECK(screen.find("inspect the saved state") == std::string::npos);
    CHECK(screen.find("Thinking...") != std::string::npos);
    // outputPad 0 renders the user message at column 0 (the VirtualTerminal
    // screen drops the OSC 133 zone prefix; the default padding 1 would shift
    // the text one column right).
    CHECK(column_of(screen, "resume request") == 0);

    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
}

TEST_CASE(
    "Native TUI settings selector changes render settings live and they survive persistence",
    "[coding_agent][tui][settings-selector][issue408]") {
    RichThinkingSession fixture;
    fixture.create();
    tests::TempWorkspace config;
    config.write("keybindings.json", R"({"app.exit":"f6"})");
    auto resumed = fixture.resume();

    tui::VirtualTerminal terminal({.columns = 72, .rows = 30});
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

    const auto settings_path = config.path() / "settings.json";
    CHECK(visible_screen(terminal).find("inspect the saved state") != std::string::npos);

    // /settings opens the selector; the first item is output-padding with the
    // default value 1, so the user message starts one column right of the
    // OSC zone prefix.
    REQUIRE(terminal.inject_input("/settings\r"));
    drain_ready(io);
    auto screen = visible_screen(terminal);
    CHECK(screen.find("Output padding") != std::string::npos);
    CHECK(screen.find("Hide thinking") != std::string::npos);
    // The default outputPad 1 renders the user message one column right of
    // the zero-padding position.
    CHECK(column_of(screen, "resume request") == 1);

    // Down to hide-thinking and confirm: the thinking block disappears live
    // and the global setting persists.
    REQUIRE(terminal.inject_input("\x1b[B\r"));
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(screen.find("inspect the saved state") == std::string::npos);
    CHECK(screen.find("Thinking...") != std::string::npos);
    REQUIRE(std::filesystem::exists(settings_path));
    CHECK(read_binary_file(settings_path).find("\"hideThinkingBlock\": true") !=
        std::string::npos);

    // Up to output-padding and confirm: the padding cycles 1 → 0, the user
    // message re-renders one column left, and the setting persists.
    REQUIRE(terminal.inject_input("\x1b[A\r"));
    drain_ready(io);
    screen = visible_screen(terminal);
    CHECK(column_of(screen, "resume request") == 0);
    CHECK(read_binary_file(settings_path).find("\"outputPad\": 0") != std::string::npos);

    // Esc closes the selector; exit.
    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    drain_ready(io);
    REQUIRE(terminal.inject_input("\x1b[17~"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);

    // A fresh boot with the same config directory applies the persisted
    // render settings (hideThinkingBlock true, outputPad 0).
    auto rebooted = fixture.resume();
    tui::VirtualTerminal reboot_terminal({.columns = 72, .rows = 30});
    boost::asio::io_context reboot_io;
    std::optional<util::ExpectedVoid> reboot_result;
    boost::asio::co_spawn(
        reboot_io,
        coding_agent::tui::run_interactive_mode(
            *rebooted->session,
            reboot_terminal,
            {.agent_config_directory = config.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            CHECK(exception == nullptr);
            reboot_result.emplace(std::move(result));
        });
    drain_ready(reboot_io);
    const auto reboot_screen = visible_screen(reboot_terminal);
    CHECK(reboot_screen.find("inspect the saved state") == std::string::npos);
    CHECK(reboot_screen.find("Thinking...") != std::string::npos);
    CHECK(column_of(reboot_screen, "resume request") == 0);
    REQUIRE(reboot_terminal.inject_input("\x1b[17~"));
    drain_ready(reboot_io);
    REQUIRE(reboot_result);
    CHECK(*reboot_result);
}
