// The URL-mode Pending Elicitation dialog (issue #845; spec #833 story 27).
//
// The dialog is the Native TUI half of a Multi Round-Trip suspension, and it
// is reached only through the session's `cch_coding_agent` projection: this
// file includes no `cch/mcp` header and cannot, which is the headless-no-
// frontend invariant (ADR 0065) asserted by construction.
//
// The claims under test are the ones a user would notice:
//   * the Server Id and the address are on screen, because an approval page
//     with no server named is not consent anyone can give;
//   * the open-browser action is offered and is **not** an answer: the dialog
//     stays up and the question stays unanswered, so an Upstream cannot
//     manufacture consent by launching a page;
//   * Done, Decline, and Cancel are three named controls with three distinct
//     outcomes, and the first one settles the dialog for good;
//   * a withdrawn dialog answers nothing and launches nothing;
//   * every line fits the render width, at the narrow widths the composed
//     render path enforces.

#include "coding_agent/tui/McpElicitationDialog.hpp"
#include "coding_agent/tui/Theme.hpp"
#include "support/OverlayWidthBound.hpp"

#include <cch/coding_agent/McpElicitation.hpp>
#include <cch/tui/Keybindings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;
using coding_agent::McpElicitationAction;
using coding_agent::McpElicitationMode;
using coding_agent::McpPendingElicitation;
using coding_agent::tui::McpElicitationDialog;
using coding_agent::tui::McpUrlElicitationView;

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

[[nodiscard]] McpPendingElicitation pending() {
    return McpPendingElicitation{
            .elicitation_id = "executor#1",
            .server_id = "executor",
            .tool_name = "approve",
            .mode = McpElicitationMode::Url,
            .request_id = "r1",
            .message = "Approve this action",
            .url = "https://executor.invalid/mcp/approve/abc",
    };
}

/// One dialog plus the answers and browser requests it produced, so a case
/// asserts on what the user did rather than on the dialog's internals.
struct DialogFixture {
    std::vector<std::pair<McpElicitationAction, std::string>> answers{};
    std::vector<std::string> opened{};
    int invalidations{0};
    /// The dialog borrows the palette and reads the keybinding registry, the
    /// way the host's theme controller outlives every flow it owns, so both
    /// live here rather than in a temporary.
    coding_agent::tui::LiveTheme theme{test_theme()};
    std::shared_ptr<const tui::KeybindingRegistry> keybindings{test_keybindings()};
    std::shared_ptr<McpElicitationDialog> dialog{};

    explicit DialogFixture(McpPendingElicitation request = pending()) {
        dialog = std::make_shared<McpElicitationDialog>(
                theme,
                keybindings,
                McpUrlElicitationView{.request = std::move(request)},
                [this](McpElicitationAction action, std::string id) { answers.emplace_back(action, std::move(id)); },
                [this](std::string url) { opened.push_back(std::move(url)); },
                [this] { ++invalidations; });
    }

    void press(std::string key) { static_cast<void>(dialog->handle_input(tui::KeyEvent{.key = std::move(key)})); }

    [[nodiscard]] std::string rendered(std::size_t width) const {
        auto result = dialog->render(width);
        REQUIRE(result.has_value());
        std::string text;
        for (const auto& line : result->lines) {
            text += tests::strip_ansi(line);
            text += '\n';
        }
        return text;
    }
};

} // namespace

TEST_CASE("the URL elicitation dialog shows the server and the address to visit",
        "[coding_agent][tui][mcp][issue845][spec]") {
    DialogFixture fixture;
    const auto screen = fixture.rendered(80);
    // The server is named: the user is authorizing one upstream's work, and an
    // approval page with no server named is not consent anyone can give.
    CHECK(screen.find("executor") != std::string::npos);
    CHECK(screen.find("approve") != std::string::npos);
    CHECK(screen.find("Approve this action") != std::string::npos);
    CHECK(screen.find("https://executor.invalid/mcp/approve/abc") != std::string::npos);
    // The three answers are named on screen, not left to a keybinding the user
    // has to know: Done, Decline, and Cancel are different decisions.
    CHECK(screen.find("done") != std::string::npos);
    CHECK(screen.find("decline") != std::string::npos);
    CHECK(screen.find("cancel") != std::string::npos);
    // And the open-browser action says what it is not.
    CHECK(screen.find("open browser (not an answer)") != std::string::npos);
}

TEST_CASE("the URL elicitation dialog renders within the width it is handed",
        "[coding_agent][tui][mcp][issue845][spec]") {
    // A long Server Id, tool name, message, and URL: the row that would blow
    // the bound if the dialog did not let the text component wrap it.
    DialogFixture fixture(McpPendingElicitation{
            .elicitation_id = "a-very-long-server-id#12",
            .server_id = "a-very-long-server-id-that-never-ends",
            .tool_name = "an_upstream_tool_with_a_deliberately_long_name",
            .mode = McpElicitationMode::Url,
            .message = "Approve this action before it is carried out, and read what you are approving first",
            .url = "https://executor.invalid/mcp/approve/0123456789abcdef0123456789abcdef",
    });
    for (const auto width : tests::kNarrowOverlayWidths) {
        auto rendered = fixture.dialog->render(width);
        REQUIRE(rendered.has_value());
        INFO("bound " << width);
        tests::check_all_lines_bounded(*rendered, width);
    }
}

TEST_CASE("opening the browser is an action and never an answer", "[coding_agent][tui][mcp][issue845][spec]") {
    DialogFixture fixture;
    fixture.press("o");
    REQUIRE(fixture.opened.size() == 1);
    CHECK(fixture.opened.front() == "https://executor.invalid/mcp/approve/abc");
    // The question is still on screen and still unanswered: an Upstream must
    // not be able to manufacture consent by launching a page.
    CHECK(fixture.dialog->live());
    CHECK(fixture.answers.empty());
    CHECK(fixture.rendered(80).find("open browser (not an answer)") != std::string::npos);
    // And the user can then answer it themselves.
    fixture.press("d");
    REQUIRE(fixture.answers.size() == 1);
    CHECK(fixture.answers.front().first == McpElicitationAction::Decline);
    CHECK(fixture.answers.front().second == "executor#1");
}

TEST_CASE("each of the three answers settles the dialog once and for all", "[coding_agent][tui][mcp][issue845][spec]") {
    SECTION("done accepts") {
        DialogFixture fixture;
        fixture.press("enter");
        REQUIRE(fixture.answers.size() == 1);
        CHECK(fixture.answers.front().first == McpElicitationAction::Accept);
        CHECK(fixture.answers.front().second == "executor#1");
    }
    SECTION("decline is its own answer") {
        DialogFixture fixture;
        fixture.press("d");
        REQUIRE(fixture.answers.size() == 1);
        CHECK(fixture.answers.front().first == McpElicitationAction::Decline);
    }
    SECTION("cancel is its own answer") {
        DialogFixture fixture;
        fixture.press("escape");
        REQUIRE(fixture.answers.size() == 1);
        CHECK(fixture.answers.front().first == McpElicitationAction::Cancel);
    }
    // A second control press changes nothing: the first settlement won, so a
    // keypress racing the answer cannot complete a settled call twice.
    DialogFixture fixture;
    fixture.press("d");
    fixture.press("enter");
    fixture.press("escape");
    CHECK(fixture.answers.size() == 1);
    CHECK(fixture.answers.front().first == McpElicitationAction::Decline);
    CHECK_FALSE(fixture.dialog->live());
}

TEST_CASE("a withdrawn dialog answers nothing and launches nothing", "[coding_agent][tui][mcp][issue845][spec]") {
    DialogFixture fixture;
    // Session Close withdraws the dialog: the call behind it is being torn
    // down, so there is nothing left to answer.
    fixture.dialog->withdraw();
    CHECK_FALSE(fixture.dialog->live());
    CHECK(fixture.answers.empty());
    // A late click on a withdrawn dialog is inert in both directions.
    fixture.press("enter");
    fixture.press("o");
    CHECK(fixture.answers.empty());
    CHECK(fixture.opened.empty());
    // A second withdrawal is a no-op rather than a second answer.
    fixture.dialog->withdraw();
    CHECK(fixture.answers.empty());
}

TEST_CASE("the dialog carries no server once it has been withdrawn", "[coding_agent][tui][mcp][issue845][spec]") {
    DialogFixture fixture;
    CHECK(fixture.dialog->server_id() == "executor");
    fixture.press("enter");
    // The projection row is the dialog's whole input, so the server id is
    // still readable after the answer: a test or a status surface can name
    // which upstream was answered.
    CHECK(fixture.dialog->server_id() == "executor");
}
