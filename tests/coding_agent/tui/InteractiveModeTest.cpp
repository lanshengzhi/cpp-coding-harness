#include "coding_agent/tui/InteractiveMode.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/ChatClient.hpp>
#include <cch/ai/Content.hpp>
#include <cch/coding_agent/Sdk.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "ai/providers/FakeChatClient.hpp"
#include "coding_agent/BoundedText.hpp"

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
