// The product `outputPad` fan-out (B63 / TPadding, #1121).
//
// pi's `outputPad` render setting reaches every chat block pi renders, not
// only the user/assistant messages: the tool block's box padding
// (`tool-execution.ts:207,283`), the bash block's header/body/status text
// padding (`bash-execution.ts:73-77,141,149,205`) and the custom/boxed
// block's box padding (`custom-message.ts:48-51,90`). The frozen pi-v1.3.0
// observation is the `audit-1121` capture in
// fixtures/pi-v1.3.0/audit-1121/output-pad.json, scenarios
// `output-pad-reaches-the-tool-block-box`,
// `output-pad-reaches-the-bash-block-text` and
// `output-pad-reaches-the-custom-block-box` (revision 71023aec1546).
//
// The DISCRIMINATING case is the tool block: a user-message-only
// implementation (the previous `apply_output_pad_to_components`) passes the
// user/assistant control and fails the tool, bash and custom blocks.
//
// pi's `onOutputPadChange` also reaches `skill-invocation-message.ts` and
// `custom-entry.ts`; the C++ tree has no counterpart for either, so those two
// blocks do not render here and their `setOutputPad` contract is carried by
// the missing components, not by this fan-out.
//
// The artifact stays UNREGISTERED (the serial merger applies the registry
// patch), so the expected values are read from the committed capture bytes
// rather than from a verified registry read.

#include "coding_agent/tui/ChatContainer.hpp"
#include "coding_agent/tui/SharedKeybindings.hpp"
#include "coding_agent/tui/Theme.hpp"
#include "support/RenderedScreen.hpp"

#include <cch/agent/AgentEvent.hpp>
#include <cch/ai/Message.hpp>
#include <cch/support/JsonValue.hpp>
#include <cch/tui/Keybindings.hpp>
#include "support/Json.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] std::shared_ptr<const tui::KeybindingRegistry> test_keybindings() {
    tui::KeybindingResolutionRequest request;
    request.definitions = tui::builtin_tui_keybinding_definitions();
    auto resolved = tui::resolve_keybindings(std::move(request));
    REQUIRE(resolved);
    return resolved->registry;
}

[[nodiscard]] std::shared_ptr<coding_agent::tui::SharedKeybindings> test_keybinding_slot() {
    return std::make_shared<coding_agent::tui::SharedKeybindings>(test_keybindings());
}

[[nodiscard]] coding_agent::tui::LiveTheme test_theme() {
    return coding_agent::tui::LiveTheme(
            coding_agent::tui::builtin_dark_theme(), tui::TerminalColorCapability::TrueColor);
}

/// The frozen pi-v1.3.0 rows of the new output-pad artifact. The bytes are the
/// committed capture; the C++ side never derives them.
[[nodiscard]] const support::JsonValue& output_pad_artifact() {
    static const auto artifact = [] {
        const std::string path = std::string{CCH_SOURCE_DIR} + "/fixtures/pi-v1.3.0/audit-1121/output-pad.json";
        std::ifstream input(path, std::ios::binary);
        REQUIRE(input);
        const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        auto parsed = support::read_json(text);
        REQUIRE(parsed.has_value());
        return *parsed;
    }();
    return artifact;
}

[[nodiscard]] const support::JsonValue& output_pad_scenario(std::string_view name) {
    const auto& artifact = output_pad_artifact();
    for (const auto& scenario : artifact.at("scenarios").get_array()) {
        if (scenario.at("name").get_string() == name) return scenario;
    }
    FAIL("missing frozen output-pad scenario: " << std::string{name});
    return artifact;
}

/// The leading spaces of the first rendered row that carries `needle`.
[[nodiscard]] std::size_t leading_spaces(const std::vector<std::string>& lines, std::string_view needle) {
    for (const auto& line : lines) {
        const auto visible = tests::strip_ansi(line);
        if (visible.find(needle) == std::string::npos) continue;
        const auto first = visible.find_first_not_of(' ');
        if (first == std::string::npos) continue;
        return first;
    }
    FAIL("no rendered row carries " << std::string{needle});
    return 0;
}

[[nodiscard]] std::vector<std::string> rendered_lines(coding_agent::tui::ChatContainer& chat, std::size_t width) {
    const auto rendered = chat.render(width);
    REQUIRE(rendered);
    return rendered->lines;
}

[[nodiscard]] std::vector<std::string> visible_rows(const std::vector<std::string>& lines) {
    std::vector<std::string> rows;
    rows.reserve(lines.size());
    for (const auto& line : lines)
        rows.push_back(tests::strip_ansi(line));
    return rows;
}

[[nodiscard]] std::vector<std::string> rows_between(
        const std::vector<std::string>& lines, std::string_view first, std::string_view last) {
    std::vector<std::string> rows;
    bool started = false;
    for (const auto& line : visible_rows(lines)) {
        if (!started && line.find(first) != std::string::npos) started = true;
        if (started) rows.push_back(line);
        if (started && line.find(last) != std::string::npos) return rows;
    }
    FAIL("no rendered rows run from \"" << std::string{first} << "\" through \"" << std::string{last} << "\"");
    return rows;
}

[[nodiscard]] std::string compact_text(const std::vector<std::string>& lines) {
    std::string result;
    for (const auto& line : visible_rows(lines)) {
        for (const auto cell : line) {
            if (cell != ' ') result.push_back(cell);
        }
    }
    return result;
}

/// One settled tool block in the transcript: an assistant message with a tool
/// call, the tool's start, and the tool's settled result.
void append_tool_block(coding_agent::tui::ChatContainer& chat,
        std::string_view tool_name,
        std::string_view call_id,
        std::string_view command) {
    ai::AssistantMessage assistant;
    assistant.content.push_back(ai::ToolCallContent{
            .id = std::string{call_id},
            .name = std::string{tool_name},
            .raw_arguments = std::format(R"({{"command":"{}"}})", command),
    });
    assistant.stop_reason = ai::AssistantStopReason::ToolUse;
    chat.apply_event(agent::MessageStartEvent{.message = assistant});
    chat.apply_event(agent::MessageEndEvent{.message = assistant});
    chat.apply_event(agent::ToolExecutionStartEvent{
            .tool_call_id = std::string{call_id},
            .tool_name = std::string{tool_name},
            .args = support::JsonValue::object_t{{"command", std::string{command}}},
    });
    chat.apply_event(agent::ToolExecutionEndEvent{
            .tool_call_id = std::string{call_id},
            .tool_name = std::string{tool_name},
            .result =
                    agent::AsyncToolExecutionResult{
                            .content = {ai::TextContent{.text = "audit output"}},
                            .details = std::nullopt,
                            .is_error = false,
                    },
            .is_error = false,
    });
}

/// One User Bash block in the transcript.
void append_bash_block(coding_agent::tui::ChatContainer& chat, std::string_view command, std::string_view output) {
    ai::BashExecutionMessage message;
    message.command = std::string{command};
    message.output = std::string{output};
    message.exit_code = 0;
    chat.append_committed_message(ai::MessageVariant{message});
}

/// One custom (boxed) block in the transcript.
void append_custom_block(coding_agent::tui::ChatContainer& chat, std::string_view custom_type, std::string_view body) {
    ai::CustomMessage message;
    message.custom_type = std::string{custom_type};
    message.content = {ai::TextContent{.text = std::string{body}}};
    message.display = true;
    chat.append_committed_message(ai::MessageVariant{message});
}

} // namespace

TEST_CASE("ChatContainer output pad reaches the tool, bash and custom blocks", "[coding_agent][tui][issue1121][spec]") {
    const auto& tool_expected = output_pad_scenario("output-pad-reaches-the-tool-block-box").at("expected");
    const auto& bash_expected = output_pad_scenario("output-pad-reaches-the-bash-block-text").at("expected");
    const auto& custom_expected = output_pad_scenario("output-pad-reaches-the-custom-block-box").at("expected");

    // The frozen pi rows: the block's content row carries one leading space at
    // outputPad 1 and none at outputPad 0.
    REQUIRE(tool_expected.at("contentRowAtOneLeadingSpaces").get_number() == 1);
    REQUIRE(tool_expected.at("contentRowAtZeroLeadingSpaces").get_number() == 0);
    REQUIRE(bash_expected.at("headerRowAtOneLeadingSpaces").get_number() == 1);
    REQUIRE(bash_expected.at("headerRowAtZeroLeadingSpaces").get_number() == 0);
    REQUIRE(custom_expected.at("labelRowAtOneLeadingSpaces").get_number() == 1);
    REQUIRE(custom_expected.at("labelRowAtZeroLeadingSpaces").get_number() == 0);

    auto theme = test_theme();
    coding_agent::tui::ChatContainer chat(theme, test_keybinding_slot());
    std::vector<std::string> narrow_tool_rows;
    std::vector<std::string> wide_tool_rows;
    std::vector<std::string> narrow_bash_command_rows;
    std::vector<std::string> wide_bash_command_rows;
    std::vector<std::string> narrow_bash_output_rows;
    std::vector<std::string> wide_bash_output_rows;
    std::vector<std::string> narrow_custom_rows;
    std::vector<std::string> wide_custom_rows;
    append_tool_block(chat,
            "audit-tool",
            "call-audit-1051",
            "echo wrap-tool audit payload with enough words to continue onto another terminal row and test "
            "width-dependent wrapping");
    append_bash_block(chat,
            "echo wrap-bash audit command with enough words to continue onto another terminal row and test "
            "width-dependent wrapping",
            "wrap-bash output carries additional words so the body wraps at the narrow width and keeps measuring "
            "differently at every tested terminal width");
    append_custom_block(chat,
            "audit",
            "wrap-custom content carries additional words so this custom message wraps at the narrow width and keeps "
            "measuring differently at every tested terminal width");

    // The DISCRIMINATING case: the tool block. The previous implementation
    // rebuilt only the user/assistant messages, so this row kept the hard-coded
    // box padding at both pads.
    for (const std::size_t width : {32U, 60U, 100U}) {
        CAPTURE(width);
        const auto width_key = std::to_string(width);
        const auto& frozen_tool_width = tool_expected.at("atWidths").at(width_key);
        const auto& frozen_bash_width = bash_expected.at("atWidths").at(width_key);
        const auto& frozen_custom_width = custom_expected.at("atWidths").at(width_key);

        const auto at_one = rendered_lines(chat, width);
        CHECK(leading_spaces(at_one, "audit-tool") == 1);
        CHECK(leading_spaces(at_one, "$ echo wrap-bash") == 1);
        CHECK(leading_spaces(at_one, "[audit]") == 1);
        const auto actual_tool_one = rows_between(at_one, "audit-tool", "width-dependent wrapping");
        const auto actual_bash_command_one = rows_between(at_one, "$ echo wrap-bash", "width-dependent wrapping");
        const auto actual_bash_output_one = rows_between(at_one, "wrap-bash output", "every tested terminal width");
        const auto actual_custom_one = rows_between(at_one, "[audit]", "width");
        if (width == 32U) {
            narrow_tool_rows = actual_tool_one;
            narrow_bash_command_rows = actual_bash_command_one;
            narrow_bash_output_rows = actual_bash_output_one;
            narrow_custom_rows = actual_custom_one;
        } else if (width == 100U) {
            wide_tool_rows = actual_tool_one;
            wide_bash_command_rows = actual_bash_command_one;
            wide_bash_output_rows = actual_bash_output_one;
            wide_custom_rows = actual_custom_one;
        }
        CHECK(actual_tool_one.size() > 1);
        CHECK(actual_bash_command_one.size() > 1);
        CHECK(actual_bash_output_one.size() > 1);
        CHECK(actual_custom_one.size() > 1);
        CHECK(frozen_tool_width.at("atOne").get_array().size() ==
                static_cast<std::size_t>(frozen_tool_width.at("lineCountAtOne").get_number()));
        CHECK(frozen_bash_width.at("atOne").get_array().size() ==
                static_cast<std::size_t>(frozen_bash_width.at("lineCountAtOne").get_number()));
        CHECK(frozen_custom_width.at("atOne").get_array().size() ==
                static_cast<std::size_t>(frozen_custom_width.at("lineCountAtOne").get_number()));
        CHECK(tool_expected.at("atWidths").at("32").at("lineCountAtOne").get_number() >
                tool_expected.at("atWidths").at("100").at("lineCountAtOne").get_number());
        CHECK(bash_expected.at("atWidths").at("32").at("lineCountAtOne").get_number() >
                bash_expected.at("atWidths").at("100").at("lineCountAtOne").get_number());
        CHECK(custom_expected.at("atWidths").at("32").at("lineCountAtOne").get_number() >
                custom_expected.at("atWidths").at("100").at("lineCountAtOne").get_number());
        CHECK(tool_expected.at("wrappedCommandRowsAtNarrowWidth").get_array().size() > 1);
        CHECK(bash_expected.at("wrappedOutputRowsAtNarrowWidth").get_array().size() > 1);
        CHECK(custom_expected.at("wrappedContentRowsAtNarrowWidth").get_array().size() > 1);
        CHECK(compact_text(at_one).find(
                      "echowrap-toolauditpayloadwithenoughwordstocontinueontoanotherterminalrowandtestwidth-"
                      "dependentwrapping") != std::string::npos);
        CHECK(compact_text(at_one).find(
                      "echowrap-bashauditcommandwithenoughwordstocontinueontoanotherterminalrowandtestwidth-"
                      "dependentwrapping") != std::string::npos);
        CHECK(compact_text(at_one).find("wrap-bashoutputcarriesadditionalwordssothebodywraps atthenarrowwidth") ==
                std::string::npos);
        CHECK(compact_text(at_one).find("wrap-"
                                        "bashoutputcarriesadditionalwordssothebodywrapsatthenarrowwidthandkeepsmeasurin"
                                        "gdifferentlyateverytestedterminalwidth") != std::string::npos);
        CHECK(compact_text(at_one).find("wrap-"
                                        "customcontentcarriesadditionalwordssothiscustommessagewrapsatthenarrowwidthand"
                                        "keepsmeasuringdifferentlyateverytestedterminalwidth") != std::string::npos);

        chat.set_output_pad(0);
        const auto at_zero = rendered_lines(chat, width);
        CHECK(leading_spaces(at_zero, "audit-tool") == 0);
        CHECK(leading_spaces(at_zero, "$ echo wrap-bash") == 0);
        CHECK(leading_spaces(at_zero, "[audit]") == 0);
        const auto actual_tool_zero = rows_between(at_zero, "audit-tool", "width-dependent wrapping");
        const auto actual_bash_command_zero = rows_between(at_zero, "$ echo wrap-bash", "width-dependent wrapping");
        const auto actual_bash_output_zero = rows_between(at_zero, "wrap-bash output", "every tested terminal width");
        const auto actual_custom_zero = rows_between(at_zero, "[audit]", "width");
        CHECK(actual_tool_zero.size() > 1);
        CHECK(actual_bash_command_zero.size() > 1);
        CHECK(actual_bash_output_zero.size() > 1);
        CHECK(actual_custom_zero.size() > 1);
        CHECK(actual_tool_one != actual_tool_zero);
        CHECK(actual_bash_command_one != actual_bash_command_zero);
        CHECK(actual_bash_output_one != actual_bash_output_zero);
        CHECK(actual_custom_one != actual_custom_zero);
        CHECK(compact_text(at_zero).find(
                      "echowrap-toolauditpayloadwithenoughwordstocontinueontoanotherterminalrowandtestwidth-"
                      "dependentwrapping") != std::string::npos);
        CHECK(compact_text(at_zero).find(
                      "echowrap-bashauditcommandwithenoughwordstocontinueontoanotherterminalrowandtestwidth-"
                      "dependentwrapping") != std::string::npos);
        CHECK(compact_text(at_zero).find("wrap-"
                                         "bashoutputcarriesadditionalwordssothebodywrapsatthenarrowwidthandkeepsmeasuri"
                                         "ngdifferentlyateverytestedterminalwidth") != std::string::npos);
        CHECK(compact_text(at_zero).find("wrap-"
                                         "customcontentcarriesadditionalwordssothiscustommessagewrapsatthenarrowwidthan"
                                         "dkeepsmeasuringdifferentlyateverytestedterminalwidth") != std::string::npos);
        if (width == 32U) {
            CHECK(actual_tool_one.size() > 1);
            CHECK(actual_bash_command_one.size() > 1);
            CHECK(actual_bash_output_one.size() > 1);
            CHECK(actual_custom_one.size() > 1);
        }
        chat.set_output_pad(1);
    }
    CHECK(narrow_tool_rows != wide_tool_rows);
    CHECK(narrow_bash_command_rows != wide_bash_command_rows);
    CHECK(narrow_bash_output_rows != wide_bash_output_rows);
    CHECK(narrow_custom_rows != wide_custom_rows);
}

TEST_CASE("ChatContainer output pad still reaches the user and assistant messages",
        "[coding_agent][tui][issue1121][spec]") {
    auto theme = test_theme();
    coding_agent::tui::ChatContainer chat(theme, test_keybinding_slot());
    chat.append_committed_message(ai::user_text_message("audit prompt"));
    ai::AssistantMessage assistant;
    assistant.content.push_back(ai::TextContent{.text = "audit reply"});
    assistant.stop_reason = ai::AssistantStopReason::Stop;
    chat.append_committed_message(assistant);

    const auto at_one = rendered_lines(chat, 60);
    CHECK(leading_spaces(at_one, "audit prompt") == 1);
    CHECK(leading_spaces(at_one, "audit reply") == 1);

    chat.set_output_pad(0);
    const auto at_zero = rendered_lines(chat, 60);
    CHECK(leading_spaces(at_zero, "audit prompt") == 0);
    CHECK(leading_spaces(at_zero, "audit reply") == 0);
}

TEST_CASE("a chat diagnostic keeps the output pad it was emitted with", "[coding_agent][tui][issue1121][spec]") {
    // pi `showError` creates the error line with the live pad at emission time
    // (`new ThemedText(() => theme.fg("error", `Error: ${errorMessage}`),
    // this.outputPad, 0)`, `interactive-mode.ts:4639`). `onOutputPadChange`
    // only reaches children that expose `setOutputPad`, and a plain
    // `ThemedText` does not (`components/themed-text.ts:11-32`), so an
    // already-emitted error keeps its original pad after the setting changes.
    auto theme = test_theme();
    coding_agent::tui::ChatContainer chat(theme, test_keybinding_slot());
    chat.append_diagnostic("emitted at pad one");
    chat.set_output_pad(0);

    const auto lines = rendered_lines(chat, 60);
    CHECK(leading_spaces(lines, "Error: emitted at pad one") == 1);

    // A diagnostic emitted after the change takes the new pad.
    chat.append_diagnostic("emitted at pad zero");
    const auto later = rendered_lines(chat, 60);
    CHECK(leading_spaces(later, "Error: emitted at pad zero") == 0);
}
