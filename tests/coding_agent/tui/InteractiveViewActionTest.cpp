// Focused tests for the Native TUI main-screen view-to-state action seam
// (InteractiveViewActions.hpp + InteractiveView): each main-screen
// keybinding emits one closed `ViewAction` alternative through the single
// `ViewActionSink`, payloads are preserved, admission order is preserved, an
// `ExpectedVoid` failure is recorded as the render error, toolkit-only
// editor state emits no application action, and the coalescible invalidate
// request stays a separate path from the action seam.
//
// The view is constructed directly with a fake action sink and real
// keybinding/theme fixtures, so these tests cross the same seam callers use
// (ADR 0040 "the interface is the test surface") without booting an Agent
// Session or SessionFactory.

#include "coding_agent/tui/InteractiveView.hpp"

#include "coding_agent/tui/KeybindingsManager.hpp"
#include "coding_agent/tui/Theme.hpp"

#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Keys.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] std::shared_ptr<const tui::KeybindingRegistry> test_keybindings() {
    // The toolkit's editor/select bindings plus the assembled application
    // actions the main screen dispatches (the same catalog the state
    // assembles). Session actions are unbound by default, so they receive
    // explicit overrides for direct injection.
    tui::KeybindingResolutionRequest request;
    request.definitions = tui::builtin_tui_keybinding_definitions();
    const std::vector<std::string_view> app_ids{
        "app.exit",
        "app.interrupt",
        "app.clear",
        "app.suspend",
        "app.editor.external",
        "app.tools.expand",
        "app.thinking.toggle",
        "app.thinking.cycle",
        "app.model.cycleForward",
        "app.model.cycleBackward",
        "app.model.select",
        "app.message.followUp",
        "app.message.dequeue",
        "app.message.copy",
        "app.clipboard.pasteImage",
        "app.session.resume",
        "app.session.fork",
        "app.session.new",
        "app.session.tree",
    };
    auto app_definitions = coding_agent::tui::app_keybinding_definitions(app_ids);
    REQUIRE(app_definitions);
    for (auto& definition : *app_definitions) {
        request.definitions.push_back(std::move(definition));
    }
    request.overrides = {
        tui::KeybindingOverride{"app.session.resume", {"f1"}},
        tui::KeybindingOverride{"app.session.fork", {"f2"}},
        tui::KeybindingOverride{"app.session.new", {"f3"}},
        tui::KeybindingOverride{"app.session.tree", {"f4"}},
    };
    auto resolved = tui::resolve_keybindings(std::move(request));
    REQUIRE(resolved);
    return resolved->registry;
}

[[nodiscard]] std::shared_ptr<coding_agent::tui::SharedKeybindings>
test_keybinding_slot() {
    return std::make_shared<coding_agent::tui::SharedKeybindings>(
        test_keybindings());
}

[[nodiscard]] coding_agent::tui::LiveTheme test_theme() {
    return coding_agent::tui::LiveTheme(
        coding_agent::tui::builtin_dark_theme(),
        tui::TerminalColorCapability::TrueColor);
}

/// One key event; Release events are ignored by the view, and the default is
/// a Press.
[[nodiscard]] tui::InputEventVariant key(
    std::string name,
    bool ctrl = false,
    bool shift = false,
    bool alt = false) {
    return tui::InputEventVariant{tui::KeyEvent{
        .key = std::move(name),
        .ctrl = ctrl,
        .shift = shift,
        .alt = alt,
        .type = tui::KeyEventType::Press,
    }};
}

struct ViewFixture {
    std::vector<coding_agent::tui::ViewAction> actions;
    std::size_t invalidations{0};
    std::optional<support::Error> sink_error;
    std::shared_ptr<coding_agent::tui::SharedKeybindings> keybindings;
    coding_agent::tui::LiveTheme theme;
    std::unique_ptr<coding_agent::tui::InteractiveView> view;

    explicit ViewFixture(bool user_bash_available = false)
        : keybindings(test_keybinding_slot()),
          theme(test_theme()) {
        coding_agent::tui::InteractiveViewOptions options;
        options.keybindings = keybindings;
        options.on_invalidate = [this] { ++invalidations; };
        options.action_sink =
            [this](coding_agent::tui::ViewAction action) noexcept
            -> support::ExpectedVoid {
                if (sink_error) {
                    return std::unexpected(*sink_error);
                }
                actions.push_back(std::move(action));
                return support::ExpectedVoid{};
            };
        options.footer_data_source = [] {
            return coding_agent::tui::FooterData{};
        };
        options.hide_thinking_block = false;
        options.output_pad = 0;
        options.user_bash_available = user_bash_available;
        options.theme = &theme;
        view = std::make_unique<coding_agent::tui::InteractiveView>(
            std::move(options));
    }

    /// Type one character through the editor so the submission payload
    /// carries a real sampled text and revision.
    void type(std::string character) {
        view->handle_input(key(std::move(character)));
    }

    [[nodiscard]] const coding_agent::tui::ViewAction& last_action() const {
        REQUIRE_FALSE(actions.empty());
        return actions.back();
    }
};

} // namespace

TEST_CASE(
    "main-screen keybindings emit the matching ViewAction alternative",
    "[coding_agent][tui][view_actions]") {
    ViewFixture fixture;
    auto& view = *fixture.view;

    view.handle_input(key("d", /*ctrl=*/true));
    REQUIRE(fixture.actions.size() == 1);
    CHECK(std::holds_alternative<coding_agent::tui::ExitAction>(fixture.actions[0]));

    view.handle_input(key("escape"));
    REQUIRE(fixture.actions.size() == 2);
    CHECK(std::holds_alternative<coding_agent::tui::InterruptAction>(fixture.actions[1]));

    view.handle_input(key("v", /*ctrl=*/true));
    REQUIRE(fixture.actions.size() == 3);
    CHECK(std::holds_alternative<coding_agent::tui::ClipboardPasteAction>(fixture.actions[2]));

    view.handle_input(key("up", /*ctrl=*/false, /*shift=*/false, /*alt=*/true));
    REQUIRE(fixture.actions.size() == 4);
    CHECK(std::holds_alternative<coding_agent::tui::DequeueAction>(fixture.actions[3]));

    view.handle_input(key("z", /*ctrl=*/true));
    REQUIRE(fixture.actions.size() == 5);
    CHECK(std::holds_alternative<coding_agent::tui::SuspendAction>(fixture.actions[4]));

    view.handle_input(key("g", /*ctrl=*/true));
    REQUIRE(fixture.actions.size() == 6);
    CHECK(std::holds_alternative<coding_agent::tui::ExternalEditorAction>(fixture.actions[5]));

    view.handle_input(key("t", /*ctrl=*/true));
    REQUIRE(fixture.actions.size() == 7);
    CHECK(std::holds_alternative<coding_agent::tui::ToggleThinkingAction>(fixture.actions[6]));

    view.handle_input(key("tab", /*ctrl=*/false, /*shift=*/true));
    REQUIRE(fixture.actions.size() == 8);
    CHECK(std::holds_alternative<coding_agent::tui::CycleThinkingAction>(fixture.actions[7]));

    view.handle_input(key("p", /*ctrl=*/true));
    REQUIRE(fixture.actions.size() == 9);
    const auto& forward =
        std::get<coding_agent::tui::CycleModelAction>(fixture.actions[8]);
    CHECK(forward.direction == coding_agent::tui::ModelCycleDirection::Forward);

    view.handle_input(key("p", /*ctrl=*/true, /*shift=*/true));
    REQUIRE(fixture.actions.size() == 10);
    const auto& backward =
        std::get<coding_agent::tui::CycleModelAction>(fixture.actions[9]);
    CHECK(backward.direction == coding_agent::tui::ModelCycleDirection::Backward);

    view.handle_input(key("l", /*ctrl=*/true));
    REQUIRE(fixture.actions.size() == 11);
    CHECK(std::holds_alternative<coding_agent::tui::SelectModelAction>(fixture.actions[10]));

    view.handle_input(key("x", /*ctrl=*/true));
    REQUIRE(fixture.actions.size() == 12);
    CHECK(std::holds_alternative<coding_agent::tui::CopyLastMessageAction>(fixture.actions[11]));

    // Session actions are unbound by default and reachable through the
    // explicit overrides in the fixture registry.
    view.handle_input(key("f1"));
    REQUIRE(fixture.actions.size() == 13);
    CHECK(std::holds_alternative<coding_agent::tui::ResumeSessionAction>(fixture.actions[12]));

    view.handle_input(key("f2"));
    REQUIRE(fixture.actions.size() == 14);
    CHECK(std::holds_alternative<coding_agent::tui::ForkSessionAction>(fixture.actions[13]));

    view.handle_input(key("f3"));
    REQUIRE(fixture.actions.size() == 15);
    CHECK(std::holds_alternative<coding_agent::tui::NewSessionAction>(fixture.actions[14]));

    view.handle_input(key("f4"));
    REQUIRE(fixture.actions.size() == 16);
    CHECK(std::holds_alternative<coding_agent::tui::OpenTreeSelectorAction>(fixture.actions[15]));
}

TEST_CASE(
    "submit and follow-up preserve the sampled editor payload",
    "[coding_agent][tui][view_actions]") {
    ViewFixture fixture;
    auto& view = *fixture.view;

    // Type a short prompt, then submit with Enter (pi `tui.input.submit`).
    for (const auto& character : std::vector<std::string>{"h", "i"}) {
        fixture.type(character);
    }
    view.handle_input(key("enter"));
    REQUIRE(fixture.actions.size() == 1);
    const auto& submit =
        std::get<coding_agent::tui::SubmitAction>(fixture.actions[0]);
    CHECK(submit.submission == coding_agent::tui::InputSubmission::Ordinary);
    CHECK(submit.request.text == "hi");
    CHECK(submit.request.editor_revision == 2);

    // Follow-up (Alt+Enter) trims and queues with the FollowUp submission.
    fixture.type("!");
    view.handle_input(key("enter", /*ctrl=*/false, /*shift=*/false, /*alt=*/true));
    REQUIRE(fixture.actions.size() == 2);
    const auto& follow_up =
        std::get<coding_agent::tui::SubmitAction>(fixture.actions[1]);
    CHECK(follow_up.submission == coding_agent::tui::InputSubmission::FollowUp);
    CHECK(follow_up.request.text == "!");
    CHECK(follow_up.request.editor_revision == 3);
}

TEST_CASE(
    "interrupt captures the bash-mode editor state at key-press time",
    "[coding_agent][tui][view_actions]") {
    ViewFixture fixture{/*user_bash_available=*/true};
    auto& view = *fixture.view;

    for (const auto& character :
         std::vector<std::string>{"!", "l", "s"}) {
        fixture.type(character);
    }
    view.handle_input(key("escape"));
    REQUIRE(fixture.actions.size() == 1);
    const auto& interrupt =
        std::get<coding_agent::tui::InterruptAction>(fixture.actions[0]);
    CHECK(interrupt.request.pending_bash_text == "!ls");
    CHECK(interrupt.request.pending_bash);
    CHECK(interrupt.request.editor_revision == 3);
}

TEST_CASE(
    "admission order is preserved across a burst of actions",
    "[coding_agent][tui][view_actions]") {
    ViewFixture fixture;
    auto& view = *fixture.view;

    view.handle_input(key("p", /*ctrl=*/true)); // cycle forward
    view.handle_input(key("l", /*ctrl=*/true)); // model selector
    view.handle_input(key("escape"));           // interrupt
    view.handle_input(key("d", /*ctrl=*/true)); // exit
    REQUIRE(fixture.actions.size() == 4);
    CHECK(std::holds_alternative<coding_agent::tui::CycleModelAction>(fixture.actions[0]));
    CHECK(std::holds_alternative<coding_agent::tui::SelectModelAction>(fixture.actions[1]));
    CHECK(std::holds_alternative<coding_agent::tui::InterruptAction>(fixture.actions[2]));
    CHECK(std::holds_alternative<coding_agent::tui::ExitAction>(fixture.actions[3]));
}

TEST_CASE(
    "toolkit-only editor state emits no application action",
    "[coding_agent][tui][view_actions]") {
    ViewFixture fixture;
    auto& view = *fixture.view;

    // Type text, then clear it with `app.clear` (ctrl+c): a toolkit-only
    // editor mutation with no ViewAction.
    fixture.type("h");
    view.handle_input(key("c", /*ctrl=*/true));
    REQUIRE(fixture.actions.empty());
    CHECK(view.editor_text().empty());

    // `app.tools.expand` (ctrl+o) also stays toolkit-only (its invalidation
    // flows through the separate coalescible render request).
    view.handle_input(key("o", /*ctrl=*/true));
    REQUIRE(fixture.actions.empty());
    CHECK(fixture.invalidations > 0);
}

TEST_CASE(
    "the coalescible invalidate request stays separate from the action seam",
    "[coding_agent][tui][view_actions]") {
    ViewFixture fixture;
    auto& view = *fixture.view;

    // A toolkit-only render path (tools expand) invalidates without emitting
    // an action; an application action does not invalidate by itself.
    view.handle_input(key("o", /*ctrl=*/true));
    CHECK(fixture.actions.empty());
    CHECK(fixture.invalidations == 1);

    view.handle_input(key("p", /*ctrl=*/true));
    CHECK(fixture.actions.size() == 1);
    CHECK(fixture.invalidations == 1);

    // Editor typing invalidates through the separate path (the editor's
    // change sink), still not via the action seam.
    fixture.type("h");
    CHECK(fixture.actions.size() == 1);
    CHECK(fixture.invalidations == 2);
}

TEST_CASE(
    "an ExpectedVoid failure is recorded and surfaces as the render error",
    "[coding_agent][tui][view_actions]") {
    ViewFixture fixture;
    auto& view = *fixture.view;

    fixture.sink_error = support::make_error(
        support::ErrorCode::Unknown,
        "fake admission failure");
    view.handle_input(key("d", /*ctrl=*/true));
    // The action never reached the recorder; the failure is retained.
    CHECK(fixture.actions.empty());

    auto rendered = view.render(80);
    REQUIRE_FALSE(rendered);
    CHECK(rendered.error().message.find("Native TUI exit action failed") !=
          std::string::npos);
    CHECK(rendered.error().detail == "fake admission failure");
}
