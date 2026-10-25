// The product selection reset at the transcript rebuild (B63 / TPadding, #1121).
//
// pi `renderSessionEntries` (`interactive-mode.ts:4105-4112`) drops the text
// selection *and* the multi-click history before the transcript it points
// into is replaced:
//   if (this.renderer instanceof TuiAltScreen) this.renderer.resetTextSelection();
// pi's `TuiAltScreen.resetTextSelection` (`tui-alt-screen.ts:317-320`) is
// `clearTextSelection()` plus `this.lastClick = undefined`, so the next press
// starts a character click again instead of continuing the word/line cycle.
//
// The product seam is `InteractiveEngine::rebuild_chat()`
// (`InteractiveEngine.cpp:1296`), the C++ counterpart of pi's
// `rebuildChatFromMessages` -> `renderSessionEntries`: it replaces the
// transcript from the authoritative session snapshot, so the selection
// coordinates point into replaced rows. The product entry that reaches it
// here is the settings `Output padding` toggle (pi `onOutputPadChange`,
// `interactive-mode.ts:5097-5105`), the same pi callback the ticket names.
//
// The DISCRIMINATING observation is the multi-click history: a
// highlight-only reset leaves the history intact, so the next double click
// still selects the whole word instead of the character under the pointer.

#include "coding_agent/tui/AltScreenHost.hpp"
#include "coding_agent/tui/InteractiveEngine.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "TestTuiActionSink.hpp"
#include "support/ModelsFixture.hpp"
#include "support/PumpUntil.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/RuntimeLoopDriver.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/coding_agent/session/SessionStore.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace cch;
using tests::drain_ready;

namespace {

/// The engine's own run sequence, with the fullscreen host registered before
/// `start` so the engine can reach the AltScreen it resets.
[[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> run_engine_with_host(
        std::shared_ptr<coding_agent::tui::InteractiveEngine> engine, coding_agent::tui::InteractiveSessionRun run) {
    if (auto started = engine->start(std::move(run)); !started) co_return std::unexpected(started.error());
    if (auto warning = co_await engine->append_project_trust_warning_if_needed(); !warning) {
        if (warning.error().code == support::ErrorCode::Cancelled) co_return co_await engine->finish();
        auto finished = co_await engine->finish();
        if (!finished) co_return std::unexpected(finished.error());
        co_return std::unexpected(warning.error());
    }
    boost::system::error_code wait_error;
    if (!engine->exit_requested()) {
        co_await engine->exit_wait().async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
    }
    co_return co_await engine->finish();
}

/// The transcript row the selection is made on. The word "alpha" is followed
/// by a space, so a character selection and a word selection differ.
constexpr std::string_view kRowText{"alpha beta gamma"};

[[nodiscard]] tests::ModelsSessionOptions session_options(
        const tests::TempWorkspace& workspace, std::shared_ptr<tests::ScriptedProvider> client) {
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
    return options;
}

[[nodiscard]] support::AsyncResult<coding_agent::CreateAgentSessionResult> create_session_async(
        tests::RuntimeFixture& runtime, tests::ModelsSessionOptions options) {
    auto models = std::move(options.models);
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    request.execution_runtime_target = runtime.make_target();
    return coding_agent::create_agent_session_async(
            std::move(request), std::nullopt, cch::tests::cli_fake_overrides(std::move(models)));
}

/// The visible screen of the regular-mode terminal.
[[nodiscard]] std::string visible_screen(const tui::VirtualTerminal& terminal) {
    std::string text;
    for (const auto& line : terminal.screen()) {
        text.append(line);
        text.push_back('\n');
    }
    return text;
}

} // namespace

TEST_CASE("Native TUI transcript rebuild drops the selection and the multi-click history",
        "[coding_agent][tui][fullscreen][selection][issue1121][spec]") {
    tests::RuntimeFixture runtime;
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    auto created = runtime.run(
            create_session_async(runtime, session_options(workspace, tests::make_scripted_fake_provider())));
    REQUIRE(created);

    tests::RuntimeLoopDriver runtime_driver(runtime);
    coding_agent::tui::testing::ActionSinkRecorder recorder;
    auto host_sink = recorder.make_sink();
    tui::VirtualTerminal terminal({.columns = 100, .rows = 30});
    tui::VirtualTerminal host_terminal({.columns = 40, .rows = 4});
    coding_agent::tui::AltScreenHost host(host_terminal, host_sink, 7);
    // A deterministic click clock, so the multi-click history is driven by
    // the injected presses alone.
    std::chrono::milliseconds now{0};
    host.alt_screen().set_selection_click_clock([&now] { return now; });
    host.alt_screen().set_copy_on_select(false);
    REQUIRE(host.alt_screen().add_child(std::make_unique<tui::Text>(std::string{kRowText}, 0, 0)));
    REQUIRE(host.alt_screen().start());
    REQUIRE(host.alt_screen().render());

    boost::asio::io_context io;
    auto engine = std::make_shared<coding_agent::tui::InteractiveEngine>(terminal, io.get_executor());
    engine->set_alt_screen_host(&host);
    std::optional<support::ExpectedVoid> run_result;
    boost::asio::co_spawn(io,
            run_engine_with_host(engine,
                    coding_agent::tui::InteractiveSessionRunBuilder{}
                            .with_session(*created->session)
                            .with_agent_config_directory(config.path())
                            .with_action_sink(recorder.make_sink())
                            .build()),
            [&](std::exception_ptr exception, support::ExpectedVoid result) {
                CHECK(exception == nullptr);
                run_result.emplace(std::move(result));
            });
    drain_ready(io);

    // A double click on the row selects the word under the pointer (x=3 is
    // inside "alpha") and records the click in the multi-click history. The
    // history now holds the SECOND consecutive press.
    REQUIRE(host_terminal.inject_input("\x1b[<0;1;3M"));
    REQUIRE(host_terminal.inject_input("\x1b[<0;1;3m"));
    now += std::chrono::milliseconds(10);
    REQUIRE(host_terminal.inject_input("\x1b[<0;1;3M"));
    REQUIRE(host_terminal.inject_input("\x1b[<0;1;3m"));
    REQUIRE(host.alt_screen().has_active_selection());
    // The CONTROL: the word is selected, so the click history is demonstrably
    // live at its second press.
    REQUIRE(host.copy_active_selection_to_clipboard());
    REQUIRE(recorder.write_clipboard.size() == 1);
    CHECK(recorder.write_clipboard.front().text == "alpha");
    recorder.write_clipboard.clear();

    // The product moment: the settings `Output padding` toggle rebuilds the
    // transcript (pi `onOutputPadChange` -> `rebuildChatFromMessages` ->
    // `renderSessionEntries`), which is the C++ `rebuild_chat()`.
    REQUIRE(terminal.inject_input("/settings\r"));
    drain_ready(io);
    REQUIRE(visible_screen(terminal).find("Output padding") != std::string::npos);
    // The settings list's search row filters the rows; "output" leaves the
    // single `Output padding` row selected.
    REQUIRE(terminal.inject_input("output"));
    drain_ready(io);
    REQUIRE(terminal.inject_input("\r"));
    drain_ready(io);

    // The selection is gone. This assertion is independent of the
    // multi-click history: it is the visible highlight.
    CHECK_FALSE(host.alt_screen().has_active_selection());

    // The DISCRIMINATING observation: the multi-click history is gone too.
    // The next press is the FIRST of a fresh cycle, so it is a character
    // click: a zero-width selection, nothing to copy. With a retained click
    // history the same press would be the THIRD consecutive one and would
    // select the whole LINE.
    now += std::chrono::milliseconds(10);
    REQUIRE(host_terminal.inject_input("\x1b[<0;1;3M"));
    REQUIRE(host_terminal.inject_input("\x1b[<0;1;3m"));
    CHECK_FALSE(host.alt_screen().has_active_selection());
    const auto copied_after_reset = host.copy_active_selection_to_clipboard();
    REQUIRE(copied_after_reset);
    CHECK_FALSE(*copied_after_reset);
    CHECK(recorder.write_clipboard.empty());

    // The cycle restarts: the second press is a word click again.
    now += std::chrono::milliseconds(10);
    REQUIRE(host_terminal.inject_input("\x1b[<0;1;3M"));
    REQUIRE(host_terminal.inject_input("\x1b[<0;1;3m"));
    REQUIRE(host.alt_screen().has_active_selection());
    // The copied bytes are the selection: the word "alpha", not the whole
    // line a retained click history would select.
    REQUIRE(host.copy_active_selection_to_clipboard());
    REQUIRE(recorder.write_clipboard.size() == 1);
    CHECK(recorder.write_clipboard.front().text == "alpha");

    REQUIRE(terminal.inject_input("\x03"));
    drain_ready(io);
    REQUIRE(terminal.inject_input("\x04"));
    drain_ready(io);
    REQUIRE(run_result);
    CHECK(*run_result);
    REQUIRE(host.alt_screen().stop());
}
