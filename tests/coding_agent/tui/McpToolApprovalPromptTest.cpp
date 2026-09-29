// The Native TUI's call-approval prompt for an `approval: "ask"` Upstream MCP
// Server (issue #843): it must show the call it is asking about — the
// Qualified Tool Name, the Upstream it targets, and the arguments that would
// be sent — and it must offer exactly two per-call answers plus a dismissal
// that is not a decision.

#include "coding_agent/tui/McpToolApprovalPrompt.hpp"
#include "coding_agent/tui/Theme.hpp"

#include <cch/coding_agent/McpToolApproval.hpp>
#include <cch/tui/Keybindings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
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

[[nodiscard]] coding_agent::tui::LiveTheme test_theme() {
    return coding_agent::tui::LiveTheme(
            coding_agent::tui::builtin_dark_theme(), tui::TerminalColorCapability::TrueColor);
}

[[nodiscard]] std::string strip_ansi(std::string_view text) {
    std::string stripped;
    stripped.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == '[') {
            index += 2;
            while (index < text.size() && !(text[index] >= '@' && text[index] <= '~'))
                ++index;
            if (index < text.size()) ++index;
            continue;
        }
        stripped.push_back(text[index]);
        ++index;
    }
    return stripped;
}

[[nodiscard]] std::string render_screen(coding_agent::tui::McpToolApprovalPromptComponent& prompt) {
    const auto rendered = prompt.render(100);
    REQUIRE(rendered);
    std::string screen;
    for (const auto& line : rendered->lines) {
        screen.append(strip_ansi(line));
        screen.push_back('\n');
    }
    return screen;
}

[[nodiscard]] coding_agent::McpToolApprovalRequest a_call() {
    return coding_agent::McpToolApprovalRequest{
            .server_id = "executor",
            .qualified_tool_name = "mcp__executor__search",
            .tool_name = "search",
            .arguments_json = R"({"query":"weather in Oslo","limit":3})",
    };
}

/// One prompt whose three answers are recorded separately, so a case can tell
/// consent from a refusal from a dismissal.
struct Answers {
    std::size_t allowed{0};
    std::size_t denied{0};
    std::size_t cancelled{0};

    [[nodiscard]] std::shared_ptr<coding_agent::tui::McpToolApprovalPromptComponent> prompt() {
        auto answers = std::make_shared<Answers>();
        return std::make_shared<coding_agent::tui::McpToolApprovalPromptComponent>(
                test_theme(),
                test_keybindings(),
                a_call(),
                [answers] { ++answers->allowed; },
                [answers] { ++answers->denied; },
                [answers] { ++answers->cancelled; });
    }
};

} // namespace

TEST_CASE("the call-approval prompt shows the qualified tool name and the full arguments",
        "[coding_agent][tui][mcp-approval][issue843][spec]") {
    auto prompt = Answers{}.prompt();
    const auto screen = render_screen(*prompt);

    // The question: what is being authorized.
    CHECK(screen.find("Allow this tool call?") != std::string::npos);
    // The name the model called, and the Upstream it targets — the two facts
    // that tell the user this is a third-party tool and which one.
    CHECK(screen.find("mcp__executor__search") != std::string::npos);
    CHECK(screen.find("executor") != std::string::npos);
    CHECK(screen.find("search") != std::string::npos);
    // The arguments in full, not summarized: both values of this call are on
    // screen, because the consent is about exactly this call.
    CHECK(screen.find("Arguments:") != std::string::npos);
    CHECK(screen.find(R"("query":"weather in Oslo")") != std::string::npos);
    CHECK(screen.find(R"("limit":3)") != std::string::npos);
    // Two per-call answers and a dismissal, and no remembered-decision
    // affordance: consent is for this call only.
    CHECK(screen.find("Allow this call") != std::string::npos);
    CHECK(screen.find("Deny this call") != std::string::npos);
    CHECK(screen.find("always") == std::string::npos);
}

TEST_CASE("the call-approval prompt answers allow, deny, and dismissal",
        "[coding_agent][tui][mcp-approval][issue843][spec]") {
    auto answers = std::make_shared<Answers>();
    auto prompt = std::make_shared<coding_agent::tui::McpToolApprovalPromptComponent>(
            test_theme(),
            test_keybindings(),
            a_call(),
            [answers] { ++answers->allowed; },
            [answers] { ++answers->denied; },
            [answers] { ++answers->cancelled; });

    // The highlighted row opens on *allow*: Enter consents to this one call.
    static_cast<void>(prompt->handle_input(tui::KeyEvent{.key = "enter"}));
    CHECK(answers->allowed == 1);
    CHECK(answers->denied == 0);
    CHECK(answers->cancelled == 0);
}

TEST_CASE("the call-approval prompt refuses on the deny row and dismisses on Escape",
        "[coding_agent][tui][mcp-approval][issue843][spec]") {
    auto answers = std::make_shared<Answers>();
    auto prompt = std::make_shared<coding_agent::tui::McpToolApprovalPromptComponent>(
            test_theme(),
            test_keybindings(),
            a_call(),
            [answers] { ++answers->allowed; },
            [answers] { ++answers->denied; },
            [answers] { ++answers->cancelled; });

    static_cast<void>(prompt->handle_input(tui::KeyEvent{.key = "down"}));
    static_cast<void>(prompt->handle_input(tui::KeyEvent{.key = "enter"}));
    CHECK(answers->denied == 1);
    CHECK(answers->allowed == 0);

    // Escape is a dismissal, not a decision: it is its own answer, and the
    // policy records nothing.
    static_cast<void>(prompt->handle_input(tui::KeyEvent{.key = "escape"}));
    CHECK(answers->cancelled == 1);
    CHECK(answers->allowed == 0);
    CHECK(answers->denied == 1);
}
