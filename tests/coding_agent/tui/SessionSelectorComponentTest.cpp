// P13: the session selector component in isolation — the threaded tree
// rendering with parent/child prefixes, the scope/sort/filter cycle, the
// delete confirmation flow, and the rename inline input.

#include <catch2/catch_test_macros.hpp>

#include "coding_agent/tui/KeybindingsManager.hpp"
#include "coding_agent/tui/SessionSelector.hpp"
#include "coding_agent/tui/Theme.hpp"

#include <cch/tui/Keybindings.hpp>

#include <cch/support/Error.hpp>
#include <chrono>
#include <iterator>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] std::shared_ptr<const tui::KeybindingRegistry> test_keybindings() {
    // The selector matches both the tui.* table and the selector-scoped
    // app.session.* actions through the same shared registry (pi's
    // KeybindingsManager).
    auto definitions = tui::builtin_tui_keybinding_definitions();
    const std::vector<std::string_view> actions{
        "app.session.toggleSort",
        "app.session.toggleNamedFilter",
        "app.session.togglePath",
        "app.session.rename",
        "app.session.delete",
        "app.session.deleteNoninvasive",
    };
    auto app_definitions = coding_agent::tui::app_keybinding_definitions(actions);
    REQUIRE(app_definitions);
    definitions.insert(
        definitions.end(),
        std::make_move_iterator(app_definitions->begin()),
        std::make_move_iterator(app_definitions->end()));
    tui::KeybindingResolutionRequest request;
    request.definitions = std::move(definitions);
    auto resolved = tui::resolve_keybindings(std::move(request));
    REQUIRE(resolved);
    return resolved->registry;
}

[[nodiscard]] coding_agent::tui::LiveTheme test_theme() {
    return coding_agent::tui::LiveTheme(
        coding_agent::tui::builtin_dark_theme(),
        tui::TerminalColorCapability::TrueColor);
}

coding_agent::session_discovery::SessionInfo make_session(
    std::string id,
    std::optional<std::string> parent = std::nullopt,
    std::string first_message = "hello",
    std::optional<std::string> name = std::nullopt) {
    coding_agent::session_discovery::SessionInfo session;
    session.path = std::filesystem::path{"/tmp/"} / (id + ".jsonl");
    session.id = std::move(id);
    session.cwd = "/work";
    session.name = std::move(name);
    if (parent) {
        session.parent_session_path = std::filesystem::path{*parent};
    }
    session.modified = std::filesystem::file_time_type::clock::now();
    session.message_count = 1;
    session.first_message = std::move(first_message);
    session.all_messages_text = "hello";
    return session;
}

/// Strip ANSI escapes for text assertions.
[[nodiscard]] std::string strip_ansi(std::string text) {
    std::string result;
    std::size_t index = 0;
    while (index < text.size()) {
        if (text[index] == '\x1b') {
            while (index < text.size() && text[index] != 'm') ++index;
            ++index;
            continue;
        }
        result.push_back(text[index++]);
    }
    return result;
}

[[nodiscard]] std::string render_text(coding_agent::tui::SessionSelectorComponent& component) {
    auto rendered = component.render(100);
    REQUIRE(rendered);
    std::string text;
    for (const auto& line : rendered->lines) {
        text += strip_ansi(line);
        text.push_back('\n');
    }
    return text;
}

} // namespace

TEST_CASE(
    "session selector renders the threaded tree with parent/child prefixes",
    "[coding_agent][tui][session-selector][issue409]") {
    auto theme = test_theme();
    auto keybindings = test_keybindings();
    auto loader = [] {
        // parent -> child -> grandchild chain plus a second child under
        // parent, so the continuation line renders under the first child.
        auto sessions = std::vector<coding_agent::session_discovery::SessionInfo>{};
        sessions.push_back(make_session("parent", std::nullopt, "parent message"));
        sessions.push_back(
            make_session("child", std::string{"/tmp/parent.jsonl"}, "child message"));
        sessions.push_back(
            make_session("grandchild", std::string{"/tmp/child.jsonl"}, "grandchild message"));
        sessions.push_back(
            make_session("sibling", std::string{"/tmp/parent.jsonl"}, "sibling message"));
        sessions.push_back(make_session("other", std::nullopt, "other message"));
        // Newest first: parent, child, grandchild, sibling, other.
        for (std::size_t index = 0; index < sessions.size(); ++index) {
            sessions[index].modified =
                std::filesystem::file_time_type::clock::now() -
                std::chrono::hours(static_cast<std::int64_t>(index));
        }
        return sessions;
    };
    coding_agent::tui::SessionSelectorComponent component(
        theme,
        keybindings,
        loader,
        [&] { return std::vector<coding_agent::session_discovery::SessionInfo>{}; },
        std::optional<std::filesystem::path>{},
        [](std::string) -> support::ExpectedVoid { return {}; },
        [] {},
        [] {},
        [](std::string, std::string) { return support::ExpectedVoid{}; },
        [] {});

    const auto text = render_text(component);
    CHECK(text.find("Resume Session (Current Folder)") != std::string::npos);
    // Root rows carry no prefix; children carry the branch glyphs; the
    // grandchild continues the first child's line with "│" (pi).
    CHECK(text.find("parent message") != std::string::npos);
    CHECK(text.find("├─ child message") != std::string::npos);
    CHECK(text.find("│  └─ grandchild message") != std::string::npos);
    CHECK(text.find("└─ sibling message") != std::string::npos);
    // Roots carry no tree prefix (the cursor columns only).
    CHECK(text.find("  other message") != std::string::npos);
}

TEST_CASE(
    "session selector cycles sort modes and the named filter through the bindings",
    "[coding_agent][tui][session-selector][issue409]") {
    auto theme = test_theme();
    auto keybindings = test_keybindings();
    auto loader = [] {
        std::vector<coding_agent::session_discovery::SessionInfo> sessions;
        sessions.push_back(make_session("a", std::nullopt, "alpha"));
        sessions.push_back(make_session("b", std::nullopt, "beta", std::string{"My Session"}));
        return sessions;
    };
    coding_agent::tui::SessionSelectorComponent component(
        theme,
        keybindings,
        loader,
        [&] { return std::vector<coding_agent::session_discovery::SessionInfo>{}; },
        std::optional<std::filesystem::path>{},
        [](std::string) -> support::ExpectedVoid { return {}; },
        [] {},
        [] {},
        [](std::string, std::string) { return support::ExpectedVoid{}; },
        [] {});

    // Ctrl+s cycles Threaded -> Recent -> Fuzzy.
    static_cast<void>(component.handle_input(tui::KeyEvent{.key = "s", .ctrl = true}));
    CHECK(render_text(component).find("Sort: Recent") != std::string::npos);
    static_cast<void>(component.handle_input(tui::KeyEvent{.key = "s", .ctrl = true}));
    CHECK(render_text(component).find("Sort: Fuzzy") != std::string::npos);
    static_cast<void>(component.handle_input(tui::KeyEvent{.key = "s", .ctrl = true}));
    CHECK(render_text(component).find("Sort: Threaded") != std::string::npos);

    // Ctrl+n filters to named sessions only.
    static_cast<void>(component.handle_input(tui::KeyEvent{.key = "n", .ctrl = true}));
    const auto named = render_text(component);
    CHECK(named.find("Name: Named") != std::string::npos);
    // The row displays the session name when one exists (pi).
    CHECK(named.find("My Session") != std::string::npos);
    CHECK(named.find("alpha") == std::string::npos);
}

TEST_CASE(
    "session selector toggles the path display and the delete confirmation cancels",
    "[coding_agent][tui][session-selector][issue409]") {
    auto theme = test_theme();
    auto keybindings = test_keybindings();
    auto loader = [] {
        std::vector<coding_agent::session_discovery::SessionInfo> sessions;
        sessions.push_back(make_session("a", std::nullopt, "alpha"));
        return sessions;
    };
    coding_agent::tui::SessionSelectorComponent component(
        theme,
        keybindings,
        loader,
        [&] { return std::vector<coding_agent::session_discovery::SessionInfo>{}; },
        std::optional<std::filesystem::path>{},
        [](std::string) -> support::ExpectedVoid { return {}; },
        [] {},
        [] {},
        [](std::string, std::string) { return support::ExpectedVoid{}; },
        [] {});

    // Ctrl+p toggles the path display on.
    static_cast<void>(component.handle_input(tui::KeyEvent{.key = "p", .ctrl = true}));
    const auto with_path = render_text(component);
    CHECK(with_path.find("path (on)") != std::string::npos);
    CHECK(with_path.find("/tmp/a.jsonl") != std::string::npos);

    // Ctrl+d enters the confirmation; Escape cancels it without deleting.
    static_cast<void>(component.handle_input(tui::KeyEvent{.key = "d", .ctrl = true}));
    CHECK(render_text(component).find("Delete session?") != std::string::npos);
    static_cast<void>(component.handle_input(tui::KeyEvent{.key = "escape"}));
    CHECK(render_text(component).find("Delete session?") == std::string::npos);
}

TEST_CASE(
    "session selector rename mode confirms through the inline input",
    "[coding_agent][tui][session-selector][issue409]") {
    auto theme = test_theme();
    auto keybindings = test_keybindings();
    std::vector<coding_agent::session_discovery::SessionInfo> sessions{
        make_session("a", std::nullopt, "alpha")};
    auto loader = [&] { return sessions; };
    std::string renamed_path;
    std::string renamed_value;
    coding_agent::tui::SessionSelectorComponent component(
        theme,
        keybindings,
        loader,
        [&] { return std::vector<coding_agent::session_discovery::SessionInfo>{}; },
        std::optional<std::filesystem::path>{},
        [](std::string) -> support::ExpectedVoid { return {}; },
        [] {},
        [] {},
        [&](std::string path, std::string name) {
            renamed_path = std::move(path);
            renamed_value = std::move(name);
            sessions[0].name = renamed_value;
            return support::ExpectedVoid{};
        },
        [] {});

    // Ctrl+r enters rename mode; the value is pre-filled, trimmed on confirm.
    static_cast<void>(component.handle_input(tui::KeyEvent{.key = "r", .ctrl = true}));
    CHECK(render_text(component).find("Rename Session") != std::string::npos);
    static_cast<void>(component.rename_input().handle_input(tui::KeyEvent{.key = "n"}));
    static_cast<void>(component.rename_input().handle_input(tui::KeyEvent{.key = "e"}));
    static_cast<void>(component.rename_input().handle_input(tui::KeyEvent{.key = "w"}));
    static_cast<void>(component.rename_input().handle_input(tui::KeyEvent{.key = "enter"}));
    CHECK(renamed_path == "/tmp/a.jsonl");
    CHECK(renamed_value == "new");
    // The list refreshed with the name.
    CHECK(render_text(component).find("new") != std::string::npos);
}

TEST_CASE("session selector search delegates the query and filtered rows to the SelectList",
        "[coding_agent][tui][session-selector][issue590]") {
    auto theme = test_theme();
    auto keybindings = test_keybindings();
    auto loader = [] {
        auto sessions = std::vector<coding_agent::session_discovery::SessionInfo>{};
        sessions.push_back(make_session("a", std::nullopt, "fix the parser bug"));
        sessions.push_back(make_session("b", std::nullopt, "refactor the loader"));
        sessions[0].all_messages_text = "fix the parser bug";
        sessions[1].all_messages_text = "refactor the loader";
        return sessions;
    };
    std::string selected_path;
    std::size_t cancellations = 0;
    coding_agent::tui::SessionSelectorComponent component(
            theme,
            keybindings,
            loader,
            [&] { return std::vector<coding_agent::session_discovery::SessionInfo>{}; },
            std::optional<std::filesystem::path>{},
            [&](std::string path) { selected_path = std::move(path); },
            [&cancellations] { ++cancellations; },
            [] {},
            [](std::string, std::string) { return support::ExpectedVoid{}; },
            [] {});

    // Empty query: the component's own tree rows render, with the search
    // line (owned by the SelectList) above them.
    auto text = render_text(component);
    CHECK(text.find("fix the parser bug") != std::string::npos);
    CHECK(text.find("refactor the loader") != std::string::npos);

    // Typing hands the view over to the SelectList: the query line and the
    // filtered flat rows come from it, the non-matching session disappears.
    for (const char character : std::string("parser")) {
        static_cast<void>(component.handle_input(tui::KeyEvent{.key = std::string(1, character)}));
    }
    text = render_text(component);
    CHECK(text.find("> parser") != std::string::npos);
    CHECK(text.find("fix the parser bug") != std::string::npos);
    CHECK(text.find("refactor the loader") == std::string::npos);

    // The search cursor reports the SelectList search row plus the
    // component-owned rows above it (blank, border, blank, title, hints,
    // blank), recorded at render rather than assumed.
    component.set_focused(true);
    const auto rendered = component.render(100);
    REQUIRE(rendered);
    const auto cursor = component.cursor_location();
    REQUIRE(cursor);
    CHECK(cursor->row == 7);
    CHECK(cursor->column == 8);

    // Enter resumes the filtered result through the select sink.
    static_cast<void>(component.handle_input(tui::KeyEvent{.key = "enter"}));
    CHECK(selected_path == "/tmp/a.jsonl");

    // Delete confirmation in the search view targets the filtered result.
    static_cast<void>(component.handle_input(tui::KeyEvent{.key = "d", .ctrl = true}));
    CHECK(render_text(component).find("Delete session?") != std::string::npos);
    static_cast<void>(component.handle_input(tui::KeyEvent{.key = "escape"}));
    CHECK(render_text(component).find("Delete session?") == std::string::npos);

    // Clearing the query hands the view back to the tree rows.
    for (std::size_t index = 0; index < 6; ++index) {
        static_cast<void>(component.handle_input(tui::KeyEvent{.key = "backspace"}));
    }
    text = render_text(component);
    CHECK(text.find("fix the parser bug") != std::string::npos);
    CHECK(text.find("refactor the loader") != std::string::npos);
    CHECK(text.find("> parser") == std::string::npos);

    // A query with no matches shows the domain empty-state row in place of
    // the SelectList's generic no-match text.
    for (const char character : std::string("zzz")) {
        static_cast<void>(component.handle_input(tui::KeyEvent{.key = std::string(1, character)}));
    }
    text = render_text(component);
    CHECK(text.find("> zzz") != std::string::npos);
    CHECK(text.find("No sessions in current folder. Press Tab to view all.") != std::string::npos);

    // Clearing that query returns to the component's own tree view again.
    for (std::size_t index = 0; index < 3; ++index) {
        static_cast<void>(component.handle_input(tui::KeyEvent{.key = "backspace"}));
    }
    text = render_text(component);
    CHECK(text.find("fix the parser bug") != std::string::npos);
    CHECK(text.find("refactor the loader") != std::string::npos);
    CHECK(text.find("> parser") == std::string::npos);

    // Escape from the search view cancels like the tree view (pi).
    for (const char character : std::string("pa")) {
        static_cast<void>(component.handle_input(tui::KeyEvent{.key = std::string(1, character)}));
    }
    static_cast<void>(component.handle_input(tui::KeyEvent{.key = "escape"}));
    CHECK(cancellations == 1);
}
