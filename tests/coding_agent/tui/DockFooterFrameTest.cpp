#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"

#include "support/EnvVarGuard.hpp"
#include "support/ModelFixture.hpp"
#include "support/ModelsFixture.hpp"
#include "support/PumpUntil.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/RuntimeLoopDriver.hpp"
#include "support/ScriptedRuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/Content.hpp>
#include <cch/support/Error.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace cch;
using tests::drain_ready;

namespace {

[[nodiscard]] std::vector<ai::Model> probe_catalog() {
    auto model = tests::make_model("fake-model", "fake", "fake");
    model.name = "Fake Model";
    model.input = {ai::ModelInput::Text};
    model.context_window = 128000;
    model.max_tokens = 16384;
    return {std::move(model)};
}

[[nodiscard]] std::unique_ptr<coding_agent::AgentSession> make_probe_session(tests::RuntimeFixture& runtime,
        tests::ScriptedRuntimeFixture& scripted,
        const std::filesystem::path& workspace) {
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_target = coding_agent::InMemorySessionTarget{};
    request.workspace = workspace;
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.session_facts.provider = "fake";
    request.session_facts.model = "fake-model";
    request.execution_runtime_target = runtime.make_target();
    request.model_runtime = scripted.runtime;
    coding_agent::isolate_magpie_gateway(request);
    auto created = runtime.run(coding_agent::create_agent_session_async(std::move(request), std::nullopt, {}));
    REQUIRE(created.has_value());
    return std::move(created->session);
}

[[nodiscard]] std::vector<std::string> composed_rows(const tui::VirtualTerminal& terminal) {
    std::vector<std::string> rows;
    for (const auto& line : terminal.screen()) {
        auto trimmed = std::string{line};
        while (!trimmed.empty() && trimmed.back() == ' ')
            trimmed.pop_back();
        if (!trimmed.empty()) rows.push_back(std::move(trimmed));
    }
    return rows;
}

/// The footer owns the last two composed rows: the workspace path row directly
/// above the context/model stats row. Called after every input injection, so a
/// frame that drops a footer row fails even when a later frame restores it.
void assert_footer_rows(const tui::VirtualTerminal& terminal, std::string_view label) {
    const auto rows = composed_rows(terminal);
    INFO("frame: " << label);
    INFO("composed rows: " << [&] {
        std::string joined;
        for (const auto& row : rows)
            joined += std::format("\n|{}|", row);
        return joined;
    }());
    REQUIRE(rows.size() >= 2);
    const auto& path_row = rows[rows.size() - 2];
    const auto& stats_row = rows[rows.size() - 1];
    CHECK(stats_row.find("/128k (auto)") != std::string::npos);
    CHECK(path_row.find("cpp-harness") != std::string::npos);
    CHECK(path_row.find("(auto)") == std::string::npos);
}

/// One run over a 50x30 terminal, in the requested mode, with the footer
/// asserted after every injection.
struct ProbeRun {
    tests::TempWorkspace home;
    tests::TempWorkspace config;
    tests::TempWorkspace workspace;
    std::optional<tests::EnvVarGuard> home_guard;
    std::optional<tests::EnvVarGuard> xdg_guard;
    tests::ScriptedRuntimeFixture scripted{probe_catalog()};
    tests::RuntimeFixture runtime;
    std::unique_ptr<coding_agent::AgentSession> session;
    tui::VirtualTerminal terminal{{.columns = 50,
            .rows = 30,
            .capabilities = {.synchronized_output = true, .color = tui::TerminalColorCapability::TrueColor}}};
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    std::optional<tests::RuntimeLoopDriver> driver;
    coding_agent::TuiMode mode{coding_agent::TuiMode::Regular};

    explicit ProbeRun(coding_agent::TuiMode requested, std::size_t responses) : mode(requested) {
        home_guard.emplace("HOME", home.path().string());
        xdg_guard.emplace("XDG_CONFIG_HOME", std::nullopt);
        std::filesystem::create_directories(config.path());
        for (std::size_t index = 0; index < responses; ++index)
            scripted.control->responses.push_back(ai::assistant_text_message(std::format("probe reply {}", index)));
        session = make_probe_session(runtime, scripted, workspace.path());
    }

    void start() {
        auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                           .with_session(*session)
                           .with_agent_config_directory(config.path())
                           .with_tui_mode(mode)
                           .build();
        boost::asio::co_spawn(io,
                coding_agent::tui::run_interactive_mode(terminal, std::move(run)),
                [this](std::exception_ptr exception, support::ExpectedVoid result) {
                    if (exception != nullptr) std::terminate();
                    run_result.emplace(std::move(result));
                });
        driver.emplace(runtime);
        REQUIRE(tests::pump_until(io, [&] { return terminal.modes().started; }));
        drain_ready(io, std::chrono::milliseconds{20});
        assert_footer_rows(terminal, "startup");
    }

    [[nodiscard]] std::string text() const {
        std::string joined;
        for (const auto& row : terminal.screen()) {
            joined.append(row);
            joined.push_back('\n');
        }
        return joined;
    }

    void inject(std::string bytes, std::string_view label) {
        REQUIRE(terminal.inject_input(std::move(bytes)));
        drain_ready(io, std::chrono::milliseconds{20});
        assert_footer_rows(terminal, label);
    }

    void inject_waiting(std::string bytes, std::string_view expected, std::string_view label) {
        REQUIRE(terminal.inject_input(std::move(bytes)));
        REQUIRE(tests::pump_until(io, [&] { return text().find(expected) != std::string::npos; }));
        drain_ready(io, std::chrono::milliseconds{20});
        assert_footer_rows(terminal, label);
    }

    void resize(std::size_t columns, std::size_t rows, std::string_view label) {
        REQUIRE(terminal.inject_resize({.columns = columns, .rows = rows}));
        drain_ready(io, std::chrono::milliseconds{20});
        assert_footer_rows(terminal, label);
    }

    void finish() {
        REQUIRE(terminal.inject_input("\x04"));
        REQUIRE(tests::pump_until(io, [&] { return run_result.has_value(); }));
        CHECK(*run_result);
    }
};

const auto kProbeModes = {coding_agent::TuiMode::Regular, coding_agent::TuiMode::Fullscreen};

[[nodiscard]] std::string mode_name(coding_agent::TuiMode mode) {
    return mode == coding_agent::TuiMode::Regular ? "regular" : "fullscreen";
}

} // namespace

TEST_CASE("Dock footer rows survive autocomplete, multi-line, submit, and resize frames",
        "[coding_agent][tui][dock-footer][issue831][spec]") {
    for (const auto mode : kProbeModes) {
        INFO("mode: " << mode_name(mode));
        ProbeRun probe(mode, 10);
        probe.start();

        // Fill the transcript past the screen height so later growth frames
        // cross the physical bottom of the main screen.
        for (int index = 0; index < 6; ++index) {
            probe.inject_waiting(std::format("filler message {}\r", index),
                    std::format("probe reply {}", index),
                    std::format("filler-{}", index));
        }

        // (i) `/` opens the autocomplete menu: the editor block grows.
        probe.inject("/", "autocomplete-open");
        probe.inject("\x03", "autocomplete-closed");

        // (ii) A nine-line paste grows the editor block past its initial height.
        probe.inject("\x1b[200~line one\nline two\nline three\nline four\nline five\nline six\nline seven\n"
                     "line eight\nline nine\x1b[201~",
                "nine-line-editor");

        // Menu and multi-line text grow the editor block together: the largest
        // single-frame dock growth.
        probe.inject("\x03", "editor-cleared");
        probe.inject("\x1b[200~/\nline two\nline three\nline four\nline five\nline six\nline seven\n"
                     "line eight\nline nine\x1b[201~",
                "autocomplete-over-nine-lines");

        // (iii) Submit shrinks the editor block back.
        probe.inject("\r", "after-submit");

        // (iv) A resize repaints the full frame.
        probe.resize(60, 30, "after-resize");
        probe.finish();
    }
}

TEST_CASE(
        "Dock footer rows survive an over-budget fullscreen dock", "[coding_agent][tui][dock-footer][issue831][spec]") {
    for (const auto mode : kProbeModes) {
        INFO("mode: " << mode_name(mode));
        ProbeRun probe(mode, 1);
        // The gated first turn keeps the session streaming, so every later
        // message queues as a pending dock row.
        probe.scripted.control->gate_at = 0;
        probe.scripted.control->emit_partial_before_gate = true;
        probe.scripted.control->gated_response = "gated done";
        probe.start();

        probe.inject_waiting("stream\r", "Working", "streaming");
        for (int index = 0; index < 20; ++index) {
            probe.inject_waiting(std::format("queued {}\r", index),
                    std::format("queued {}", index),
                    std::format("queued-{}", index));
        }

        // The dock's intrinsic height now exceeds the 30-row frame; the editor
        // block grows on top of it. A dock that allocates the two-row footer
        // one row composes the path row and drops the stats row here.
        probe.inject("/", "autocomplete-open");
        probe.inject("\x1b[200~\nline two\nline three\nline four\nline five\nline six\nline seven\n"
                     "line eight\nline nine\x1b[201~",
                "autocomplete-over-nine-lines");
        probe.resize(60, 30, "after-resize");

        // Releasing the gate drains the queue: the footer must stay put through
        // the shrink frames too, which is why the per-frame assertion above is
        // the acceptance rather than this final screen.
        probe.scripted.control->release();
        REQUIRE(tests::pump_until(probe.io, [&] { return probe.text().find("gated done") != std::string::npos; }));
        drain_ready(probe.io, std::chrono::milliseconds{20});
        assert_footer_rows(probe.terminal, "released");
    }
}
