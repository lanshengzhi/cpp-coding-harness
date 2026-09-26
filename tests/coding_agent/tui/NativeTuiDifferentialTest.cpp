// Dual-runtime Native TUI evidence capture for issue #800.
//
// This test is deliberately a capture seam rather than another C++-side
// golden. The TypeScript harness supplies one scenario and the exact input
// sequence; this process runs the same sequence through Pike's Native TUI
// VirtualTerminal and writes both visible cells and terminal output. The
// harness compares visible cell text plus the normalized, ordered SGR token
// structure — raw ANSI bytes are retained as evidence only, never compared —
// and keeps the existing C++ goldens as independent byte-level gates.

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/Theme.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/PumpUntil.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/RuntimeLoopDriver.hpp"
#include "support/ScriptedRuntimeFixture.hpp"
#include "support/Json.hpp"
#include "support/ModelFixture.hpp"

#include <cch/ai/Content.hpp>
#include <cch/coding_agent/AgentConfigDir.hpp>
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
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
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

[[nodiscard]] std::vector<tui::TerminalDimensions> parse_resize_sequence(std::string_view value) {
    if (value.empty()) return {};
    std::vector<tui::TerminalDimensions> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto arrow = value.find("->", start);
        const auto piece = value.substr(start, arrow == std::string_view::npos ? arrow : arrow - start);
        const auto separator = piece.find('x');
        if (separator == std::string_view::npos) std::abort();
        result.push_back({
                .columns = parse_size(piece.substr(0, separator)),
                .rows = parse_size(piece.substr(separator + 1)),
        });
        if (arrow == std::string_view::npos) break;
        start = arrow + 2;
    }
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

[[nodiscard]] std::string canonical_color(std::string_view value) {
    if (value.empty()) return {};
    if (value.starts_with("38;2;") || value.starts_with("48;2;")) {
        constexpr std::size_t prefix_length = 5;
        const auto first = value.find(';', prefix_length);
        const auto second = value.find(';', first + 1);
        const auto red = parse_size(value.substr(prefix_length, first - prefix_length));
        const auto green = parse_size(value.substr(first + 1, second - first - 1));
        const auto blue = parse_size(value.substr(second + 1));
        return std::format("#{:02x}{:02x}{:02x}", red, green, blue);
    }
    if (value.starts_with("38;5;")) return std::string{"palette:"} + std::string{value.substr(value.rfind(';') + 1)};
    if (value.starts_with("48;5;")) return std::string{"palette:"} + std::string{value.substr(value.rfind(';') + 1)};
    const auto code = parse_size(value);
    if ((code >= 30 && code <= 37) || (code >= 40 && code <= 47)) return "palette:" + std::to_string(code - 30);
    if ((code >= 90 && code <= 97) || (code >= 100 && code <= 107)) return "palette:" + std::to_string(code - 90);
    return std::string{value};
}

[[nodiscard]] support::JsonValue style_json(const tui::VirtualTerminalStyle& style) {
    support::JsonValue::object_t value;
    value.emplace("bold", support::JsonValue{style.bold});
    value.emplace("dim", support::JsonValue{style.dim});
    value.emplace("italic", support::JsonValue{style.italic});
    value.emplace("underline", support::JsonValue{style.underline});
    value.emplace("blink", support::JsonValue{style.blink});
    value.emplace("inverse", support::JsonValue{style.inverse});
    value.emplace("hidden", support::JsonValue{style.hidden});
    value.emplace("strikethrough", support::JsonValue{style.strikethrough});
    value.emplace("foreground", support::JsonValue{canonical_color(style.fg_color)});
    value.emplace("background", support::JsonValue{canonical_color(style.bg_color)});
    return support::JsonValue{std::move(value)};
}

void append_styled_run(support::JsonValue::array_t& runs, std::string& text, const tui::VirtualTerminalStyle& style) {
    if (text.empty()) return;
    support::JsonValue::object_t run;
    run.emplace("text", support::JsonValue{text});
    run.emplace("style", style_json(style));
    runs.emplace_back(support::JsonValue{std::move(run)});
    text.clear();
}

[[nodiscard]] support::JsonValue styled_rows_json(const std::vector<std::vector<tui::VirtualTerminalCell>>& rows) {
    support::JsonValue::array_t encoded_rows;
    encoded_rows.reserve(rows.size());
    for (const auto& row : rows) {
        const tui::VirtualTerminalStyle default_style;
        std::size_t occupied = 0;
        for (std::size_t column = 0; column < row.size(); ++column) {
            const auto& cell = row[column];
            if (!cell.grapheme.empty() || cell.continuation || cell.style != default_style) occupied = column + 1;
        }
        support::JsonValue::array_t runs;
        std::string text;
        tui::VirtualTerminalStyle run_style;
        for (std::size_t column = 0; column < occupied; ++column) {
            const auto& cell = row[column];
            if (cell.continuation) continue;
            if (!text.empty() && cell.style != run_style) append_styled_run(runs, text, run_style);
            if (text.empty()) run_style = cell.style;
            text += cell.grapheme.empty() ? " " : cell.grapheme;
        }
        append_styled_run(runs, text, run_style);
        encoded_rows.emplace_back(std::move(runs));
    }
    return support::JsonValue{std::move(encoded_rows)};
}

void require_directory(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    REQUIRE(!error);
}

std::string theme_color_hex(const coding_agent::tui::ResolvedThemeColor& color) {
    const auto* rgb = std::get_if<coding_agent::tui::RgbThemeColor>(&color);
    if (rgb == nullptr) std::terminate();
    return std::format("#{:02x}{:02x}{:02x}",
            static_cast<unsigned>(rgb->red),
            static_cast<unsigned>(rgb->green),
            static_cast<unsigned>(rgb->blue));
}

[[nodiscard]] support::JsonValue theme_evidence(tui::TerminalColorCapability capability) {
    const auto theme = coding_agent::tui::builtin_dark_theme();
    support::JsonValue::object_t colors;
    for (const auto token : coding_agent::tui::all_theme_tokens())
        colors.emplace(std::string{coding_agent::tui::theme_token_name(token)},
                theme_color_hex(coding_agent::tui::color_for(theme, token)));
    support::JsonValue::object_t root;
    root.emplace("name", support::JsonValue{theme.name});
    root.emplace("colorCapability",
            support::JsonValue{
                    std::string{capability == tui::TerminalColorCapability::TrueColor ? "truecolor" : "xterm256"}});
    root.emplace("colors", support::JsonValue{std::move(colors)});
    return support::JsonValue{std::move(root)};
}

void write_capture(const std::filesystem::path& path,
        std::string_view scenario,
        std::size_t width,
        const std::filesystem::path& workspace,
        const std::vector<std::string>& inputs,
        tui::TerminalColorCapability capability,
        const std::vector<support::JsonValue>& snapshots) {
    support::JsonValue::object_t root;
    root.emplace("runtime", support::JsonValue{"pike"});
    root.emplace("scenario", support::JsonValue{std::string{scenario}});
    root.emplace("width", support::JsonValue{static_cast<double>(width)});
    root.emplace("workspace", support::JsonValue{workspace.string()});
    root.emplace("inputs", lines_json(inputs));
    root.emplace("theme", theme_evidence(capability));
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

[[nodiscard]] std::vector<ai::Model> differential_catalog() {
    auto reasoning = tests::make_full_thinking_model("faux-1");
    reasoning.provider = "faux";
    reasoning.api = "faux";
    reasoning.name = "Faux Reasoning";
    reasoning.base_url = "http://localhost:0";
    reasoning.input = {ai::ModelInput::Text, ai::ModelInput::Image};
    reasoning.context_window = 128000;
    reasoning.max_tokens = 16384;

    auto plain = tests::make_model("faux-2", "faux", "faux");
    plain.name = "Faux Plain";
    plain.base_url = "http://localhost:0";
    plain.input = {ai::ModelInput::Text, ai::ModelInput::Image};
    plain.context_window = 128000;
    plain.max_tokens = 16384;
    return {std::move(reasoning), std::move(plain)};
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
    request.provide_user_shell = true;
    request.workspace = workspace;
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.session_facts.provider = "faux";
    request.session_facts.model = "faux-1";
    request.session_facts.thinking = "off";
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
    value.emplace("styledVisible", styled_rows_json(terminal.cells()));
    value.emplace("styledScrollback", styled_rows_json(terminal.scrollback_cells()));
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
        "[coding_agent][tui][differential][issue797][issue800][spec]") {
    const auto scenario = environment_or_empty("CCH_DIFFERENTIAL_SCENARIO");
    if (scenario.empty()) SKIP("dual-runtime capture environment is not selected");
    const auto width = parse_size(environment_or_empty("CCH_DIFFERENTIAL_WIDTH"));
    const auto inputs = input_sequences();
    const auto output_path = environment_or_empty("CCH_DIFFERENTIAL_OUTPUT");
    const auto workspace = deterministic_workspace(scenario);
    const std::filesystem::path configured_agent_directory{environment_or_empty("CCH_DIFFERENTIAL_AGENT_DIRECTORY")};
    REQUIRE_FALSE(configured_agent_directory.empty());
    tests::EnvVarGuard xdg_config_home{"XDG_CONFIG_HOME"};
    xdg_config_home.set(configured_agent_directory.parent_path().parent_path().string());
    const auto agent_config_directory = coding_agent::agent_config_dir();
    REQUIRE(agent_config_directory == configured_agent_directory);
    require_directory(workspace / ".pi");
    require_directory(agent_config_directory / "skills");
    require_directory(agent_config_directory / "prompts");
    require_directory(agent_config_directory / "themes");
    {
        std::ofstream settings(agent_config_directory / "settings.json", std::ios::binary);
        settings << R"({"theme":"dark"})";
        settings.flush();
        REQUIRE(settings.good());
    }
    tests::EnvVarGuard home{"HOME"};
    home.set("/home/tester");
    auto scripted = tests::ScriptedRuntimeFixture(differential_catalog());
    add_scripted_responses(scripted, scenario);
    tests::RuntimeFixture runtime;
    auto session = make_session(runtime, scripted, workspace);

    tui::VirtualTerminal terminal({
            .columns = width,
            .rows = 24,
            .capabilities =
                    {
                            .synchronized_output = true,
                            .color = tui::TerminalColorCapability::TrueColor,
                    },
    });
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                       .with_session(*session)
                       .with_agent_config_directory(agent_config_directory)
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
    snapshots.push_back(snapshot(terminal, 0));
    auto output_offset = output_size(terminal);
    const auto resize = environment_or_empty("CCH_DIFFERENTIAL_RESIZE");
    for (const auto dimensions : parse_resize_sequence(resize)) {
        const auto resize_offset = output_size(terminal);
        REQUIRE(terminal.inject_resize(dimensions));
        REQUIRE(tests::pump_until(io, [&] { return output_size(terminal) > resize_offset; }));
        drain_ready(io, std::chrono::milliseconds{20});
        snapshots.push_back(snapshot(terminal, output_offset));
        output_offset = output_size(terminal);
    }

    std::size_t response_index = 0;
    for (std::size_t input_index = 0; input_index < inputs.size(); ++input_index) {
        const auto& input = inputs[input_index];
        const auto input_offset = output_size(terminal);
        REQUIRE(terminal.inject_input(input));
        if (scenario == "tool-result" && input.ends_with("\r")) {
            REQUIRE(tests::pump_until(
                    io, [&] { return screen_contains(terminal, "I will read the deterministic fixture."); }));
            drain_ready(io, std::chrono::milliseconds{20});
            snapshots.push_back(snapshot(terminal, output_offset));
            output_offset = output_size(terminal);
            CHECK(tests::pump_until(io, [&] { return screen_contains(terminal, "deterministic tool answer"); }));
            // pi `read.ts:111-115` returns the empty string for a collapsed
            // successful read result, so neither runtime shows the file body:
            // the checked-in pi capture for this scenario carries `read
            // notes.txt` and no `alpha` either. The assertion is on the title
            // being present and the body being absent, not on a body that only
            // pike used to draw.
            CHECK(screen_contains(terminal, "read notes.txt"));
            CHECK_FALSE(screen_contains(terminal, "alpha"));
            CHECK_FALSE(screen_contains(terminal, "ENOENT"));
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
            if (scenario == "model-selector" && input_index == 0)
                REQUIRE(tests::pump_until(io, [&] { return screen_contains(terminal, "Only showing models"); }));
            if (scenario == "model-selector" && input_index == 1)
                REQUIRE(tests::pump_until(io, [&] { return screen_contains(terminal, "Model:"); }));
            if (scenario == "thinking-selector" && input_index == 0)
                REQUIRE(tests::pump_until(io, [&] { return screen_contains(terminal, "Set thinking level"); }));
            if (scenario == "thinking-selector" && input_index == 1)
                REQUIRE(tests::pump_until(io, [&] { return screen_contains(terminal, "No reasoning"); }));
            if (scenario == "thinking-selector" && input_index == 2)
                REQUIRE(tests::pump_until(io, [&] { return screen_contains(terminal, "Thinking level:"); }));
            if (scenario == "settings-selector" && input_index == 0)
                REQUIRE(tests::pump_until(io, [&] { return screen_contains(terminal, "Open settings menu"); }));
            if (scenario == "settings-selector" && (input_index == 1 || input_index == 2))
                REQUIRE(tests::pump_until(io, [&] { return screen_contains(terminal, "Type to search"); }));
            if (scenario == "editor-long" || scenario == "editor-cjk" || scenario == "editor-token") {
                const auto expected_editor_text = inputs[0].substr(0, 20);
                if (input_index == 0)
                    REQUIRE(tests::pump_until(io, [&] { return screen_contains(terminal, expected_editor_text); }));
                else
                    REQUIRE(tests::pump_until(io, [&] { return !screen_contains(terminal, expected_editor_text); }));
            }
        }
        drain_ready(io, std::chrono::milliseconds{20});
        snapshots.push_back(snapshot(terminal, output_offset));
        output_offset = output_size(terminal);
    }

    if (!output_path.empty()) {
        write_capture(output_path, scenario, width, workspace, inputs, terminal.capabilities().color, snapshots);
    }

    REQUIRE(terminal.inject_input("\x04"));
    REQUIRE((tests::pump_until(io, [&] { return run_result.has_value(); }, std::chrono::seconds{5})));
    REQUIRE(run_result);
    CHECK(*run_result);
}
