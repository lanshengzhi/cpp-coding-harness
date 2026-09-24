// Dual-runtime Native TUI evidence capture for issue #800.
//
// This test is deliberately a capture seam rather than another C++-side
// golden. The TypeScript harness supplies one scenario and the exact input
// sequence; this process runs the same sequence through Pike's Native TUI
// VirtualTerminal and writes both visible cells and terminal output. The
// harness compares the two runtimes and keeps the existing C++ goldens as
// independent byte-level gates.

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/PumpUntil.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/RuntimeLoopDriver.hpp"
#include "support/ScriptedRuntimeFixture.hpp"
#include "support/Json.hpp"

#include <cch/ai/Content.hpp>
#include <cch/support/JsonValue.hpp>
#include <cch/tui/Terminal.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

using namespace cch;
using tests::drain_ready;

namespace {

[[nodiscard]] std::size_t parse_size(std::string_view value) {
    std::size_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) std::abort();
    return result;
}

[[nodiscard]] std::string environment_or_empty(const char* name) {
    const auto* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string{value};
}

[[nodiscard]] int hex_value(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

[[nodiscard]] std::string decode_hex(std::string_view value) {
    std::string decoded;
    decoded.reserve(value.size() / 2);
    for (std::size_t index = 0; index + 1 < value.size(); index += 2) {
        const auto high = hex_value(value[index]);
        const auto low = hex_value(value[index + 1]);
        if (high < 0 || low < 0) continue;
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    return decoded;
}

[[nodiscard]] std::vector<std::string> input_sequences() {
    const auto encoded = environment_or_empty("CCH_DIFFERENTIAL_INPUTS");
    std::vector<std::string> inputs;
    std::size_t start = 0;
    while (start <= encoded.size()) {
        const auto end = encoded.find('|', start);
        const auto piece = encoded.substr(start, end == std::string::npos ? end : end - start);
        if (piece == "-")
            inputs.emplace_back();
        else if (!piece.empty())
            inputs.push_back(decode_hex(piece));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return inputs;
}

[[nodiscard]] support::JsonValue lines_json(const std::vector<std::string>& lines) {
    support::JsonValue::array_t values;
    values.reserve(lines.size());
    for (const auto& line : lines)
        values.emplace_back(line);
    return support::JsonValue{std::move(values)};
}

void write_capture(const std::filesystem::path& path,
        std::string_view scenario,
        std::size_t width,
        const std::filesystem::path& workspace,
        const std::vector<std::string>& inputs,
        const std::vector<support::JsonValue>& snapshots) {
    support::JsonValue::object_t root;
    root.emplace("runtime", support::JsonValue{"pike"});
    root.emplace("scenario", support::JsonValue{std::string{scenario}});
    root.emplace("width", support::JsonValue{static_cast<double>(width)});
    root.emplace("workspace", support::JsonValue{workspace.string()});
    root.emplace("inputs", lines_json(inputs));
    root.emplace("snapshots", support::JsonValue{snapshots});
    const auto serialized = support::write_json(support::JsonValue{std::move(root)});
    REQUIRE(serialized.has_value());
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    REQUIRE(!error);
    std::ofstream output{path, std::ios::binary};
    output << *serialized;
}

[[nodiscard]] std::string scenario_response(std::string_view scenario) {
    if (scenario == "user-message") return "deterministic assistant reply";
    if (scenario == "status-footer") return "deterministic status reply";
    if (scenario == "scrollback") return "deterministic scrollback reply";
    return "deterministic tool answer";
}

void add_scripted_responses(tests::ScriptedRuntimeFixture& scripted, std::string_view scenario) {
    if (scenario == "tool-result") {
        auto tool_turn = ai::assistant_text_message("I will read the deterministic fixture.");
        tool_turn.content.emplace_back(ai::tool_call_content("differential-read", "read", R"({"path":"notes.txt"})"));
        tool_turn.stop_reason = ai::AssistantStopReason::ToolUse;
        scripted.control->responses.push_back(std::move(tool_turn));
        scripted.control->responses.push_back(ai::assistant_text_message("deterministic tool answer"));
        return;
    }
    if (scenario == "scrollback") {
        for (std::size_t index = 0; index < 6; ++index) {
            scripted.control->responses.push_back(
                    ai::assistant_text_message("deterministic scrollback reply " + std::to_string(index + 1)));
        }
        return;
    }
    if (scenario == "user-message" || scenario == "status-footer") {
        scripted.control->responses.push_back(ai::assistant_text_message(scenario_response(scenario)));
    }
}

[[nodiscard]] std::filesystem::path deterministic_workspace(std::string_view scenario) {
    const auto configured = environment_or_empty("CCH_DIFFERENTIAL_WORKSPACE");
    const std::filesystem::path workspace =
            configured.empty() ? std::filesystem::path{"/tmp/cpp-harness-pike-differential-" + std::string{scenario} +
                                                       "-" + std::to_string(::getpid())}
                               : std::filesystem::path{configured};
    std::error_code error;
    std::filesystem::remove_all(workspace, error);
    error.clear();
    std::filesystem::create_directories(workspace, error);
    REQUIRE(!error);
    {
        std::ofstream notes(workspace / "notes.txt", std::ios::binary);
        notes << "alpha\n";
    }
    return workspace;
}

[[nodiscard]] std::unique_ptr<coding_agent::AgentSession> make_session(tests::RuntimeFixture& runtime,
        tests::ScriptedRuntimeFixture& scripted,
        const std::filesystem::path& workspace) {
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_target = coding_agent::InMemorySessionTarget{};
    request.workspace = workspace;
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.execution_runtime_target = runtime.make_target();
    request.model_runtime = scripted.runtime;
    auto created = runtime.run(coding_agent::create_agent_session_async(std::move(request), std::nullopt, {}));
    REQUIRE(created.has_value());
    return std::move(created->session);
}

[[nodiscard]] support::JsonValue snapshot(const tui::VirtualTerminal& terminal, std::size_t output_offset) {
    support::JsonValue::object_t value;
    value.emplace("visible", lines_json(terminal.screen()));
    value.emplace("scrollback", lines_json(terminal.scrollback()));
    std::string ansi;
    for (std::size_t index = output_offset; index < terminal.output().size(); ++index) {
        ansi.append(terminal.output()[index]);
    }
    value.emplace("ansi", support::JsonValue{std::move(ansi)});
    return support::JsonValue{std::move(value)};
}

[[nodiscard]] std::size_t output_size(const tui::VirtualTerminal& terminal) { return terminal.output().size(); }

[[nodiscard]] bool screen_contains(const tui::VirtualTerminal& terminal, std::string_view expected) {
    for (const auto& line : terminal.screen())
        if (line.find(expected) != std::string::npos) return true;
    return false;
}

} // namespace

TEST_CASE("Native TUI differential capture exposes deterministic Pike cells and ANSI",
        "[coding_agent][tui][differential][issue800][spec]") {
    const auto scenario = environment_or_empty("CCH_DIFFERENTIAL_SCENARIO");
    if (scenario.empty()) SKIP("dual-runtime capture environment is not selected");
    const auto width = parse_size(environment_or_empty("CCH_DIFFERENTIAL_WIDTH"));
    const auto inputs = input_sequences();
    const auto output_path = environment_or_empty("CCH_DIFFERENTIAL_OUTPUT");
    const auto workspace = deterministic_workspace(scenario);
    tests::EnvVarGuard home{"HOME"};
    home.set("/home/tester");
    tests::ScriptedRuntimeFixture scripted;
    add_scripted_responses(scripted, scenario);
    tests::RuntimeFixture runtime;
    auto session = make_session(runtime, scripted, workspace);

    tui::VirtualTerminal terminal({.columns = width, .rows = 24});
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                       .with_session(*session)
                       .with_agent_config_directory(workspace)
                       .build();
    boost::asio::co_spawn(io,
            coding_agent::tui::run_interactive_mode(terminal, std::move(run)),
            [&](std::exception_ptr exception, support::ExpectedVoid result) {
                if (exception != nullptr) std::terminate();
                run_result.emplace(std::move(result));
            });
    tests::RuntimeLoopDriver runtime_driver(runtime);
    REQUIRE(tests::pump_until(io, [&] { return screen_contains(terminal, "Press ctrl+o"); }));
    drain_ready(io, std::chrono::milliseconds{20});

    std::vector<support::JsonValue> snapshots;
    auto output_offset = output_size(terminal);
    snapshots.push_back(snapshot(terminal, 0));
    output_offset = output_size(terminal);
    const auto resize = environment_or_empty("CCH_DIFFERENTIAL_RESIZE");
    if (!resize.empty()) {
        const auto separator = resize.find('x');
        REQUIRE(separator != std::string::npos);
        const auto next_width = parse_size(resize.substr(0, separator));
        const auto next_height = parse_size(resize.substr(separator + 1));
        const auto resize_offset = output_size(terminal);
        REQUIRE(terminal.inject_resize({.columns = next_width, .rows = next_height}));
        REQUIRE(tests::pump_until(io, [&] { return output_size(terminal) > resize_offset; }));
        drain_ready(io, std::chrono::milliseconds{20});
        snapshots.push_back(snapshot(terminal, output_offset));
        output_offset = output_size(terminal);
    }

    std::size_t response_index = 0;
    for (const auto& input : inputs) {
        const auto input_offset = output_size(terminal);
        REQUIRE(terminal.inject_input(input));
        drain_ready(io, std::chrono::milliseconds{20});
        if (scenario == "tool-result" && input.ends_with("\r")) {
            REQUIRE(tests::pump_until(
                    io, [&] { return screen_contains(terminal, "I will read the deterministic fixture."); }));
            drain_ready(io, std::chrono::milliseconds{20});
            snapshots.push_back(snapshot(terminal, output_offset));
            output_offset = output_size(terminal);
            REQUIRE(tests::pump_until(io, [&] { return screen_contains(terminal, "deterministic tool answer"); }));
        } else if (input.ends_with("\r") &&
                   (scenario == "user-message" || scenario == "status-footer" || scenario == "scrollback")) {
            const auto expected = scenario == "user-message" ? "deterministic assistant reply"
                                  : scenario == "status-footer"
                                          ? "deterministic status reply"
                                          : "deterministic scrollback reply " + std::to_string(response_index + 1);
            REQUIRE(tests::pump_until(io, [&] { return screen_contains(terminal, expected); }));
            ++response_index;
        } else if (scenario == "settings-selector" && input == "\x1b") {
            REQUIRE(terminal.flush_input());
            REQUIRE(tests::pump_until(io, [&] { return !screen_contains(terminal, "Type to search"); }));
        } else {
            REQUIRE(tests::pump_until(io, [&] { return output_size(terminal) > input_offset; }));
        }
        drain_ready(io, std::chrono::milliseconds{20});
        snapshots.push_back(snapshot(terminal, output_offset));
        output_offset = output_size(terminal);
    }

    if (!output_path.empty()) {
        write_capture(output_path, scenario, width, workspace, inputs, snapshots);
    }

    REQUIRE(terminal.inject_input("\x04"));
    REQUIRE((tests::pump_until(io, [&] { return run_result.has_value(); }, std::chrono::seconds{5})));
    REQUIRE(run_result);
    CHECK(*run_result);
}
