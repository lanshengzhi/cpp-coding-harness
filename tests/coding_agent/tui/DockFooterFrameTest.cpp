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
    // Physical rows: trailing spaces trimmed, empty rows kept, so a frame
    // that drops a row and leaves a blank hole where the footer belongs is
    // visible in the assertion instead of being skipped over.
    std::vector<std::string> rows;
    for (const auto& line : terminal.screen()) {
        auto trimmed = std::string{line};
        while (!trimmed.empty() && trimmed.back() == ' ')
            trimmed.pop_back();
        rows.push_back(std::move(trimmed));
    }
    return rows;
}

/// What a frame's footer must look like. `Both` pins the two footer rows on
/// the frame. `StatsYields` pins the behavior frozen pi measures when the
/// fullscreen dock's intrinsic height exceeds the frame (audit-831): pi's
/// dock keeps every non-editor entry at `{ shrink: 1, minSize: 0 }`, the
/// shrink distribution takes the footer's second row, and the leaf crop
/// keeps the first — the stats row leaves the frame and the path row is the
/// last content row. Pike matches pi here; the ticket's premise that both
/// rows must survive every frame is recorded as stale rather than fixed
/// into a pi divergence.
enum class FooterExpectation { Both, StatsYields };

/// The footer owns the last two content rows: the workspace path row directly
/// above the context/model stats row. Called after every input injection, so
/// a frame that drops a footer row fails even when a later frame restores it.
/// In fullscreen mode the dock is bottom-anchored, so the physical bottom two
/// screen rows must be exactly the two footer rows (no blank hole below).
void assert_footer_rows(const tui::VirtualTerminal& terminal,
        std::string_view label,
        FooterExpectation expectation = FooterExpectation::Both) {
    const auto rows = composed_rows(terminal);
    INFO("frame: " << label);
    INFO("composed rows: " << [&] {
        std::string joined;
        for (const auto& row : rows)
            joined += std::format("\n|{}|", row);
        return joined;
    }());
    REQUIRE(rows.size() >= 2);
    // The footer's two rows must be the last two CONTENT rows and contiguous:
    // a frame that drops one and leaves a blank hole fails the row checks.
    std::size_t tail = rows.size();
    while (tail > 0 && rows[tail - 1].empty())
        --tail;
    REQUIRE(tail >= 2);
    const auto& path_row = rows[tail - 2];
    const auto& stats_row = rows[tail - 1];
    if (expectation == FooterExpectation::Both) {
        CHECK(stats_row.find("/128k (auto)") != std::string::npos);
        CHECK(path_row.find("cpp-harness") != std::string::npos);
        CHECK(path_row.find("(auto)") == std::string::npos);
        return;
    }
    // StatsYields: the measured over-budget frame. The stats row is absent,
    // and the path row is the frame's last content row.
    for (const auto& row : rows)
        CHECK(row.find("/128k (auto)") == std::string::npos);
    CHECK(stats_row.find("cpp-harness") != std::string::npos);
    CHECK(stats_row.find("(auto)") == std::string::npos);
}

/// Fullscreen-only physical-bottom pin: the dock is bottom-anchored. On
/// fitting frames the last two screen rows must be exactly the footer's two
/// rows (no blank hole below). On over-budget frames both runtimes park the
/// path row on the physical bottom row with the stats row yielded (pi-parity,
/// see FooterExpectation).
void assert_fullscreen_bottom(const tui::VirtualTerminal& terminal,
        std::string_view label,
        FooterExpectation expectation = FooterExpectation::Both) {
    const auto rows = composed_rows(terminal);
    INFO("frame: " << label);
    INFO("physical tail: |" << rows[rows.size() - 3] << "|\n|" << rows[rows.size() - 2] << "|\n|"
                            << rows[rows.size() - 1] << "|");
    const auto& penultimate = rows[rows.size() - 2];
    const auto& last = rows[rows.size() - 1];
    if (expectation == FooterExpectation::StatsYields) {
        // Measured over-budget shape, identical on both runtimes (audit-831):
        // the shrink distribution allocates the footer one row, the leaf crop
        // keeps the first, and the path row is parked on the physical bottom
        // row with the stats row gone and dock content above it.
        CHECK(last.find("cpp-harness") != std::string::npos);
        CHECK(last.find("(auto)") == std::string::npos);
        return;
    }
    CHECK(penultimate.find("cpp-harness") != std::string::npos);
    CHECK(penultimate.find("(auto)") == std::string::npos);
    CHECK(last.find("/128k (auto)") != std::string::npos);
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
        assert_frame("startup", FooterExpectation::Both);
    }

    [[nodiscard]] std::string text() const {
        std::string joined;
        for (const auto& row : terminal.screen()) {
            joined.append(row);
            joined.push_back('\n');
        }
        return joined;
    }

    void assert_frame(std::string_view label, FooterExpectation expectation) const {
        assert_footer_rows(terminal, label, expectation);
        if (mode == coding_agent::TuiMode::Fullscreen) assert_fullscreen_bottom(terminal, label, expectation);
    }

    void inject(std::string bytes, std::string_view label, FooterExpectation expectation = FooterExpectation::Both) {
        REQUIRE(terminal.inject_input(std::move(bytes)));
        drain_ready(io, std::chrono::milliseconds{20});
        assert_frame(label, expectation);
    }

    void inject_waiting(std::string bytes,
            std::string_view expected,
            std::string_view label,
            FooterExpectation expectation = FooterExpectation::Both) {
        REQUIRE(terminal.inject_input(std::move(bytes)));
        REQUIRE(tests::pump_until(io, [&] { return text().find(expected) != std::string::npos; }));
        drain_ready(io, std::chrono::milliseconds{20});
        assert_frame(label, expectation);
    }

    void resize(std::size_t columns,
            std::size_t rows,
            std::string_view label,
            FooterExpectation expectation = FooterExpectation::Both) {
        REQUIRE(terminal.inject_resize({.columns = columns, .rows = rows}));
        drain_ready(io, std::chrono::milliseconds{20});
        assert_frame(label, expectation);
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
        // block grows on top of it. In fullscreen both pike and frozen pi
        // (audit-831) yield the footer's stats row on these two frames while
        // the dock stays over budget; the expectation pins that measured
        // parity behavior, so a later "fix" that keeps the row fails here
        // until pi changes. Regular mode has no dock and keeps both rows.
        const auto yields =
                mode == coding_agent::TuiMode::Fullscreen ? FooterExpectation::StatsYields : FooterExpectation::Both;
        probe.inject("/", "autocomplete-open");
        probe.inject("\x1b[200~\nline two\nline three\nline four\nline five\nline six\nline seven\n"
                     "line eight\nline nine\x1b[201~",
                "autocomplete-over-nine-lines",
                yields);
        probe.resize(60, 30, "after-resize", yields);

        // Releasing the gate drains the queue: the footer must be back to two
        // rows on the shrink frames too, which is why the per-frame assertion
        // above is the acceptance rather than this final screen.
        probe.scripted.control->release();
        REQUIRE(tests::pump_until(probe.io, [&] { return probe.text().find("gated done") != std::string::npos; }));
        drain_ready(probe.io, std::chrono::milliseconds{20});
        probe.assert_frame("released", FooterExpectation::Both);
    }
}
