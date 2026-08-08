// P14: the tree selector component in isolation (pi `tree-selector.ts`) —
// the session topology rendering with indentation/connectors/gutters and the
// active-path marker, the five filter modes, search-while-typing, the
// fold/unfold branch navigation, the label edit flow with the inline Input,
// the label timestamp toggle, the entry copy, and the select/cancel sinks.

#include "../../../third_party/catch2/catch_test_macros.hpp"

#include "coding_agent/tui/KeybindingCatalog.hpp"
#include "coding_agent/tui/Theme.hpp"
#include "coding_agent/tui/TreeSelector.hpp"

#include <cch/tui/Keybindings.hpp>

#include <chrono>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] std::shared_ptr<const tui::KeybindingRegistry> test_keybindings() {
    auto definitions = tui::builtin_tui_keybinding_definitions();
    const std::vector<std::string_view> actions{
        "app.tree.foldOrUp",
        "app.tree.unfoldOrDown",
        "app.tree.editLabel",
        "app.tree.toggleLabelTimestamp",
        "app.tree.filter.default",
        "app.tree.filter.noTools",
        "app.tree.filter.userOnly",
        "app.tree.filter.labeledOnly",
        "app.tree.filter.all",
        "app.tree.filter.cycleForward",
        "app.tree.filter.cycleBackward",
        "app.message.copy",
    };
    auto app_definitions = coding_agent::tui::baseline_application_keybindings(
        actions, tui::native_keybinding_platform());
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

[[nodiscard]] std::string render_text(coding_agent::tui::TreeSelectorComponent& component) {
    auto rendered = component.render(100);
    REQUIRE(rendered);
    std::string text;
    for (const auto& line : rendered->lines) {
        text += strip_ansi(line);
        text.push_back('\n');
    }
    return text;
}

using Node = harness::session::SessionTreeNode;

[[nodiscard]] Node with_parent(Node node, std::string parent_id) {
    node.entry.parent_id = std::move(parent_id);
    return node;
}

[[nodiscard]] Node user_node(std::string id, std::string text, std::int64_t ts = 0) {
    Node node;
    node.entry.entry_id = std::move(id);
    node.entry.kind = harness::session::SessionEntryKind::Message;
    node.entry.timestamp = ts;
    node.entry.message = ai::MessageVariant{ai::user_text_message(std::move(text))};
    return node;
}

[[nodiscard]] Node assistant_node(std::string id, std::string text, std::int64_t ts = 0) {
    Node node;
    node.entry.entry_id = std::move(id);
    node.entry.kind = harness::session::SessionEntryKind::Message;
    node.entry.timestamp = ts;
    auto message = ai::assistant_text_message(std::move(text));
    message.api = "fake-api";
    message.provider = "fake";
    message.model = "fake-model";
    node.entry.message = ai::MessageVariant{std::move(message)};
    return node;
}

[[nodiscard]] Node tool_node(std::string id, std::string tool_name, std::int64_t ts = 0) {
    Node node;
    node.entry.entry_id = std::move(id);
    node.entry.kind = harness::session::SessionEntryKind::Message;
    node.entry.timestamp = ts;
    ai::ToolResultMessage tool;
    tool.tool_call_id = "call-1";
    tool.tool_name = std::move(tool_name);
    tool.content.push_back(ai::text_content("result text"));
    node.entry.message = ai::MessageVariant{std::move(tool)};
    return node;
}

[[nodiscard]] Node thinking_node(std::string id, std::int64_t ts = 0) {
    Node node;
    node.entry.entry_id = std::move(id);
    node.entry.kind = harness::session::SessionEntryKind::ThinkingLevelChange;
    node.entry.timestamp = ts;
    node.entry.value = harness::session::ThinkingLevelChangeValue{.thinking_level = "high"};
    return node;
}

[[nodiscard]] Node model_change_node(std::string id, std::int64_t ts = 0) {
    Node node;
    node.entry.entry_id = std::move(id);
    node.entry.kind = harness::session::SessionEntryKind::ModelChange;
    node.entry.timestamp = ts;
    node.entry.value = harness::session::ModelChangeValue{
        .provider = "alpha",
        .model_id = "alpha-1",
    };
    return node;
}

/// A linear conversation with a branch: u0 -> a0 -> u1 (labeled) -> a1, and
/// a second child under u0 (u0 -> u2), so the tree renders connectors and
/// the labeled entry sits mid-chain. The leaf is a1.
[[nodiscard]] std::vector<Node> sample_tree(std::optional<std::string>& leaf_id) {
    auto u0 = user_node("u0", "first question", 1000);
    auto a0 = with_parent(assistant_node("a0", "first answer", 2000), "u0");
    auto u1 = with_parent(user_node("u1", "second question", 3000), "a0");
    u1.label = "important";
    u1.label_timestamp = 4000;
    auto a1 = with_parent(assistant_node("a1", "second answer", 5000), "u1");
    auto u2 = with_parent(user_node("u2", "side branch", 6000), "u0");

    u0.children.push_back(std::move(a0));
    u0.children.push_back(std::move(u2));
    u0.children[0].children.push_back(std::move(u1));
    u0.children[0].children[0].children.push_back(std::move(a1));

    leaf_id = "a1";
    return {std::move(u0)};
}

/// The captured sink calls from one component lifetime.
struct SinkCalls {
    std::vector<std::string> selects;
    std::vector<std::string> labels;
    std::vector<std::optional<std::string>> copies;
    int cancels{0};
};

} // namespace

TEST_CASE(
    "tree selector renders the session topology with connectors and the counter",
    "[coding_agent][tui][tree-selector][issue410]") {
    auto theme = test_theme();
    auto keybindings = test_keybindings();
    std::optional<std::string> leaf_id;
    auto tree = sample_tree(leaf_id);
    coding_agent::tui::TreeSelectorComponent component(
        theme,
        keybindings,
        std::move(tree),
        *leaf_id,
        /*terminal_height=*/20,
        [](std::string) {},
        [] {},
        [](std::string, std::optional<std::string>) {},
        [](std::optional<std::string>) {},
        [] {});

    const auto text = render_text(component);
    // pi's composition: border, bold title, help rows, search line, tree.
    CHECK(text.find("  Session Tree") != std::string::npos);
    CHECK(text.find("Type to search:") != std::string::npos);
    CHECK(text.find("move") != std::string::npos);
    CHECK(text.find("filters") != std::string::npos);
    // Rows: the root carries no connector; the branch children carry the
    // connector/fold glyphs, the continuation gutter, and the label.
    CHECK(text.find("• user: first question") != std::string::npos);
    CHECK(text.find("├⊟ • assistant: first answer") != std::string::npos);
    CHECK(text.find("• [important] user: second question") != std::string::npos);
    CHECK(text.find("• assistant: second answer") != std::string::npos);
    CHECK(text.find("└─ user: side branch") != std::string::npos);
    // The continuation gutter renders under the first branch.
    CHECK(text.find("│") != std::string::npos);
    // The counter footer (4/5: the leaf sits at index 3) and the selection
    // cursor on the leaf.
    CHECK(text.find("(4/5)") != std::string::npos);
}

TEST_CASE(
    "tree selector filter modes and search narrow the visible rows",
    "[coding_agent][tui][tree-selector][issue410]") {
    auto theme = test_theme();
    auto keybindings = test_keybindings();
    std::optional<std::string> leaf_id;
    // The sample branch tree plus settings/bookkeeping entries hidden in the
    // default view (the labeled u1 sits mid-chain).
    auto tree = sample_tree(leaf_id);
    auto& root = tree.front();
    auto thinking = with_parent(thinking_node("thinking-1", 1500), "u0");
    auto model = with_parent(model_change_node("model-1", 1600), "u0");
    root.children.push_back(std::move(thinking));
    root.children.push_back(std::move(model));
    leaf_id = "model-1";

    coding_agent::tui::TreeSelectorComponent component(
        theme,
        keybindings,
        std::vector<Node>{std::move(root)},
        *leaf_id,
        /*terminal_height=*/20,
        [](std::string) {},
        [] {},
        [](std::string, std::optional<std::string>) {},
        [](std::optional<std::string>) {},
        [] {});

    // The default view hides the thinking/model entries.
    auto text = render_text(component);
    CHECK(text.find("[thinking: high]") == std::string::npos);
    CHECK(text.find("[model: alpha-1]") == std::string::npos);

    // Ctrl+a shows everything.
    component.handle_input(tui::KeyEvent{.key = "a", .ctrl = true});
    text = render_text(component);
    CHECK(text.find("[thinking: high]") != std::string::npos);
    CHECK(text.find("[model: alpha-1]") != std::string::npos);
    CHECK(text.find("[all]") != std::string::npos);

    // Ctrl+u keeps user messages only.
    component.handle_input(tui::KeyEvent{.key = "u", .ctrl = true});
    text = render_text(component);
    CHECK(text.find("user: first question") != std::string::npos);
    CHECK(text.find("assistant:") == std::string::npos);
    CHECK(text.find("[user]") != std::string::npos);

    // Ctrl+l keeps labeled entries only (the mid-chain label).
    component.handle_input(tui::KeyEvent{.key = "l", .ctrl = true});
    text = render_text(component);
    CHECK(text.find("[important]") != std::string::npos);
    CHECK(text.find("user: first question") == std::string::npos);
    CHECK(text.find("[labeled]") != std::string::npos);

    // Ctrl+d returns to the default view.
    component.handle_input(tui::KeyEvent{.key = "d", .ctrl = true});
    text = render_text(component);
    CHECK(text.find("[labeled]") == std::string::npos);

    // Search narrows to matching rows (search-while-typing).
    component.handle_input(tui::KeyEvent{.key = "s"});
    component.handle_input(tui::KeyEvent{.key = "e"});
    component.handle_input(tui::KeyEvent{.key = "c"});
    text = render_text(component);
    CHECK(text.find("second question") != std::string::npos);
    CHECK(text.find("first question") == std::string::npos);

    // Escape with an active search clears it instead of cancelling.
    component.handle_input(tui::KeyEvent{.key = "escape"});
    text = render_text(component);
    CHECK(text.find("first question") != std::string::npos);
}

TEST_CASE(
    "tree selector folds and unfolds branches with the branch actions",
    "[coding_agent][tui][tree-selector][issue410]") {
    auto theme = test_theme();
    auto keybindings = test_keybindings();
    std::optional<std::string> leaf_id;
    auto tree = sample_tree(leaf_id);
    coding_agent::tui::TreeSelectorComponent component(
        theme,
        keybindings,
        std::move(tree),
        *leaf_id,
        /*terminal_height=*/20,
        [](std::string) {},
        [] {},
        [](std::string, std::optional<std::string>) {},
        [](std::optional<std::string>) {},
        [] {});

    // The leaf is selected; move up to the branch point a0, then fold it
    // (ctrl+left): its descendants disappear.
    component.handle_input(tui::KeyEvent{.key = "up"});
    component.handle_input(tui::KeyEvent{.key = "up"});
    component.handle_input(tui::KeyEvent{.key = "up"});
    component.handle_input(tui::KeyEvent{.key = "left", .ctrl = true});
    auto text = render_text(component);
    CHECK(text.find("second answer") == std::string::npos);
    CHECK(text.find("second question") == std::string::npos);

    // Unfold (ctrl+right) restores the children.
    component.handle_input(tui::KeyEvent{.key = "right", .ctrl = true});
    text = render_text(component);
    CHECK(text.find("second answer") != std::string::npos);
}

TEST_CASE(
    "tree selector edits a label through the inline input and reports it",
    "[coding_agent][tui][tree-selector][issue410]") {
    auto theme = test_theme();
    auto keybindings = test_keybindings();
    std::optional<std::string> leaf_id;
    auto tree = sample_tree(leaf_id);
    SinkCalls calls;
    coding_agent::tui::TreeSelectorComponent component(
        theme,
        keybindings,
        std::move(tree),
        *leaf_id,
        /*terminal_height=*/20,
        [&](std::string id) { calls.selects.push_back(std::move(id)); },
        [&] { ++calls.cancels; },
        [&](std::string id, std::optional<std::string> label) {
            calls.labels.push_back(std::move(id));
            calls.labels.push_back(label.value_or(""));
        },
        [&](std::optional<std::string> text) { calls.copies.push_back(std::move(text)); },
        [] {});

    // Move up to u1 (already labeled "important") and press shift+l: the
    // label input opens pre-filled.
    component.handle_input(tui::KeyEvent{.key = "up"});
    component.handle_input(tui::KeyEvent{.key = "l", .shift = true});
    auto text = render_text(component);
    CHECK(text.find("Label (empty to remove):") != std::string::npos);
    CHECK(text.find("important") != std::string::npos);

    // Clear the pre-fill, type a new label, and save with Enter.
    component.label_input().set_value("");
    for (const char letter : std::string{"reviewed"}) {
        component.handle_input(tui::KeyEvent{.key = std::string(1, letter)});
    }
    component.handle_input(tui::KeyEvent{.key = "enter"});
    REQUIRE(calls.labels.size() == 2);
    CHECK(calls.labels[0] == "u1");
    CHECK(calls.labels[1] == "reviewed");
    text = render_text(component);
    CHECK(text.find("Label (empty to remove):") == std::string::npos);
    CHECK(text.find("[reviewed] user: second question") != std::string::npos);

    // Escape cancels the label editor without a change.
    component.handle_input(tui::KeyEvent{.key = "l", .shift = true});
    component.handle_input(tui::KeyEvent{.key = "escape"});
    CHECK(calls.labels.size() == 2);
}

TEST_CASE(
    "tree selector copies the selected entry and reports no-text entries",
    "[coding_agent][tui][tree-selector][issue410]") {
    auto theme = test_theme();
    auto keybindings = test_keybindings();
    std::optional<std::string> leaf_id;
    auto tree = sample_tree(leaf_id);
    SinkCalls calls;
    coding_agent::tui::TreeSelectorComponent component(
        theme,
        keybindings,
        std::move(tree),
        *leaf_id,
        /*terminal_height=*/20,
        [&](std::string id) { calls.selects.push_back(std::move(id)); },
        [&] { ++calls.cancels; },
        [](std::string, std::optional<std::string>) {},
        [&](std::optional<std::string> text) { calls.copies.push_back(std::move(text)); },
        [] {});

    // The leaf (assistant a1) copies its text.
    component.handle_input(tui::KeyEvent{.key = "x", .ctrl = true});
    REQUIRE(calls.copies.size() == 1);
    REQUIRE(calls.copies.back().has_value());
    CHECK(*calls.copies.back() == "second answer");

    // The label entry has no copyable text: move up to the labeled user
    // message and copy it (its text copies); a settings entry reports
    // nullopt. Add a model-change entry and select it in the all view.
    std::optional<std::string> other_leaf;
    auto model = model_change_node("model-1");
    auto u0 = with_parent(user_node("u0", "first question", 1000), std::string{});
    auto model_with_parent = with_parent(std::move(model), "u0");
    u0.children.push_back(std::move(model_with_parent));
    other_leaf = "model-1";
    coding_agent::tui::TreeSelectorComponent component2(
        theme,
        keybindings,
        std::vector<Node>{std::move(u0)},
        *other_leaf,
        /*terminal_height=*/20,
        [](std::string) {},
        [] {},
        [](std::string, std::optional<std::string>) {},
        [&](std::optional<std::string> text) { calls.copies.push_back(std::move(text)); },
        [] {});
    component2.handle_input(tui::KeyEvent{.key = "a", .ctrl = true});
    // The initial selection walked up to the visible root; move down to the
    // model-change entry and copy it: a settings entry reports nullopt.
    component2.handle_input(tui::KeyEvent{.key = "down"});
    component2.handle_input(tui::KeyEvent{.key = "x", .ctrl = true});
    REQUIRE(calls.copies.size() == 2);
    CHECK_FALSE(calls.copies.back().has_value());
}

TEST_CASE(
    "tree selector selects on Enter, cancels on Escape, and toggles label timestamps",
    "[coding_agent][tui][tree-selector][issue410]") {
    auto theme = test_theme();
    auto keybindings = test_keybindings();
    std::optional<std::string> leaf_id;
    auto tree = sample_tree(leaf_id);
    SinkCalls calls;
    coding_agent::tui::TreeSelectorComponent component(
        theme,
        keybindings,
        std::move(tree),
        *leaf_id,
        /*terminal_height=*/20,
        [&](std::string id) { calls.selects.push_back(std::move(id)); },
        [&] { ++calls.cancels; },
        [](std::string, std::optional<std::string>) {},
        [](std::optional<std::string>) {},
        [] {});

    // Enter selects the current leaf.
    component.handle_input(tui::KeyEvent{.key = "enter"});
    REQUIRE(calls.selects.size() == 1);
    CHECK(calls.selects[0] == "a1");

    // Escape cancels (no active search).
    component.handle_input(tui::KeyEvent{.key = "escape"});
    CHECK(calls.cancels == 1);

    // Shift+t toggles the label timestamp footer hint.
    component.handle_input(tui::KeyEvent{.key = "t", .shift = true});
    const auto text = render_text(component);
    CHECK(text.find("[+label time]") != std::string::npos);
    component.handle_input(tui::KeyEvent{.key = "t", .shift = true});
    CHECK(render_text(component).find("[+label time]") == std::string::npos);
}

TEST_CASE(
    "tree selector renders the empty filtered state",
    "[coding_agent][tui][tree-selector][issue410]") {
    auto theme = test_theme();
    auto keybindings = test_keybindings();
    // A tree whose only entry is hidden by the default filter.
    auto thinking = thinking_node("thinking-1");
    coding_agent::tui::TreeSelectorComponent component(
        theme,
        keybindings,
        std::vector<Node>{std::move(thinking)},
        "thinking-1",
        /*terminal_height=*/20,
        [](std::string) {},
        [] {},
        [](std::string, std::optional<std::string>) {},
        [](std::optional<std::string>) {},
        [] {});

    const auto text = render_text(component);
    CHECK(text.find("No entries found") != std::string::npos);
    CHECK(text.find("(0/0)") != std::string::npos);
}

TEST_CASE(
    "tree selector renders branch-summary and compaction entries from pi-created sessions",
    "[coding_agent][tui][tree-selector][issue410]") {
    auto theme = test_theme();
    auto keybindings = test_keybindings();

    Node root;
    root.entry.entry_id = "u0";
    root.entry.kind = harness::session::SessionEntryKind::Message;
    root.entry.message = ai::MessageVariant{ai::user_text_message("question")};
    Node summary;
    summary.entry.entry_id = "bs1";
    summary.entry.parent_id = "u0";
    summary.entry.kind = harness::session::SessionEntryKind::BranchSummary;
    summary.entry.value = harness::session::BranchSummaryEntryValue{
        .from_id = "old-leaf",
        .summary = "abandoned branch notes",
    };
    Node compaction;
    compaction.entry.entry_id = "cp1";
    compaction.entry.parent_id = "bs1";
    compaction.entry.kind = harness::session::SessionEntryKind::Compaction;
    compaction.entry.value = harness::session::CompactionEntryValue{
        .summary = "compacted history",
        .tokens_before = 15234,
    };
    root.children.push_back(std::move(summary));
    root.children[0].children.push_back(std::move(compaction));

    coding_agent::tui::TreeSelectorComponent component(
        theme,
        keybindings,
        std::vector<Node>{std::move(root)},
        "cp1",
        /*terminal_height=*/20,
        [](std::string) {},
        [] {},
        [](std::string, std::optional<std::string>) {},
        [](std::optional<std::string>) {},
        [] {});

    const auto text = render_text(component);
    // Branch summarization generation stays absent, but pi-authored entries
    // render: the branch-summary text and the rounded compaction token
    // count (pi: `Math.round(entry.tokensBefore / 1000)`).
    CHECK(text.find("[branch summary]: abandoned branch notes") != std::string::npos);
    CHECK(text.find("[compaction: 15k tokens]") != std::string::npos);
}
