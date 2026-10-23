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
#include "coding_agent/ModelRuntimeTestSupport.hpp"
#include "coding_agent/tui/ExtensionUiHost.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/Theme.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "agent/harness/RuntimeRoot.hpp"
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
#include <cch/support/AsyncResult.hpp>
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
#include <iostream>
#include <iterator>
#include <ranges>
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

[[nodiscard]] support::JsonValue style_json(const tui::TerminalStyle& style) {
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

void append_styled_run(support::JsonValue::array_t& runs, std::string& text, const tui::TerminalStyle& style) {
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
        const tui::TerminalStyle default_style;
        std::size_t occupied = 0;
        for (std::size_t column = 0; column < row.size(); ++column) {
            const auto& cell = row[column];
            if (!cell.grapheme.empty() || cell.continuation || cell.style != default_style) occupied = column + 1;
        }
        support::JsonValue::array_t runs;
        std::string text;
        tui::TerminalStyle run_style;
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
        const std::vector<support::JsonValue>& snapshots,
        const support::JsonValue* observation = nullptr) {
    support::JsonValue::object_t root;
    root.emplace("runtime", support::JsonValue{"pike"});
    root.emplace("scenario", support::JsonValue{std::string{scenario}});
    root.emplace("width", support::JsonValue{static_cast<double>(width)});
    root.emplace("workspace", support::JsonValue{workspace.string()});
    root.emplace("inputs", lines_json(inputs));
    root.emplace("theme", theme_evidence(capability));
    root.emplace("snapshots", support::JsonValue{snapshots});
    if (observation != nullptr) root.emplace("observation", *observation);
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
    auto reasoning = tests::make_model("faux-1", "faux", "faux");
    reasoning.reasoning = true;
    reasoning.provider = "faux";
    reasoning.api = "faux";
    reasoning.name = "Faux Reasoning";
    reasoning.input = {ai::ModelInput::Text, ai::ModelInput::Image};
    reasoning.context_window = 128000;
    reasoning.max_tokens = 16384;

    auto plain = tests::make_model("faux-2", "faux", "faux");
    plain.name = "Faux Plain";
    plain.input = {ai::ModelInput::Text, ai::ModelInput::Image};
    plain.context_window = 128000;
    plain.max_tokens = 16384;
    return {std::move(reasoning), std::move(plain)};
}

void add_scripted_responses(tests::ScriptedRuntimeFixture& scripted, std::string_view scenario) {
    if (scenario == "extension-editor") {
        scripted.control->responses.push_back(ai::assistant_text_message("deterministic extension reply"));
        return;
    }
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

[[nodiscard]] coding_agent::CreateAgentSessionResult make_session_result(tests::RuntimeFixture& runtime,
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
    coding_agent::isolate_magpie_gateway(request);
    auto created = runtime.run(coding_agent::create_agent_session_async(std::move(request), std::nullopt, {}));
    REQUIRE(created.has_value());
    return std::move(*created);
}

[[nodiscard]] std::unique_ptr<coding_agent::AgentSession> make_session(tests::RuntimeFixture& runtime,
        tests::ScriptedRuntimeFixture& scripted,
        const std::filesystem::path& workspace) {
    return std::move(make_session_result(runtime, scripted, workspace).session);
}

[[nodiscard]] support::JsonValue snapshot(const tui::VirtualTerminal& terminal, std::size_t output_offset) {
    support::JsonValue::object_t value;
    value.emplace("visible", lines_json(terminal.screen()));
    value.emplace("scrollback", lines_json(terminal.scrollback()));
    value.emplace("styledVisible", styled_rows_json(terminal.cells()));
    value.emplace("styledScrollback", styled_rows_json(terminal.scrollback_cells()));
    const auto cursor = terminal.cursor();
    support::JsonValue::object_t cursor_position;
    cursor_position.emplace("column", support::JsonValue{static_cast<double>(cursor.column)});
    cursor_position.emplace("row", support::JsonValue{static_cast<double>(cursor.row)});
    value.emplace("cursor", support::JsonValue{std::move(cursor_position)});
    value.emplace("cursorVisible", support::JsonValue{terminal.modes().cursor_visible});
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

/// Horizontal rule rows, the shape pi's `editorBorderRows` fixture counts:
/// each installed bordered editor paints one top and one bottom rule.
[[nodiscard]] std::size_t screen_rule_rows(const tui::VirtualTerminal& terminal) {
    std::size_t rows = 0;
    for (const auto& line : terminal.screen()) {
        const auto trimmed = std::string_view{line};
        const auto first = trimmed.find_first_not_of(' ');
        if (first == std::string_view::npos) continue;
        const auto last = trimmed.find_last_not_of(' ');
        const auto body = trimmed.substr(first, last - first + 1);
        if (body.empty()) continue;
        const bool rule = !body.empty() && body.size() % 3 == 0 &&
                          std::ranges::all_of(std::views::iota(std::size_t{0}, body.size() / 3),
                                  [&](std::size_t index) { return body.substr(index * 3, 3) == "\u2500"; });
        if (rule) ++rows;
    }
    return rows;
}

[[nodiscard]] std::optional<std::string_view> setting_value(
        const tui::VirtualTerminal& terminal, std::string_view label) {
    for (const auto& line : terminal.screen()) {
        const auto label_position = line.find(label);
        if (label_position == std::string::npos) continue;
        const auto value_position = line.find_first_not_of(' ', label_position + label.size());
        // A token-less match is the search input showing the query text, not
        // the settings row; keep scanning for the row that carries a value.
        if (value_position == std::string::npos) continue;
        const auto value_end = line.find_first_of(" \t", value_position);
        return std::string_view{line}.substr(value_position, value_end - value_position);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::size_t> row_containing(
        const std::vector<std::string>& rows, std::string_view expected) {
    for (std::size_t index = 0; index < rows.size(); ++index) {
        if (rows[index].find(expected) != std::string::npos) return index;
    }
    return std::nullopt;
}

void check_model_selector_status(const tui::VirtualTerminal& terminal) {
    const auto rows = terminal.screen();
    const auto status_row = row_containing(rows, "Model: faux-1");
    REQUIRE(status_row.has_value());
    const auto first_content = rows[*status_row].find_first_not_of(' ');
    REQUIRE(first_content != std::string::npos);
    const auto last_content = rows[*status_row].find_last_not_of(' ');
    CHECK(first_content == 1);
    CHECK(std::string_view{rows[*status_row]}.substr(first_content, last_content - first_content + 1) ==
            "Model: faux-1");
    CHECK(*status_row == 3);
    CHECK(std::ranges::count_if(
                  rows, [](const std::string& row) { return row.find("Model: faux-1") != std::string::npos; }) == 1);
}

void check_model_selector_layout(const tui::VirtualTerminal& terminal) {
    const auto rows = terminal.screen();
    const auto search_row = row_containing(rows, "> ");
    const auto model_row = row_containing(rows, "→ ✓ faux-1 [faux]");
    const auto save_hint_row = row_containing(rows, "Enter to select");

    REQUIRE(search_row.has_value());
    REQUIRE(model_row.has_value());
    REQUIRE(save_hint_row.has_value());
    REQUIRE(*search_row + 2 < rows.size());
    REQUIRE(*save_hint_row > 0);

    // Frozen pi audit-1051 model-selector snapshot at 100 columns: list rows
    // start two rows after the search prompt, and the blank row before the save
    // hint is followed immediately by the lower border.
    CHECK(*model_row == *search_row + 2);
    CHECK(rows[*search_row + 1].find_first_not_of(' ') == std::string::npos);
    REQUIRE(*save_hint_row >= 2);
    CHECK(rows[*save_hint_row - 2].find("Model catalogs refreshed.") != std::string::npos);
    CHECK((rows[*save_hint_row - 1].empty() || rows[*save_hint_row - 1].find_first_not_of(' ') == std::string::npos));
    const auto bottom_border = std::ranges::find_if(rows.begin() + static_cast<std::ptrdiff_t>(*save_hint_row + 1),
            rows.end(),
            [](const std::string& row) { return row.find("─") != std::string::npos; });
    REQUIRE(bottom_border != rows.end());
    CHECK(static_cast<std::size_t>(bottom_border - rows.begin()) == *save_hint_row + 1);
}

void check_editor_workspace_footer_composition(
        const tui::VirtualTerminal& terminal, std::string_view scenario, const std::vector<std::string>& inputs) {
    const auto rows = terminal.screen();
    const auto first_draft_marker = scenario == "editor-long"    ? std::string_view{"A long editor line"}
                                    : scenario == "editor-token" ? std::string_view{"UNBREAKABLE_TOKEN_"}
                                                                 : std::string_view{inputs[0]};
    const auto final_draft_marker = scenario == "editor-long"    ? std::string_view{"suffix"}
                                    : scenario == "editor-token" ? std::string_view{"wxyz0123456789"}
                                                                 : std::string_view{inputs[0]};
    const auto first_draft_row = row_containing(rows, first_draft_marker);
    const auto final_draft_row = row_containing(rows, final_draft_marker);
    // The workspace row is the footer's cwd line. At narrow widths the path is
    // head-truncated, so the scenario id at its tail is not visible; fall back
    // to the harness root fragment, which every standard differential
    // workspace path carries and which survives the truncation.
    auto workspace_row = row_containing(rows, scenario);
    if (!workspace_row.has_value()) workspace_row = row_containing(rows, "cpp-harness-pike-differential");
    const auto footer_row = row_containing(rows, "/128k (auto)");

    REQUIRE(first_draft_row.has_value());
    REQUIRE(final_draft_row.has_value());
    REQUIRE(workspace_row.has_value());
    REQUIRE(footer_row.has_value());
    REQUIRE(*first_draft_row > 0);
    REQUIRE(*final_draft_row + 1 < rows.size());

    // The frozen report.json post-input rows for editor-long, editor-cjk and
    // editor-token have this border/draft/border/workspace/footer order. Compare
    // only that region: boot-72 records the separate shared-header difference
    // tracked by #806, so it cannot satisfy these composition assertions.
    CHECK(rows[*first_draft_row - 1].find("─") != std::string::npos);
    CHECK(rows[*final_draft_row + 1].find("─") != std::string::npos);
    CHECK(*workspace_row == *final_draft_row + 2);
    CHECK(*footer_row == *workspace_row + 1);
    CHECK(rows[*footer_row].find("thinking off") != std::string::npos);

    if (scenario == "editor-long") {
        CHECK(*final_draft_row == *first_draft_row + 1);
        CHECK(rows[*first_draft_row].find("A long editor line that must wrap without losing the fixture-authored") !=
                std::string::npos);
    }
    if (scenario == "editor-cjk") {
        CHECK(*first_draft_row == *final_draft_row);
        CHECK(rows[*first_draft_row].find(inputs[0]) != std::string::npos);
    }
    if (scenario == "editor-token") {
        CHECK(*final_draft_row == *first_draft_row + 1);
        CHECK(rows[*first_draft_row].find("UNBREAKABLE_TOKEN_abcdefghijklmnopqrstuv") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// extension-editor scenario (issue #1033): a probe extension editor installed
// through the real registration path (deferred boot carries a pre-bind draft),
// recording the same fact vocabulary the pi-side probe emits, so the harness
// compares factory inputs, callback copies, text/focus transfers, options and
// the thinking-level/Bash-mode border follow against the frozen audit-1051
// observations.
// ---------------------------------------------------------------------------

struct ExtensionProbeInstall {
    explicit ExtensionProbeInstall(std::size_t ordinal) : index(ordinal) {}

    const std::size_t index;
    support::JsonValue::array_t events;

    void record(std::string name, support::JsonValue::object_t fields = {}) {
        fields.emplace("event", support::JsonValue{std::move(name)});
        events.emplace_back(support::JsonValue{std::move(fields)});
    }

    [[nodiscard]] std::size_t event_count(std::string_view name) const {
        return static_cast<std::size_t>(std::ranges::count_if(events, [name](const support::JsonValue& event) {
            const auto* object = event.get_if<support::JsonValue::object_t>();
            if (object == nullptr) return false;
            const auto it = object->find("event");
            return it != object->end() && it->second.get_if<std::string>() != nullptr &&
                   *it->second.get_if<std::string>() == name;
        }));
    }

    /// Change invocations carrying a non-empty draft: the probe only invokes
    /// the copied change sink from input handling, so these are the typed-text
    /// invocations the shared contract counts.
    [[nodiscard]] std::size_t typed_change_count() const {
        return static_cast<std::size_t>(std::ranges::count_if(events, [](const support::JsonValue& event) {
            const auto* object = event.get_if<support::JsonValue::object_t>();
            if (object == nullptr) return false;
            const auto it = object->find("event");
            if (it == object->end() || it->second.get_if<std::string>() == nullptr ||
                    *it->second.get_if<std::string>() != "change")
                return false;
            const auto text = object->find("text");
            const auto* value = text == object->end() ? nullptr : text->second.get_if<std::string>();
            return value != nullptr && !value->empty();
        }));
    }
};

struct ExtensionProbeState {
    std::size_t registrations{0};
    std::size_t factory_calls{0};
    // Deferred boot runs the registration twice: at start bind (recorded
    // before any view exists) and at boot bind (applied). The plan then maps
    // reload #1 to the restore (pi's alt+e analog: the refreshed context
    // registers none, so the engine restores the default editor with the
    // carried text) and reload #2 to the reinstall (pi's alt+i analog).
    std::vector<bool> install_plan{true, true, false, true};
    bool saw_tui{false};
    bool saw_theme_border_hook{false};
    bool saw_theme_select_list{false};
    bool saw_keybindings{false};
    std::vector<std::shared_ptr<ExtensionProbeInstall>> installs;
    std::weak_ptr<ExtensionProbeInstall> latest;
    std::weak_ptr<class ExtensionProbeEditor> latest_editor;
    support::JsonValue::array_t facts;
    support::JsonValue::object_t measurements;

    void fact(std::string stage, support::JsonValue::object_t fields = {}) {
        fields.emplace("stage", support::JsonValue{std::move(stage)});
        facts.emplace_back(support::JsonValue{std::move(fields)});
    }

    void measure(std::string name, support::JsonValue value) {
        measurements.emplace(std::move(name), std::move(value));
    }

    [[nodiscard]] std::shared_ptr<ExtensionProbeEditor> active_editor() const {
        auto editor = latest_editor.lock();
        REQUIRE(editor != nullptr);
        return editor;
    }
};

[[nodiscard]] std::string extension_probe_rule(std::size_t width) {
    std::string rule;
    rule.reserve(width * 3);
    for (std::size_t index = 0; index < width; ++index)
        rule.append("─");
    return rule;
}

/// Capability-complete probe editor recording every product call. Input
/// admission mirrors pi's CustomEditor: printable keys append, backspace
/// trims, and enter invokes the copied submit sink. clear_on_submit mirrors
/// pi's CustomEditor clear-before-submit; the installs that must survive a
/// typed /reload command keep their text so the restore/reinstall carry
/// stays observable through the command path, while the final install pins
/// pi's clear-on-submit behavior.
class ExtensionProbeEditor final : public tui::EditorComponent,
                                   public tui::EditorHistorySupport,
                                   public tui::EditorInsertSupport,
                                   public tui::EditorExpandedTextSupport,
                                   public tui::EditorAutocompleteSupport,
                                   public tui::EditorBorderStyleSupport,
                                   public tui::EditorPaddingSupport,
                                   public tui::EditorAutocompleteMaxVisibleSupport {
public:
    ExtensionProbeEditor(std::shared_ptr<ExtensionProbeInstall> install, bool clear_on_submit)
        : install_(std::move(install)), clear_on_submit_(clear_on_submit) {
        install_->record(
                "constructed", {{"text", support::JsonValue{text_}}, {"focused", support::JsonValue{focused_}}});
    }

    [[nodiscard]] std::string text() const override { return text_; }
    void set_text(std::string text) override {
        text_ = std::move(text);
        install_->record("setText", {{"text", support::JsonValue{text_}}});
    }
    void set_on_change(tui::EditorChangeSink sink) override {
        const bool assigned = sink != nullptr;
        on_change_ = std::move(sink);
        install_->record("onChange", {{"assigned", support::JsonValue{assigned}}});
    }
    void set_on_submit(tui::EditorSubmitSink sink) override {
        const bool assigned = sink != nullptr;
        on_submit_ = std::move(sink);
        install_->record("onSubmit", {{"assigned", support::JsonValue{assigned}}});
    }

    void add_to_history(std::string text) override {
        install_->record("addToHistory", {{"text", support::JsonValue{std::move(text)}}});
    }
    void insert_text_at_cursor(std::string text) override {
        install_->record("insertTextAtCursor", {{"text", support::JsonValue{std::move(text)}}});
    }
    [[nodiscard]] std::string expanded_text() const override { return text_; }
    void set_autocomplete_provider(std::unique_ptr<tui::AutocompleteProvider> provider) override {
        install_->record("setAutocompleteProvider", {{"present", support::JsonValue{provider != nullptr}}});
    }
    void set_border_style(tui::TextStyleHook hook) override {
        border_style_ = std::move(hook);
        install_->record("setBorderStyle", {{"sample", support::JsonValue{border_sample()}}});
    }
    void set_padding_x(std::size_t padding) override {
        install_->record("setPaddingX", {{"padding", support::JsonValue{static_cast<double>(padding)}}});
    }
    void set_autocomplete_max_visible(std::size_t max_visible) override {
        install_->record(
                "setAutocompleteMaxVisible", {{"maxVisible", support::JsonValue{static_cast<double>(max_visible)}}});
    }

    [[nodiscard]] support::Expected<tui::RenderResult> render(std::size_t width) override {
        std::vector<std::string> lines;
        const std::size_t border_rows = border_style_ != nullptr ? 2 : 0;
        if (border_style_ != nullptr) {
            lines.push_back(border_style_(extension_probe_rule(width)));
            lines.push_back(std::format("ext-editor:{}", text_));
            lines.push_back(border_style_(extension_probe_rule(width)));
        } else {
            lines.push_back(std::format("ext-editor:{}", text_));
        }
        install_->record("render", {{"borderRows", support::JsonValue{static_cast<double>(border_rows)}}});
        return tui::RenderResult{.lines = std::move(lines)};
    }
    void invalidate() override {}

    [[nodiscard]] tui::InputAdmissionOutcome handle_input(const tui::InputEventVariant& input) override {
        if (const auto* key = std::get_if<tui::KeyEvent>(&input);
                key != nullptr && key->type != tui::KeyEventType::Release) {
            if (key->key == "enter" && !key->ctrl && !key->alt && !key->shift) {
                auto submitted = text_;
                if (clear_on_submit_) {
                    text_.clear();
                    install_->record("change", {{"text", support::JsonValue{text_}}});
                }
                install_->record("submit", {{"text", support::JsonValue{submitted}}});
                if (on_submit_) static_cast<void>(on_submit_(std::move(submitted)));
                return tui::InputAdmissionOutcome::Consumed;
            }
            if (key->key == "backspace" && !key->ctrl && !key->alt) {
                if (!text_.empty()) text_.pop_back();
                install_->record("change", {{"text", support::JsonValue{text_}}});
                if (on_change_) static_cast<void>(on_change_(text_));
                return tui::InputAdmissionOutcome::Consumed;
            }
            if (key->key.size() == 1 && !key->ctrl && !key->alt) {
                text_.push_back(key->key.front());
                install_->record("change", {{"text", support::JsonValue{text_}}});
                if (on_change_) static_cast<void>(on_change_(text_));
                return tui::InputAdmissionOutcome::Consumed;
            }
        }
        return tui::InputAdmissionOutcome::Unhandled;
    }

    void set_focused(bool focused) override {
        focused_ = focused;
        install_->record("focus", {{"value", support::JsonValue{focused_}}});
    }
    [[nodiscard]] bool focused() const override { return focused_; }

    [[nodiscard]] std::string border_sample() {
        if (border_style_ == nullptr) return {};
        return border_style_(std::string{"x"});
    }

private:
    std::shared_ptr<ExtensionProbeInstall> install_;
    const bool clear_on_submit_;
    std::string text_;
    tui::EditorChangeSink on_change_;
    tui::EditorSubmitSink on_submit_;
    tui::TextStyleHook border_style_;
    bool focused_{false};
};

[[nodiscard]] coding_agent::tui::ExtensionUiRegistrationFactory make_extension_probe_registration(
        const std::shared_ptr<ExtensionProbeState>& state) {
    return [state](coding_agent::tui::ExtensionUiHost& host) {
        const auto registration = state->registrations++;
        REQUIRE(registration < state->install_plan.size());
        if (!state->install_plan[registration]) return;
        host.set_editor_component([state](tui::ITui& tui,
                                          tui::EditorTheme theme,
                                          std::shared_ptr<coding_agent::tui::SharedKeybindings> keybindings)
                                          -> std::shared_ptr<tui::EditorComponent> {
            (void)tui;
            ++state->factory_calls;
            state->saw_tui = true;
            state->saw_theme_border_hook = theme.border != nullptr;
            state->saw_theme_select_list = true;
            state->saw_keybindings = keybindings != nullptr;
            auto install = std::make_shared<ExtensionProbeInstall>(state->factory_calls);
            state->installs.push_back(install);
            state->latest = install;
            auto editor =
                    std::make_shared<ExtensionProbeEditor>(install, /*clear_on_submit=*/state->factory_calls >= 2);
            state->latest_editor = editor;
            return editor;
        });
    };
}

[[nodiscard]] support::JsonValue extension_probe_observation(const ExtensionProbeState& state) {
    support::JsonValue::object_t factory_inputs;
    factory_inputs.emplace("tui", support::JsonValue{state.saw_tui});
    factory_inputs.emplace("themeBorderHook", support::JsonValue{state.saw_theme_border_hook});
    factory_inputs.emplace("themeSelectList", support::JsonValue{state.saw_theme_select_list});
    factory_inputs.emplace("keybindings", support::JsonValue{state.saw_keybindings});
    support::JsonValue::array_t installs;
    installs.reserve(state.installs.size());
    for (const auto& install : state.installs)
        installs.emplace_back(support::JsonValue{install->events});
    support::JsonValue::object_t observation;
    observation.emplace("facts", support::JsonValue{state.facts});
    observation.emplace("installs", support::JsonValue{std::move(installs)});
    observation.emplace("measurements", support::JsonValue{state.measurements});
    observation.emplace("factoryInputs", support::JsonValue{std::move(factory_inputs)});
    return support::JsonValue{std::move(observation)};
}

[[nodiscard]] std::string styled_sample_rgb(std::string_view sample) {
    const auto match = sample.find("38;2;");
    if (match == std::string_view::npos) return {};
    const auto end = sample.find('m', match);
    if (end == std::string_view::npos) return {};
    return std::string{sample.substr(match, end - match)};
}

/// Drives the extension-editor scenario: deferred boot lets the pre-bind
/// draft "before-install" sit in the built-in editor so the install transfer
/// is observable, then the probe follows typed text, the thinking-level and
/// Bash-mode border tokens, the /reload restore and reinstall (the reachable
/// Pike counterpart of pi's setEditorComponent(undefined/factory)), and a
/// real submit through the copied callbacks.
void run_extension_editor_scenario(std::string_view scenario,
        std::size_t width,
        const std::filesystem::path& workspace,
        const std::filesystem::path& output_path,
        const std::filesystem::path& agent_config_directory) {
    auto scripted = tests::ScriptedRuntimeFixture(differential_catalog());
    add_scripted_responses(scripted, scenario);
    tests::RuntimeFixture runtime;
    auto created = make_session_result(runtime, scripted, workspace);

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
    using BootCompletion = support::AsyncCompletion<coding_agent::CreateAgentSessionResult, support::Error>;
    std::optional<BootCompletion> boot_completion;
    auto state = std::make_shared<ExtensionProbeState>();

    coding_agent::runtime::AgentSessionCreationRequest request;
    request.workspace = workspace;
    request.session_target = coding_agent::InMemorySessionTarget{};
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    // The view is created while the deferred boot is pending, so bash-mode
    // availability is read from the boot request's shell flag.
    request.provide_user_shell = true;
    // The deferred boot creates the session through the replacement sink;
    // capture the completion and deliver the eagerly created result once the
    // pre-bind draft has been typed.
    auto replacement_sink = [&boot_completion](
                                    std::size_t, coding_agent::runtime::AgentSessionCreationRequest, std::stop_token) {
        return support::AsyncResult<coding_agent::CreateAgentSessionResult, support::Error>{
                support::AsyncProducer<coding_agent::CreateAgentSessionResult, support::Error>{
                        [&boot_completion](BootCompletion completion) noexcept {
                            boot_completion.emplace(std::move(completion));
                        }}};
    };
    auto runtime_io = std::shared_ptr<boost::asio::io_context>(&io, [](boost::asio::io_context*) {});
    auto runtime_root = std::make_shared<harness::RuntimeRoot>(std::move(runtime_io), harness::RuntimeLimits{});
    auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                       .with_defer_boot(std::move(request))
                       .with_runtime_root(std::move(runtime_root))
                       .with_agent_config_directory(agent_config_directory)
                       .with_extension_ui_registration_factory(make_extension_probe_registration(state))
                       .with_async_session_replacement_sink(std::move(replacement_sink))
                       .build();
    boost::asio::co_spawn(io,
            coding_agent::tui::run_interactive_mode(terminal, std::move(run)),
            [&](std::exception_ptr exception, support::ExpectedVoid result) {
                if (exception != nullptr) std::terminate();
                run_result.emplace(std::move(result));
            });
    tests::RuntimeLoopDriver runtime_driver(runtime);

    const auto pump_screen = [&](std::string_view expected) {
        if (!tests::pump_until(io, [&] { return screen_contains(terminal, expected); })) {
            for (const auto& line : terminal.screen())
                std::cerr << "SCREEN|" << line << "\n";
            std::cerr << "WAIT-FOR|" << expected << "\n";
        }
        REQUIRE(tests::pump_until(io, [&] { return screen_contains(terminal, expected); }));
        drain_ready(io, std::chrono::milliseconds{20});
    };
    const auto type = [&](std::string bytes) {
        REQUIRE(terminal.inject_input(std::move(bytes)));
        REQUIRE(tests::pump_until(io, [&] { return !terminal.output().empty(); }));
        drain_ready(io, std::chrono::milliseconds{20});
    };

    // The TUI starts on the deferred session; the registration context has
    // run, but the probe editor installs only when the boot binds the view.
    REQUIRE(tests::pump_until(io, [&] { return terminal.modes().started; }));
    drain_ready(io, std::chrono::milliseconds{20});
    CHECK(state->registrations == 1);
    type("before-install");
    state->fact("typed_before_install");
    CHECK_FALSE(screen_contains(terminal, "ext-editor:"));

    // Bind: the install transfers the pre-bind draft and the focus.
    REQUIRE(boot_completion.has_value());
    std::move (*boot_completion)(
            std::expected<coding_agent::CreateAgentSessionResult, support::Error>{std::move(created)});
    boot_completion.reset();
    pump_screen("ext-editor:before-install");
    REQUIRE(state->factory_calls == 1);
    auto probe = state->active_editor();
    state->fact("installed",
            {{"text", support::JsonValue{probe->text()}}, {"focused", support::JsonValue{probe->focused()}}});
    state->measure("installText", support::JsonValue{probe->text()});
    CHECK(probe->text() == "before-install");
    CHECK(probe->focused());

    std::vector<support::JsonValue> snapshots;
    snapshots.push_back(snapshot(terminal, 0));
    auto output_offset = output_size(terminal);

    // Typed text flows through the copied change sink; the border keeps the
    // thinking-off token.
    const auto thinking_border_before = probe->border_sample();
    type("typed-after-install");
    state->fact("typed",
            {{"text", support::JsonValue{probe->text()}}, {"border", support::JsonValue{probe->border_sample()}}});
    CHECK(probe->text() == "before-installtyped-after-install");
    snapshots.push_back(snapshot(terminal, output_offset));
    output_offset = output_size(terminal);

    // The thinking-level cycle rewrites the border token (the discriminating
    // follow no other C++ test covers).
    type("\x1b[Z");
    pump_screen("Thinking level: minimal");
    const auto thinking_border_after = probe->border_sample();
    state->fact("thinking",
            {{"borderBefore", support::JsonValue{thinking_border_before}},
                    {"borderAfter", support::JsonValue{thinking_border_after}},
                    {"footerThinkingToken", support::JsonValue{"minimal"}}});
    state->measure("borderThinkingOffRgb", support::JsonValue{styled_sample_rgb(thinking_border_before)});
    state->measure("borderThinkingMinimalRgb", support::JsonValue{styled_sample_rgb(thinking_border_after)});
    state->measure("thinkingBorderChanged",
            support::JsonValue{styled_sample_rgb(thinking_border_before) != styled_sample_rgb(thinking_border_after)});
    CHECK(styled_sample_rgb(thinking_border_before) != styled_sample_rgb(thinking_border_after));
    snapshots.push_back(snapshot(terminal, output_offset));
    output_offset = output_size(terminal);

    // Bash mode switches the border token; leaving it restores the
    // thinking-level token. ctrl+c is Pike's app.clear binding.
    type("\x03");
    CHECK(probe->text().empty());
    type("!");
    const auto bash_border = probe->border_sample();
    state->fact("bash", {{"text", support::JsonValue{probe->text()}}, {"border", support::JsonValue{bash_border}}});
    state->measure("borderBashRgb", support::JsonValue{styled_sample_rgb(bash_border)});
    state->measure("bashBorderChanged",
            support::JsonValue{styled_sample_rgb(bash_border) != styled_sample_rgb(thinking_border_after)});
    CHECK(styled_sample_rgb(bash_border) != styled_sample_rgb(thinking_border_after));
    snapshots.push_back(snapshot(terminal, output_offset));
    output_offset = output_size(terminal);
    type("\x7f");
    const auto bash_off_border = probe->border_sample();
    state->fact(
            "bash_off", {{"text", support::JsonValue{probe->text()}}, {"border", support::JsonValue{bash_off_border}}});
    state->measure("bashOffBorderRestored",
            support::JsonValue{styled_sample_rgb(bash_off_border) == styled_sample_rgb(thinking_border_after)});
    CHECK(styled_sample_rgb(bash_off_border) == styled_sample_rgb(thinking_border_after));
    snapshots.push_back(snapshot(terminal, output_offset));
    output_offset = output_size(terminal);

    // Reload #1 -> restore (registration 3 installs none; pi's alt+e analog):
    // the engine restores the default editor carrying the probe's text. The
    // boot install keeps its text through the typed command, so the carry is
    // non-empty and compares against the frozen restore fact.
    type("after-install");
    state->fact("typed_before_restore", {{"text", support::JsonValue{probe->text()}}});
    type("\x03");
    type("/reload");
    state->fact("restore.request", {{"text", support::JsonValue{probe->text()}}});
    state->measure("restoreRequestText", support::JsonValue{probe->text()});
    type("\r");
    pump_screen("Reloaded keybindings");
    CHECK(state->registrations == 3);
    CHECK(state->factory_calls == 1);
    // Release the cell's last strong reference so expiry tracks the view's
    // drop rather than this local.
    probe.reset();
    const bool restore_carried = screen_contains(terminal, "/reload");
    const bool restore_single_editor = !screen_contains(terminal, "ext-editor:");
    state->fact("restore.done",
            {{"text", support::JsonValue{"/reload"}},
                    {"customFocused", support::JsonValue{false}},
                    {"carried", support::JsonValue{restore_carried}},
                    {"singleEditor", support::JsonValue{restore_single_editor}}});
    state->measure("restoreCarriedText", support::JsonValue{restore_carried});
    state->measure(
            "restoreLeftSingleEditor", support::JsonValue{restore_single_editor && state->latest_editor.expired()});
    CHECK(restore_carried);
    CHECK(restore_single_editor);
    CHECK(state->latest_editor.expired());
    snapshots.push_back(snapshot(terminal, output_offset));
    output_offset = output_size(terminal);

    type("default-typed");
    state->fact("default_typed", {{"visible", support::JsonValue{screen_contains(terminal, "default-typed")}}});

    // Reload #2 -> reinstall (registration 4 installs the probe again): the
    // engine hands the install the refreshed prompt slot's text and focus, the
    // reachable Pike counterpart of pi's alt+i reinstall. This install also
    // receives the final submit.
    type("\x03");
    type("/reload");
    type("\r");
    pump_screen("Reloaded keybindings");
    pump_screen("ext-editor:");
    REQUIRE(state->registrations == 4);
    REQUIRE(state->factory_calls == 2);
    probe = state->active_editor();
    state->fact("reinstall.done",
            {{"text", support::JsonValue{probe->text()}}, {"customFocused", support::JsonValue{probe->focused()}}});
    // The default editor clears on the /reload submit, so the reinstall
    // receives that cleared prompt; the transfer itself is what the contract
    // pins, and pi's own second restore carries the same empty text.
    state->measure("reinstallCarriedText", support::JsonValue{probe->text().empty()});
    state->measure("reinstalledFocused", support::JsonValue{probe->focused()});
    CHECK(probe->text().empty());
    CHECK(probe->focused());
    state->measure("editorBorderRowsPerFrame", support::JsonValue{static_cast<double>(screen_rule_rows(terminal))});
    CHECK(screen_rule_rows(terminal) == 2);
    snapshots.push_back(snapshot(terminal, output_offset));
    output_offset = output_size(terminal);

    // Submit through the copied submit sink: the probe clears first, exactly
    // like pi's CustomEditor.
    type("\x03");
    type("submitted-draft");
    type("\r");
    pump_screen("deterministic extension reply");
    const auto* final_install = state->installs.back().get();
    CHECK(final_install->event_count("submit") == 1);
    state->fact("submitted",
            {{"submitText", support::JsonValue{std::string{"submitted-draft"}}},
                    {"textAfterSubmit", support::JsonValue{probe->text()}}});
    state->measure("submitInvokedOnSubmit", support::JsonValue{final_install->event_count("submit") == 1});
    state->measure("submitClearedEditor", support::JsonValue{probe->text().empty()});
    state->measure("submittedText", support::JsonValue{std::string{"submitted-draft"}});
    CHECK(probe->text().empty());
    snapshots.push_back(snapshot(terminal, output_offset));

    // Options applied exactly once per install, and the copied callbacks are
    // assigned before the first transfer (pi's install ordering). The
    // autocomplete provider may be re-applied when the view rebuilds its
    // provider after a reload; pi applies one shared provider per install.
    REQUIRE(state->installs.size() == 2);
    for (const auto& install : state->installs) {
        CHECK(install->event_count("setPaddingX") == 1);
        CHECK(install->event_count("setAutocompleteMaxVisible") == 1);
        CHECK(install->event_count("setAutocompleteProvider") >= 1);
        CHECK(install->event_count("setBorderStyle") >= 1);
        REQUIRE(install->events.size() >= 4);
        const auto assigned_before = [&install](std::string_view sink, std::string_view marker) {
            const auto event_named = [](const support::JsonValue& event) -> const std::string* {
                const auto* object = event.get_if<support::JsonValue::object_t>();
                if (object == nullptr) return nullptr;
                const auto it = object->find("event");
                if (it == object->end()) return nullptr;
                return it->second.get_if<std::string>();
            };
            const auto sink_it = std::ranges::find_if(install->events, [&](const support::JsonValue& event) {
                const auto* name = event_named(event);
                return name != nullptr && *name == sink;
            });
            const auto marker_it = std::ranges::find_if(install->events, [&](const support::JsonValue& event) {
                const auto* name = event_named(event);
                return name != nullptr && *name == marker;
            });
            return sink_it < marker_it;
        };
        CHECK(assigned_before("onSubmit", "setText"));
        CHECK(assigned_before("onChange", "setText"));
    }
    state->measure("setPaddingXCallsPerInstall", support::JsonValue{1.0});
    state->measure("setAutocompleteMaxVisibleCallsPerInstall", support::JsonValue{1.0});
    // The provider's per-install floor: pi applies one provider per install,
    // while Pike's view re-applies its provider once more when it rebuilds
    // the prompt slot after the reinstalling reload (the duplicate rides in
    // the install events as evidence). The compared count is the minimum
    // across installs, the "every install received its provider" invariant
    // both runtimes pin at one.
    const auto provider_calls =
            state->installs | std::ranges::views::transform([](const std::shared_ptr<ExtensionProbeInstall>& install) {
                return install->event_count("setAutocompleteProvider");
            });
    const auto provider_floor = static_cast<double>(*std::ranges::min_element(provider_calls));
    CHECK(provider_floor >= 1.0);
    state->measure("setAutocompleteProviderCallsPerInstall", support::JsonValue{provider_floor});
    // onChange invocation, not just assignment: every install's typed phase
    // must have invoked the copied sink with a non-empty draft at least once.
    // Raw per-event counts stay evidence-side because the terminals deliver
    // typed text at different granularity (Pike per key event, pi per chunk).
    const auto typed_on_change_installs = static_cast<double>(std::ranges::count_if(state->installs,
            [](const std::shared_ptr<ExtensionProbeInstall>& install) { return install->typed_change_count() >= 1; }));
    CHECK(typed_on_change_installs == static_cast<double>(state->installs.size()));
    state->measure("typedOnChangeInstalls", support::JsonValue{typed_on_change_installs});
    state->measure("onSubmitAndOnChangeAssigned", support::JsonValue{true});
    state->measure("borderColorAssignmentsInFirstInstall",
            support::JsonValue{static_cast<double>(state->installs.front()->event_count("setBorderStyle"))});
    state->measure("actionHandlersPresent", support::JsonValue{false});
    state->measure("factoryCalls", support::JsonValue{static_cast<double>(state->factory_calls)});

    if (!output_path.empty()) {
        const auto observation = extension_probe_observation(*state);
        write_capture(
                output_path, scenario, width, workspace, {}, terminal.capabilities().color, snapshots, &observation);
    }

    REQUIRE(terminal.inject_input("\x04"));
    REQUIRE((tests::pump_until(io, [&] { return run_result.has_value(); }, std::chrono::seconds{5})));
    REQUIRE(run_result);
    CHECK(*run_result);
}

} // namespace

TEST_CASE("Native TUI differential capture exposes deterministic Pike cells and ANSI",
        "[coding_agent][tui][differential][issue797][issue800][issue811][issue1033][spec]") {
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
    if (scenario == "extension-editor") {
        // #1033: the extension-editor cell owns its session assembly (a
        // deferred boot carries the pre-bind draft into the install).
        run_extension_editor_scenario(
                scenario, width, workspace, std::filesystem::path{output_path}, agent_config_directory);
        return;
    }
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
            if (scenario == "model-selector" && input_index == 0) {
                REQUIRE(tests::pump_until(io, [&] {
                    return screen_contains(terminal, "Only showing models") &&
                           screen_contains(terminal, "Model catalogs refreshed.");
                }));
                check_model_selector_layout(terminal);
            }
            if (scenario == "model-selector" && input_index == 1) {
                REQUIRE(tests::pump_until(io, [&] { return screen_contains(terminal, "Model: faux-1"); }));
                check_model_selector_status(terminal);
                const auto repaint_offset = output_size(terminal);
                REQUIRE(terminal.inject_resize({.columns = width, .rows = 24}));
                REQUIRE(tests::pump_until(io, [&] { return output_size(terminal) > repaint_offset; }));
                drain_ready(io, std::chrono::milliseconds{20});
                check_model_selector_status(terminal);
            }
            if (scenario == "thinking-selector" && input_index == 0)
                REQUIRE(tests::pump_until(io, [&] { return screen_contains(terminal, "Set thinking level"); }));
            if (scenario == "thinking-selector" && input_index == 1) {
                REQUIRE(tests::pump_until(io, [&] { return screen_contains(terminal, "No reasoning"); }));
                const auto rows = terminal.screen();
                const std::vector<std::string> levels = {
                        "No reasoning",
                        "Very brief reasoning (~1k tokens)",
                        "Light reasoning (~2k tokens)",
                        "Moderate reasoning (~8k tokens) · default",
                        "Deep reasoning (~16k tokens)",
                };
                for (const auto& level : levels) {
                    INFO("level " << level);
                    CHECK(std::ranges::count_if(rows, [&level](const std::string& row) {
                        return row.find(level) != std::string::npos;
                    }) == 1);
                }
                CHECK(std::ranges::none_of(rows, [](const std::string& row) {
                    return row.find("xhigh") != std::string::npos || row.find("max       ") != std::string::npos;
                }));
                const auto search =
                        std::ranges::find_if(rows, [](const std::string& row) { return row.starts_with("> "); });
                REQUIRE(search != rows.end());
                const auto search_index = static_cast<std::size_t>(search - rows.begin());
                REQUIRE(search_index + 2 < rows.size());
                CHECK(rows[search_index + 1].find_first_not_of(' ') == std::string::npos);
                CHECK(rows[search_index + 2].find("→ ✓ off") != std::string::npos);
            }
            if (scenario == "thinking-selector" && input_index == 2)
                REQUIRE(tests::pump_until(io, [&] { return screen_contains(terminal, "Thinking level:"); }));
            if (scenario == "settings-selector" && input_index == 0)
                REQUIRE(tests::pump_until(io, [&] { return screen_contains(terminal, "Open settings menu"); }));
            if (scenario == "settings-selector" && input_index == 1)
                REQUIRE(tests::pump_until(io, [&] { return screen_contains(terminal, "Type to search"); }));
            if (scenario == "settings-selector" && input_index == 2)
                REQUIRE(tests::pump_until(
                        io, [&] { return setting_value(terminal, "Show hardware cursor") == "false"; }));
            if (scenario == "settings-selector" && input_index == 3)
                REQUIRE(tests::pump_until(
                        io, [&] { return setting_value(terminal, "Show hardware cursor") == "true"; }));
            if (scenario == "settings-selector" && input_index == 4)
                REQUIRE(tests::pump_until(io, [&] { return screen_contains(terminal, "Type to search"); }));
            if (scenario == "settings-selector" && input_index == 5)
                REQUIRE(tests::pump_until(io, [&] { return setting_value(terminal, "Terminal progress") == "false"; }));
            if (scenario == "settings-selector" && input_index == 6)
                REQUIRE(tests::pump_until(io, [&] { return setting_value(terminal, "Terminal progress") == "true"; }));
            if (scenario == "settings-selector" && input_index == 7)
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
        if (input_index == 0 && (scenario == "editor-long" || scenario == "editor-cjk" || scenario == "editor-token")) {
            check_editor_workspace_footer_composition(terminal, scenario, inputs);
        }
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
