#include <cch/tui/Overlay.hpp>
#include <cch/tui/SelectList.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>
#include <cch/tui/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

std::vector<cch::tui::SelectItem> make_items(std::size_t count) {
    std::vector<cch::tui::SelectItem> items;
    for (std::size_t index = 0; index < count; ++index) {
        items.push_back(cch::tui::SelectItem{
            .value = "item" + std::to_string(index),
            .label = "Item " + std::to_string(index),
            .description = "description " + std::to_string(index),
        });
    }
    return items;
}

} // namespace

TEST_CASE("SelectList renders empty and unmatched filters through VirtualTerminal", "[tui][select-list][issue52]") {
    cch::tui::VirtualTerminal terminal({.columns = 30, .rows = 3});
    cch::tui::Tui tui(terminal);
    auto list = std::make_unique<cch::tui::SelectList>(std::vector<cch::tui::SelectItem>{});
    auto* list_ptr = list.get();
    REQUIRE(tui.add_child(std::move(list)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(list_ptr));
    REQUIRE(tui.render());
    CHECK(terminal.screen()[0].find("No matching commands") != std::string::npos);

    list_ptr->set_filter("missing");
    REQUIRE(tui.render());
    CHECK(terminal.screen()[0].find("No matching commands") != std::string::npos);
}

TEST_CASE("SelectList pages large bounded lists and adapts descriptions after resize", "[tui][select-list][issue52]") {
    cch::tui::VirtualTerminal terminal({.columns = 60, .rows = 5});
    cch::tui::Tui tui(terminal);
    auto list = std::make_unique<cch::tui::SelectList>(
        make_items(12),
        cch::tui::SelectListOptions{.max_visible = 3});
    auto* list_ptr = list.get();
    REQUIRE(tui.add_child(std::move(list)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(list_ptr));
    REQUIRE(tui.render());
    REQUIRE(list_ptr->selected_item());
    CHECK(list_ptr->selected_item()->value == "item0");
    CHECK(terminal.screen()[0].find("description 0") != std::string::npos);

    REQUIRE(terminal.inject_input("\x1b[6~"));
    REQUIRE(tui.render());
    REQUIRE(list_ptr->selected_item());
    CHECK(list_ptr->selected_item()->value == "item3");
    CHECK(terminal.screen()[3].find("(4/12)") != std::string::npos);

    REQUIRE(terminal.inject_resize({.columns = 30, .rows = 4}));
    REQUIRE(tui.render());
    for (const auto& row : terminal.screen()) {
        CHECK(row.find("description") == std::string::npos);
    }
}

TEST_CASE("SelectList filters navigates wraps selects and cancels with semantic keys", "[tui][select-list][issue52]") {
    std::string selected;
    std::string changed;
    std::size_t cancellations = 0;
    cch::tui::SelectList list(
        {
            {.value = "alpha", .label = "Alpha"},
            {.value = "alpine", .label = "Alpine"},
            {.value = "beta", .label = "Beta"},
        },
        cch::tui::SelectListOptions{
            .max_visible = 2,
            .on_select = [&selected](const cch::tui::SelectItem& item) -> cch::support::ExpectedVoid {
                selected = item.value;
                return {};
            },
            .on_cancel = [&cancellations]() -> cch::support::ExpectedVoid {
                ++cancellations;
                return {};
            },
            .on_selection_change = [&changed](const cch::tui::SelectItem& item) -> cch::support::ExpectedVoid {
                changed = item.value;
                return {};
            },
        });

    list.set_filter("al");
    list.handle_input(cch::tui::KeyEvent{.key = "up"});
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == "alpine");
    CHECK(changed == "alpine");
    list.handle_input(cch::tui::KeyEvent{.key = "enter"});
    CHECK(selected == "alpine");
    list.handle_input(cch::tui::KeyEvent{.key = "escape"});
    CHECK(cancellations == 1);
}

TEST_CASE("SelectList normalizes and aligns descriptions within configured columns", "[tui][select-list][issue52]") {
    cch::tui::SelectList list(
        {
            {.value = "short", .label = "Short", .description = "line one\nline two"},
            {
                .value = "long",
                .label = "A very long primary label",
                .description = "second description",
            },
        },
        cch::tui::SelectListOptions{
            .layout = {
                .min_primary_column_width = 12,
                .max_primary_column_width = 20,
            },
        });

    const auto rendered = list.render(80);
    REQUIRE(rendered);
    REQUIRE(rendered->lines.size() == 2);
    CHECK(rendered->lines[0].find('\n') == std::string::npos);
    CHECK(rendered->lines[0].find("line one line two") != std::string::npos);
    const auto first_description = rendered->lines[0].find("line one");
    const auto second_description = rendered->lines[1].find("second description");
    REQUIRE(first_description != std::string::npos);
    REQUIRE(second_description != std::string::npos);
    CHECK(cch::tui::visible_width(rendered->lines[0].substr(0, first_description)) ==
          cch::tui::visible_width(rendered->lines[1].substr(0, second_description)));
}

TEST_CASE("SelectList dispatches configured keys from its effective registry", "[tui][select-list][issue57]") {
    cch::tui::KeybindingResolutionRequest request;
    request.definitions = cch::tui::builtin_tui_keybinding_definitions();
    request.overrides = {{.id = "tui.select.confirm", .keys = {"f2"}}};
    const auto keybindings = cch::tui::resolve_keybindings(std::move(request));
    REQUIRE(keybindings);

    std::size_t selections = 0;
    cch::tui::SelectList list(
        make_items(1),
        cch::tui::SelectListOptions{
            .on_select = [&selections](const cch::tui::SelectItem&) -> cch::support::ExpectedVoid {
                ++selections;
                return {};
            },
            .keybindings = keybindings->registry,
        });
    list.handle_input(cch::tui::KeyEvent{.key = "enter"});
    CHECK(selections == 0);
    list.handle_input(cch::tui::KeyEvent{.key = "f2"});
    CHECK(selections == 1);
}

TEST_CASE("SelectList resolves a key shared by two actions in dispatch order", "[tui][select-list]") {
    // f9 claims up and confirm; selection movement leads pi's select-list chain.
    auto registry = std::make_shared<const cch::tui::KeybindingRegistry>(std::vector<cch::tui::EffectiveKeybinding>{
            {.id = "tui.select.up", .keys = {"f9"}},
            {.id = "tui.select.confirm", .keys = {"f9", "enter"}},
    });
    std::size_t selections = 0;
    cch::tui::SelectList list(make_items(3),
            cch::tui::SelectListOptions{
                    .on_select = [&selections](const cch::tui::SelectItem&) -> cch::support::ExpectedVoid {
                        ++selections;
                        return {};
                    },
                    .keybindings = std::move(registry),
            });
    list.handle_input(cch::tui::KeyEvent{.key = "f9"});
    CHECK(selections == 0);
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == "item2"); // up wraps to the tail
    list.handle_input(cch::tui::KeyEvent{.key = "enter"});
    CHECK(selections == 1);
}

TEST_CASE("SelectList renders descriptions in an overlay with bounded rows", "[tui][select-list][overlay][issue52]") {
    cch::tui::VirtualTerminal terminal({.columns = 70, .rows = 6});
    cch::tui::Tui tui(terminal);
    cch::tui::OverlayOptions overlay_options;
    overlay_options.size_constraints.max_height = 3;
    auto overlay = std::make_unique<cch::tui::Overlay>(std::move(overlay_options));
    auto list = std::make_unique<cch::tui::SelectList>(make_items(8), cch::tui::SelectListOptions{.max_visible = 2});
    REQUIRE(overlay->add_child(std::move(list)));
    REQUIRE(overlay->focus_first());
    auto* overlay_ptr = overlay.get();
    REQUIRE(tui.add_overlay(std::move(overlay)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(overlay_ptr));
    REQUIRE(tui.render());

    CHECK(terminal.screen()[0].find("Item 0") != std::string::npos);
    CHECK(terminal.screen()[0].find("description 0") != std::string::npos);
    CHECK(terminal.screen()[2].find("(1/8)") != std::string::npos);
}

// ---------------------------------------------------------------------------
// #586: embedded search, fuzzy ranking, chrome framing, cursor reporting.
// ---------------------------------------------------------------------------

namespace {

void type_into(cch::tui::SelectList& list, std::string_view text) {
    for (const auto& character : text) {
        list.handle_input(cch::tui::KeyEvent{.key = std::string(1, character)});
    }
}

std::string border_rule(std::size_t width) {
    std::string rule;
    rule.reserve(width * 3);
    for (std::size_t index = 0; index < width; ++index)
        rule += "\u2500";
    return rule;
}

} // namespace

TEST_CASE("SelectList search mode typing re-ranks and clamps the selection to the top match",
        "[tui][select-list][issue586]") {
    std::vector<std::string> changed;
    cch::tui::SelectList list(
            {
                    {.value = "first", .label = "First thing"},
                    {.value = "alpha", .label = "Alpha"},
                    {.value = "alpine", .label = "Alpine"},
                    {.value = "beta", .label = "Beta"},
            },
            cch::tui::SelectListOptions{
                    .on_selection_change = [&changed](const cch::tui::SelectItem& item) -> cch::support::ExpectedVoid {
                        changed.push_back(item.value);
                        return {};
                    },
                    .enable_search = true,
            });

    // Empty query keeps every item in original order with the top selected.
    CHECK(list.search_query().empty());
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == "first");

    // "alpi" fuzzy-matches only Alpine; the selection clamps to the top match
    // and the re-rank notifies the change.
    type_into(list, "alpi");
    CHECK(list.search_query() == "alpi");
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == "alpine");
    REQUIRE_FALSE(changed.empty());
    CHECK(changed.back() == "alpine");

    // Extending the query past every item empties the filter; navigation is
    // inert and the no-match row renders in place of the list.
    type_into(list, "x");
    CHECK(list.search_query() == "alpix");
    CHECK_FALSE(list.selected_item());
    list.handle_input(cch::tui::KeyEvent{.key = "down"});
    CHECK_FALSE(list.selected_item());
    const auto rendered = list.render(60);
    REQUIRE(rendered);
    CHECK(rendered->lines[1].find("No matching commands") != std::string::npos);

    // Backspace restores the previous ranking (selection back on the match).
    list.handle_input(cch::tui::KeyEvent{.key = "backspace"});
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == "alpine");
    CHECK(changed.back() == "alpine");

    // Clearing the query restores all items in original order; the selection
    // deterministically lands on the first item (top rule).
    for (int index = 0; index < 4; ++index) {
        list.handle_input(cch::tui::KeyEvent{.key = "backspace"});
    }
    CHECK(list.search_query().empty());
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == "first");
    CHECK(changed.back() == "first");
}

TEST_CASE(
        "SelectList search mode ranks better fuzzy matches above the original order", "[tui][select-list][issue586]") {
    cch::tui::SelectList list(
            {
                    {.value = "scattered", .label = "f_o_o_bar"},
                    {.value = "consecutive", .label = "foobar"},
            },
            cch::tui::SelectListOptions{.enable_search = true});

    // Both items fuzzy-match "foo"; the consecutive occurrence outranks the
    // scattered one even though it was second in the original order.
    type_into(list, "foo");
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == "consecutive");
}

TEST_CASE("SelectList search mode matches label description and hidden search text", "[tui][select-list][issue586]") {
    // The default searchable text joins the label, description and value, so
    // the description-only word matches.
    cch::tui::SelectList by_description(
            {
                    {.value = "metrics", .label = "Metrics", .description = "grafana dashboard"},
            },
            cch::tui::SelectListOptions{.enable_search = true});
    type_into(by_description, "grafana");
    REQUIRE(by_description.selected_item());
    CHECK(by_description.selected_item()->value == "metrics");

    // The value participates in the default searchable text as well.
    cch::tui::SelectList by_value(
            {
                    {.value = "grafana", .label = "Metrics dashboard"},
            },
            cch::tui::SelectListOptions{.enable_search = true});
    type_into(by_value, "grafana");
    REQUIRE(by_value.selected_item());
    CHECK(by_value.selected_item()->value == "grafana");

    // search_text replaces the item's searchable text entirely: the hidden
    // token matches, while the visible label does not.
    cch::tui::SelectList hidden(
            {
                    {.value = "hidden", .label = "Visible row", .search_text = "needle in haystack"},
            },
            cch::tui::SelectListOptions{.enable_search = true});
    type_into(hidden, "needle");
    REQUIRE(hidden.selected_item());
    CHECK(hidden.selected_item()->value == "hidden");

    type_into(hidden, " visible");
    CHECK(hidden.search_query() == "needle visible");
    CHECK_FALSE(hidden.selected_item());
}

TEST_CASE("SelectList search mode confirm selects the filtered item and cancel exits", "[tui][select-list][issue586]") {
    std::string selected;
    std::size_t cancellations = 0;
    cch::tui::SelectList list(
            {
                    {.value = "alpha", .label = "Alpha"},
                    {.value = "beta", .label = "Beta"},
                    {.value = "gamma", .label = "Gamma"},
            },
            cch::tui::SelectListOptions{
                    .on_select = [&selected](const cch::tui::SelectItem& item) -> cch::support::ExpectedVoid {
                        selected = item.value;
                        return {};
                    },
                    .on_cancel = [&cancellations]() -> cch::support::ExpectedVoid {
                        ++cancellations;
                        return {};
                    },
                    .enable_search = true,
            });

    // Confirm fires on_select with the currently filtered top match.
    type_into(list, "ga");
    list.handle_input(cch::tui::KeyEvent{.key = "enter"});
    CHECK(selected == "gamma");
    CHECK(list.search_query() == "ga");

    // Escape and Ctrl+C both fire on_cancel without clearing the query.
    list.handle_input(cch::tui::KeyEvent{.key = "escape"});
    list.handle_input(cch::tui::KeyEvent{.key = "c", .ctrl = true});
    CHECK(cancellations == 2);
    CHECK(list.search_query() == "ga");

    // Confirm on an empty filter (no matches) is a no-op; cancel still fires.
    type_into(list, "zz");
    CHECK_FALSE(list.selected_item());
    list.handle_input(cch::tui::KeyEvent{.key = "enter"});
    CHECK(selected == "gamma");
    list.handle_input(cch::tui::KeyEvent{.key = "escape"});
    CHECK(cancellations == 3);

    // Clearing back to a matching query restores confirm-on-filtered-item.
    list.handle_input(cch::tui::KeyEvent{.key = "backspace"});
    list.handle_input(cch::tui::KeyEvent{.key = "backspace"});
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == "gamma");
    list.handle_input(cch::tui::KeyEvent{.key = "enter"});
    CHECK(selected == "gamma");
}

TEST_CASE("SelectList search mode pages through the filtered window and reports the indicator",
        "[tui][select-list][issue586]") {
    cch::tui::VirtualTerminal terminal({.columns = 60, .rows = 10});
    cch::tui::Tui tui(terminal);
    auto list = std::make_unique<cch::tui::SelectList>(
            make_items(12), cch::tui::SelectListOptions{.max_visible = 3, .enable_search = true});
    auto* list_ptr = list.get();
    REQUIRE(tui.add_child(std::move(list)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(list_ptr));

    // Printable typing flows through the decoded event chain into the input.
    REQUIRE(terminal.inject_input("it"));
    REQUIRE(tui.render());
    CHECK(list_ptr->search_query() == "it");
    REQUIRE(list_ptr->selected_item());
    CHECK(list_ptr->selected_item()->value == "item0");
    CHECK(terminal.screen()[0].find("> it") != std::string::npos);

    // Page keys step by max_visible inside the filtered set.
    REQUIRE(terminal.inject_input("\x1b[6~"));
    REQUIRE(tui.render());
    REQUIRE(list_ptr->selected_item());
    CHECK(list_ptr->selected_item()->value == "item3");
    CHECK(terminal.screen()[2].find("Item 3") != std::string::npos);
    CHECK(terminal.screen()[4].find("(4/12)") != std::string::npos);

    // The indicator reflects the filtered count after a narrowing query.
    REQUIRE(terminal.inject_input("em3"));
    REQUIRE(tui.render());
    CHECK(list_ptr->search_query() == "item3");
    REQUIRE(list_ptr->selected_item());
    CHECK(list_ptr->selected_item()->value == "item3");
}

TEST_CASE("SelectList search filter hook fully replaces the fuzzy ranking", "[tui][select-list][issue586]") {
    std::vector<std::string> queries;
    cch::tui::SelectSearchFilterHook hook =
            [&queries](std::string_view query,
                    const std::vector<cch::tui::SelectItem>& items) -> std::vector<std::size_t> {
        queries.emplace_back(query);
        std::vector<std::size_t> result;
        result.reserve(items.size());
        for (std::size_t index = items.size(); index > 0; --index)
            result.push_back(index - 1);
        return result;
    };
    cch::tui::SelectList list(
            {
                    {.value = "alpha", .label = "Alpha"},
                    {.value = "beta", .label = "Beta"},
                    {.value = "gamma", .label = "Gamma"},
            },
            cch::tui::SelectListOptions{
                    .enable_search = true,
                    .search_filter_hook = std::move(hook),
            });

    // The hook is consulted on every ranking recomputation, including the
    // construction-time empty query (empty-query contract: all indices).
    REQUIRE(queries.size() == 1);
    CHECK(queries[0].empty());

    // Reversed display order and top-ranked selection follow the hook.
    const auto first = list.render(50);
    REQUIRE(first);
    REQUIRE(first->lines.size() == 4); // search row + three items
    CHECK(first->lines[1].find("Gamma") != std::string::npos);
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == "gamma");

    // The hook fully replaces fuzzy matching: a query that would filter
    // everything out under fuzzy ranking keeps the hook's reversed list.
    type_into(list, "alpha");
    CHECK(queries.back() == "alpha");
    const auto second = list.render(50);
    REQUIRE(second);
    REQUIRE(second->lines.size() == 4);
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == "gamma");

    // Out-of-range hook indices are dropped (defensive) and never crash.
    std::size_t invocations = 0;
    cch::tui::SelectList guarded(
            {
                    {.value = "alpha", .label = "Alpha"},
                    {.value = "beta", .label = "Beta"},
            },
            cch::tui::SelectListOptions{
                    .enable_search = true,
                    .search_filter_hook =
                            [&invocations](std::string_view,
                                    const std::vector<cch::tui::SelectItem>& items) -> std::vector<std::size_t> {
                        ++invocations;
                        return {items.size()}; // always out of range
                    },
            });
    type_into(guarded, "a");
    CHECK(invocations >= 2);
    CHECK_FALSE(guarded.selected_item());
    const auto guarded_render = guarded.render(50);
    REQUIRE(guarded_render);
    CHECK(guarded_render->lines[1].find("No matching commands") != std::string::npos);
}

TEST_CASE("SelectList set_items re-filters with the current query and preserves the selection",
        "[tui][select-list][issue586]") {
    cch::tui::SelectList list(
            {
                    {.value = "alpha", .label = "Alpha"},
                    {.value = "beta", .label = "Beta"},
                    {.value = "gamma", .label = "Gamma"},
            },
            cch::tui::SelectListOptions{.enable_search = true});

    // Keep a query active and replace the set: the query is re-applied and
    // the previously selected item (by value) stays selected when present.
    type_into(list, "ga");
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == "gamma");
    list.set_items({
            {.value = "gamma", .label = "Gamma updated"},
            {.value = "delta", .label = "Delta"},
            {.value = "gauss", .label = "Gauss"},
    });
    CHECK(list.search_query() == "ga");
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == "gamma");
    CHECK(list.selected_item()->label == "Gamma updated");

    // Replacing with a set that no longer matches the query empties the list.
    list.set_items({
            {.value = "delta", .label = "Delta"},
            {.value = "epsilon", .label = "Epsilon"},
    });
    CHECK_FALSE(list.selected_item());
    CHECK(list.search_query() == "ga");

    // Clearing the query after set_items shows the new items in order.
    list.handle_input(cch::tui::KeyEvent{.key = "backspace"});
    list.handle_input(cch::tui::KeyEvent{.key = "backspace"});
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == "delta");

    // Search disabled: set_items re-applies the legacy filter and preserves
    // the selection by value, clamping when the value disappears.
    cch::tui::SelectList legacy(
            {
                    {.value = "alpine", .label = "Alpine"},
                    {.value = "beta", .label = "Beta"},
            },
            cch::tui::SelectListOptions{});
    legacy.set_filter("al");
    REQUIRE(legacy.selected_item());
    CHECK(legacy.selected_item()->value == "alpine");
    legacy.set_items({
            {.value = "delta", .label = "Delta"},
            {.value = "alpine", .label = "Alpine"},
            {.value = "alpha", .label = "Alpha"},
    });
    REQUIRE(legacy.selected_item());
    CHECK(legacy.selected_item()->value == "alpine");
    CHECK(legacy.selected_item()->label == "Alpine");
    legacy.set_items({
            {.value = "beta", .label = "Beta"},
            {.value = "gamma", .label = "Gamma"},
    });
    CHECK_FALSE(legacy.selected_item());
}

TEST_CASE("SelectList renders the search placeholder initial query and cursor column", "[tui][select-list][issue586]") {
    cch::tui::SelectList list(
            {
                    {.value = "alpha", .label = "Alpha"},
                    {.value = "beta", .label = "Beta"},
            },
            cch::tui::SelectListOptions{
                    .enable_search = true,
                    .search_placeholder = "Filter items",
            });
    list.set_focused(true);

    // Empty query: the placeholder renders dimmed after the search cursor.
    const auto empty_query = list.render(40);
    REQUIRE(empty_query);
    REQUIRE_FALSE(empty_query->lines.empty());
    CHECK(empty_query->lines[0].starts_with("> \x1b[7m \x1b[27m\x1b[2mFilter items\x1b[22m"));
    CHECK(cch::tui::visible_width(empty_query->lines[0]) == 40);
    const auto empty_cursor = list.cursor_location();
    REQUIRE(empty_cursor);
    CHECK(empty_cursor->row == 0);
    CHECK(empty_cursor->column == 2);

    // Typing hides the placeholder and moves the cursor column.
    type_into(list, "ab");
    const auto typed = list.render(40);
    REQUIRE(typed);
    CHECK(typed->lines[0].starts_with("> ab"));
    CHECK(typed->lines[0].find("Filter items") == std::string::npos);
    const auto typed_cursor = list.cursor_location();
    REQUIRE(typed_cursor);
    CHECK(typed_cursor->column == 4);
}

TEST_CASE("SelectList search mode starts from the initial query when configured", "[tui][select-list][issue586]") {
    cch::tui::SelectList list(
            {
                    {.value = "alpha", .label = "Alpha"},
                    {.value = "alpine", .label = "Alpine"},
                    {.value = "beta", .label = "Beta"},
            },
            cch::tui::SelectListOptions{
                    .enable_search = true,
                    .search_placeholder = "Filter items",
                    .initial_search = "alpi",
            });

    CHECK(list.search_query() == "alpi");
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == "alpine");
    const auto rendered = list.render(40);
    REQUIRE(rendered);
    CHECK(rendered->lines[0].starts_with("> alpi"));
    // A non-empty initial query suppresses the placeholder.
    CHECK(rendered->lines[0].find("Filter items") == std::string::npos);
}

TEST_CASE("SelectList frames lists with borders titles and hints in a deterministic chrome layout",
        "[tui][select-list][issue586]") {
    cch::tui::SelectList list(
            {
                    {.value = "alpha", .label = "Alpha"},
                    {.value = "beta", .label = "Beta"},
            },
            cch::tui::SelectListOptions{
                    .title = "Servers",
                    .hint = "Arrow keys navigate",
            });
    const auto rendered = list.render(30);
    REQUIRE(rendered);
    REQUIRE(rendered->lines.size() == 9);
    CHECK(rendered->lines[0] == border_rule(30));
    CHECK(rendered->lines[1].empty());
    CHECK(rendered->lines[2] == "Servers");
    CHECK(rendered->lines[3].empty());
    CHECK(rendered->lines[4] == "Arrow keys navigate");
    CHECK(rendered->lines[5].empty());
    CHECK(rendered->lines[6].find("Alpha") != std::string::npos);
    CHECK(rendered->lines[7].find("Beta") != std::string::npos);
    CHECK(rendered->lines[8] == border_rule(30));

    // Multiline chrome text preserves interior empty lines; a trailing CRLF
    // renders without its carriage return.
    cch::tui::SelectList multiline({{.value = "alpha", .label = "Alpha"}},
            cch::tui::SelectListOptions{
                    .title = "Alpha\n\nBeta",
                    .hint = "first line\r\nsecond line",
            });
    const auto framed = multiline.render(30);
    REQUIRE(framed);
    REQUIRE(framed->lines.size() == 11);
    CHECK(framed->lines[0] == border_rule(30));
    CHECK(framed->lines[1].empty());
    CHECK(framed->lines[2] == "Alpha");
    CHECK(framed->lines[3].empty());
    CHECK(framed->lines[4] == "Beta");
    CHECK(framed->lines[5].empty());
    CHECK(framed->lines[6] == "first line");
    CHECK(framed->lines[7] == "second line");
    CHECK(framed->lines[8].empty());
    CHECK(framed->lines[9].find("Alpha") != std::string::npos);
    CHECK(framed->lines[10] == border_rule(30));

    // Empty list with chrome: the no-match row is framed as well.
    cch::tui::SelectList empty({}, cch::tui::SelectListOptions{.title = "Empty"});
    const auto framed_empty = empty.render(30);
    REQUIRE(framed_empty);
    REQUIRE(framed_empty->lines.size() == 6);
    CHECK(framed_empty->lines[0] == border_rule(30));
    CHECK(framed_empty->lines[1].empty());
    CHECK(framed_empty->lines[2] == "Empty");
    CHECK(framed_empty->lines[3].empty());
    CHECK(framed_empty->lines[4].find("No matching commands") != std::string::npos);
    CHECK(framed_empty->lines[5] == border_rule(30));

    // Every emitted chrome line respects the render width.
    for (const auto& line : framed->lines) {
        CHECK(cch::tui::visible_width(line) <= 30);
    }
}

TEST_CASE("SelectList chrome with search styles borders and stays width bounded", "[tui][select-list][issue586]") {
    cch::tui::SelectList list(
            {
                    {.value = "very-long-value-name", .label = "A very long primary label that must truncate"},
            },
            cch::tui::SelectListOptions{
                    .enable_search = true,
                    .search_placeholder = "Filter the list",
                    .title = "A very long title that must truncate",
                    .hint = "A very long hint that must truncate",
                    .border_hook = [](std::string text) { return "\x1b[36m" + std::move(text) + "\x1b[0m"; },
            });
    type_into(list, "qu");
    list.set_focused(true);
    const auto rendered = list.render(12);
    REQUIRE(rendered);
    REQUIRE(rendered->lines.size() == 9);
    // Border rules are styled through border_hook but keep their width.
    CHECK(cch::tui::visible_width(rendered->lines.front()) == 12);
    CHECK(cch::tui::visible_width(rendered->lines.back()) == 12);
    CHECK(rendered->lines.front().find("\x1b[36m") != std::string::npos);
    // The styled border line is a full-width run of the rule character.
    CHECK(cch::tui::strip_terminal_sequences(rendered->lines.front()) == border_rule(12));
    // Search row and truncated chrome rows never exceed the width.
    for (const auto& line : rendered->lines) {
        CHECK(cch::tui::visible_width(line) <= 12);
    }
    const auto cursor = list.cursor_location();
    REQUIRE(cursor);
    CHECK(cursor->row == 6);
    CHECK(cursor->column == 4);
}

TEST_CASE(
        "SelectList reports the search cursor across chrome layouts and focus states", "[tui][select-list][issue586]") {
    const auto focused_list = [](cch::tui::SelectListOptions options) {
        cch::tui::SelectList list({{.value = "alpha", .label = "Alpha"}}, std::move(options));
        for (const auto& character : std::string("tool")) {
            list.handle_input(cch::tui::KeyEvent{.key = std::string(1, character)});
        }
        list.set_focused(true);
        return list;
    };

    // No chrome: the search line is row 0.
    auto plain = focused_list(cch::tui::SelectListOptions{.enable_search = true});
    auto rendered = plain.render(50);
    REQUIRE(rendered);
    const auto plain_cursor = plain.cursor_location();
    REQUIRE(plain_cursor);
    CHECK(plain_cursor->row == 0);
    CHECK(plain_cursor->column == 6);

    // Title only: border, blank, title, blank, search.
    auto titled = focused_list(cch::tui::SelectListOptions{.enable_search = true, .title = "Models"});
    rendered = titled.render(50);
    REQUIRE(rendered);
    REQUIRE(rendered->lines.size() == 7);
    CHECK(rendered->lines[4].starts_with("> tool"));
    const auto titled_cursor = titled.cursor_location();
    REQUIRE(titled_cursor);
    CHECK(titled_cursor->row == 4);
    CHECK(titled_cursor->column == 6);

    // Title plus hint: the search line moves below the hint block.
    auto hinted = focused_list(cch::tui::SelectListOptions{
            .enable_search = true,
            .title = "Models",
            .hint = "Type to filter",
    });
    rendered = hinted.render(50);
    REQUIRE(rendered);
    REQUIRE(rendered->lines.size() == 9);
    const auto hinted_cursor = hinted.cursor_location();
    REQUIRE(hinted_cursor);
    CHECK(hinted_cursor->row == 6);
    CHECK(hinted_cursor->column == 6);

    // Focus and rendering are prerequisites: never rendered, unfocused, or
    // search-disabled lists never report a cursor.
    cch::tui::SelectList never_rendered(
            {{.value = "alpha", .label = "Alpha"}}, cch::tui::SelectListOptions{.enable_search = true});
    never_rendered.set_focused(true);
    CHECK_FALSE(never_rendered.cursor_location());

    auto unfocused = focused_list(cch::tui::SelectListOptions{.enable_search = true});
    unfocused.set_focused(false);
    rendered = unfocused.render(50);
    REQUIRE(rendered);
    CHECK_FALSE(unfocused.cursor_location());

    cch::tui::SelectList no_search(
            {{.value = "alpha", .label = "Alpha"}}, cch::tui::SelectListOptions{.title = "Chrome without search"});
    no_search.set_focused(true);
    rendered = no_search.render(50);
    REQUIRE(rendered);
    CHECK_FALSE(no_search.cursor_location());

    // The reported row always matches the line the search input rendered on.
    auto bordered = focused_list(cch::tui::SelectListOptions{
            .enable_search = true,
            .border_hook = [](std::string text) { return "\x1b[36m" + std::move(text) + "\x1b[0m"; },
    });
    rendered = bordered.render(40);
    REQUIRE(rendered);
    const auto bordered_cursor = bordered.cursor_location();
    REQUIRE(bordered_cursor);
    CHECK(bordered_cursor->row == 2);
    CHECK(bordered_cursor->column == 6);
}

TEST_CASE("SelectList no-match row text is overridable and defaults to the toolkit wording",
        "[tui][select-list][issue590]") {
    // Custom wording replaces the no-match row for empty and unmatched
    // lists (search disabled and enabled); the default stays byte-identical
    // so existing consumers are unaffected.
    cch::tui::SelectList custom({}, cch::tui::SelectListOptions{.no_match_text = "  No matching models"});
    const auto empty_render = custom.render(40);
    REQUIRE(empty_render);
    REQUIRE_FALSE(empty_render->lines.empty());
    CHECK(empty_render->lines[0] == "  No matching models");

    cch::tui::SelectList custom_search({{.value = "alpha", .label = "Alpha"}},
            cch::tui::SelectListOptions{
                    .enable_search = true,
                    .no_match_text = "  No matching sessions",
            });
    type_into(custom_search, "zz");
    CHECK_FALSE(custom_search.selected_item());
    const auto search_render = custom_search.render(40);
    REQUIRE(search_render);
    REQUIRE(search_render->lines.size() >= 2);
    CHECK(search_render->lines[1] == "  No matching sessions");

    // Absent the option, the toolkit wording renders in both modes.
    cch::tui::SelectList legacy({}, cch::tui::SelectListOptions{});
    const auto default_render = legacy.render(40);
    REQUIRE(default_render);
    CHECK(default_render->lines[0] == "  No matching commands");

    cch::tui::SelectList legacy_search(
            {{.value = "alpha", .label = "Alpha"}}, cch::tui::SelectListOptions{.enable_search = true});
    type_into(legacy_search, "zz");
    const auto default_search = legacy_search.render(40);
    REQUIRE(default_search);
    REQUIRE(default_search->lines.size() >= 2);
    CHECK(default_search->lines[1] == "  No matching commands");
}

TEST_CASE("select_item_search_text prefers search_text and joins the visible parts", "[tui][select-list]") {
    // search_text overrides the label/description/value join entirely.
    CHECK(cch::tui::select_item_search_text(cch::tui::SelectItem{
                  .value = "value", .label = "Label", .description = "Details", .search_text = "hidden search text"}) ==
            "hidden search text");
    // Non-empty label, description and value join in that order.
    CHECK(cch::tui::select_item_search_text(cch::tui::SelectItem{
                  .value = "value", .label = "Label", .description = "Details"}) == "Label Details value");
    // Empty components are skipped without leaving extra separators.
    CHECK(cch::tui::select_item_search_text(
                  cch::tui::SelectItem{.value = "value", .label = "", .description = "Details"}) == "Details value");
    CHECK(cch::tui::select_item_search_text(
                  cch::tui::SelectItem{.value = "", .label = "Label", .description = std::nullopt}) == "Label");
    CHECK(cch::tui::select_item_search_text(
                  cch::tui::SelectItem{.value = "", .label = "", .description = std::nullopt}) == "");
}
