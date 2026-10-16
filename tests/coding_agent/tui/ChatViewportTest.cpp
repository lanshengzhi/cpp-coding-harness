// The product two-region fullscreen viewport (#1036, spec #946): the frozen
// `createChatViewport` assembly (`chat-viewport.ts:22`) installed as the
// `AltScreen` layout root. These are behavioural product assertions; the frozen
// pi-v1.0.4 frames, styles and cursor are replayed from the named bundle in
// ChatViewportEvidenceTest.

#include "coding_agent/tui/ChatViewport.hpp"

#include <cch/tui/AltScreen.hpp>
#include <cch/tui/ScrollView.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

using cch::tui::ScrollbarMode;
using cch::tui::ScrollOverscroll;

/// The region content of one viewport: plain `Text` rows, the shape the frozen
/// capture and the product's own regions both render through.
struct Regions {
    std::vector<std::string> document{"doc-1"};
    std::string pending{};
    std::string status{};
    std::string editor{"editor-1"};
    std::vector<std::string> footer{};
    std::optional<std::vector<std::string>> widgets_above{};
    std::optional<std::vector<std::string>> widgets_below{};
    ScrollbarMode scrollbar{ScrollbarMode::Auto};
};

[[nodiscard]] std::string joined(std::vector<std::string> lines) {
    std::string text;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index > 0) text.push_back('\n');
        text += lines[index];
    }
    return text;
}

/// The painted rows with the terminal's width padding removed: the frozen
/// observation uses pi's `visibleLines` convention (`test/layout.test.ts:10-12`).
[[nodiscard]] std::vector<std::string> trimmed_rows(const cch::tui::VirtualTerminal& terminal) {
    std::vector<std::string> rows;
    rows.reserve(terminal.screen().size());
    for (const auto& row : terminal.screen()) {
        auto trimmed = row;
        while (!trimmed.empty() && trimmed.back() == ' ')
            trimmed.pop_back();
        rows.push_back(std::move(trimmed));
    }
    return rows;
}

/// One installed viewport: the viewport's root is the alt-screen layout root
/// over a virtual terminal, exactly as the engine installs it in Fullscreen
/// mode. The document and editor `Text` instances stay addressable so a test
/// can grow them under an installed root.
class InstalledViewport {
public:
    InstalledViewport(std::size_t columns, std::size_t rows, Regions regions = {})
        : terminal({.columns = columns, .rows = rows}), screen_(terminal) {
        ChatViewportOptions options;
        auto document = std::make_unique<cch::tui::Text>(joined(std::move(regions.document)), 0, 0);
        auto editor = std::make_unique<cch::tui::Text>(std::move(regions.editor), 0, 0);
        document_ = document.get();
        editor_ = editor.get();
        options.document = std::move(document);
        options.pending_messages = std::make_unique<cch::tui::Text>(std::move(regions.pending), 0, 0);
        options.status = std::make_unique<cch::tui::Text>(std::move(regions.status), 0, 0);
        options.editor = std::move(editor);
        options.footer = std::make_unique<cch::tui::Text>(joined(std::move(regions.footer)), 0, 0);
        if (regions.widgets_above.has_value()) {
            options.widgets_above = std::make_unique<cch::tui::Text>(joined(std::move(*regions.widgets_above)), 0, 0);
        }
        if (regions.widgets_below.has_value()) {
            options.widgets_below = std::make_unique<cch::tui::Text>(joined(std::move(*regions.widgets_below)), 0, 0);
        }
        options.scrollbar = regions.scrollbar;
        auto viewport = create_chat_viewport(std::move(options));
        REQUIRE(viewport);
        viewport_ = std::move(*viewport);
        screen_.set_layout_root(std::move(viewport_.root));
        REQUIRE(screen_.start());
    }

    ~InstalledViewport() { (void)screen_.stop(); }

    InstalledViewport(const InstalledViewport&) = delete;
    InstalledViewport& operator=(const InstalledViewport&) = delete;

    void render() { REQUIRE(screen_.render()); }

    void wheel_up(std::size_t times = 1) {
        for (std::size_t index = 0; index < times; ++index)
            REQUIRE(terminal.inject_input("\x1b[<64;1;1M"));
    }

    [[nodiscard]] cch::tui::Text* editor() { return editor_; }
    [[nodiscard]] std::unique_ptr<cch::tui::Component> take_editor() { return viewport_.take_editor(); }
    [[nodiscard]] support::ExpectedVoid set_editor_component(std::unique_ptr<cch::tui::Component> editor) {
        return viewport_.set_editor(std::move(editor));
    }
    void take_root() { detached_root_ = screen_.take_layout_root(); }
    [[nodiscard]] std::unique_ptr<cch::tui::Component> release_root() { return std::move(detached_root_); }
    void reinstall_root(std::unique_ptr<cch::tui::Component> root) { screen_.set_layout_root(std::move(root)); }
    [[nodiscard]] cch::tui::ScrollView& transcript() { return *viewport_.transcript; }
    [[nodiscard]] cch::tui::AltScreen& screen() { return screen_; }
    [[nodiscard]] std::vector<std::string> rows() const { return trimmed_rows(terminal); }

    void set_document(std::vector<std::string> lines) { document_->set_text(joined(std::move(lines))); }
    void set_editor(std::string text) { editor_->set_text(std::move(text)); }

    cch::tui::VirtualTerminal terminal;

private:
    cch::tui::AltScreen screen_;
    ChatViewport viewport_;
    std::unique_ptr<cch::tui::Component> detached_root_;
    cch::tui::Text* document_{nullptr};
    cch::tui::Text* editor_{nullptr};
};

} // namespace

TEST_CASE("ChatViewport assembles the transcript viewport over the fixed dock",
        "[coding_agent][tui][fullscreen][viewport][issue1036][spec]") {
    InstalledViewport viewport(24,
            9,
            Regions{
                    .document = {"one", "two"},
                    .pending = "pending-1",
                    .status = "status-1",
                    .editor = "editor-1",
                    .footer = {"footer-1", "footer-2"},
            });
    viewport.render();

    CHECK(viewport.transcript().follow_end());
    CHECK(viewport.transcript().primary());
    CHECK(viewport.transcript().overscroll() == ScrollOverscroll::Chain);
    CHECK(viewport.transcript().scrollbar() == ScrollbarMode::Auto);
    // Transcript first, then the dock in frozen order: pending, status, editor
    // (floored at three rows) and footer.
    CHECK(viewport.rows() ==
            std::vector<std::string>{
                    "one", "two", "pending-1", "status-1", "editor-1", "", "", "footer-1", "footer-2"});
}

TEST_CASE("ChatViewport keeps the dock fixed while the transcript scrolls independently",
        "[coding_agent][tui][fullscreen][viewport][issue1036][spec]") {
    InstalledViewport viewport(20,
            8,
            Regions{
                    .document = {"doc-01",
                            "doc-02",
                            "doc-03",
                            "doc-04",
                            "doc-05",
                            "doc-06",
                            "doc-07",
                            "doc-08",
                            "doc-09",
                            "doc-10",
                            "doc-11",
                            "doc-12",
                            "doc-13",
                            "doc-14",
                            "doc-15",
                            "doc-16",
                            "doc-17",
                            "doc-18",
                            "doc-19",
                            "doc-20"},
                    .pending = "pending-1",
                    .status = "status-1",
                    .editor = "editor-1",
                    .footer = {"footer-1", "footer-2"},
            });
    viewport.render();
    const auto before = viewport.rows();
    REQUIRE(before.size() == 8);
    CHECK(before.front() == "doc-20");
    CHECK(before[1] == "pending-1");
    CHECK(before[7] == "footer-2");

    viewport.wheel_up(2);
    viewport.render();
    const auto after = viewport.rows();

    // The transcript window moved and the wheel routed to it: the alt screen's
    // primary viewport is this transcript, not the implicit document.
    CHECK(after.front() != before.front());
    CHECK(viewport.transcript().scroll_top() > 0);
    CHECK_FALSE(viewport.transcript().following_end());
    CHECK(viewport.transcript().is_scrollbar_visible());
    CHECK(viewport.screen().viewport_top() == viewport.transcript().scroll_top());
    CHECK_FALSE(viewport.screen().is_following_output());
    // Pending, status, editor and footer keep their rows through the scroll.
    const std::vector<std::string> dock{after.begin() + 1, after.end()};
    const std::vector<std::string> fixed{before.begin() + 1, before.end()};
    CHECK(dock == fixed);
}

TEST_CASE("ChatViewport editor can leave and re-enter its retained dock without losing scroll state",
        "[coding_agent][tui][fullscreen][viewport][issue1018][spec]") {
    InstalledViewport viewport(20,
            8,
            Regions{
                    .document = {"doc-01",
                            "doc-02",
                            "doc-03",
                            "doc-04",
                            "doc-05",
                            "doc-06",
                            "doc-07",
                            "doc-08",
                            "doc-09",
                            "doc-10",
                            "doc-11",
                            "doc-12",
                            "doc-13",
                            "doc-14",
                            "doc-15",
                            "doc-16",
                            "doc-17",
                            "doc-18",
                            "doc-19",
                            "doc-20"},
                    .pending = "pending-1",
                    .status = "status-1",
                    .editor = "editor-1",
                    .footer = {"footer-1", "footer-2"},
            });
    viewport.render();
    viewport.wheel_up(2);
    viewport.render();
    const auto scroll_top = viewport.transcript().scroll_top();
    REQUIRE(scroll_top > 0);
    REQUIRE_FALSE(viewport.transcript().following_end());
    auto before = viewport.rows();

    viewport.take_root();
    auto root = viewport.release_root();
    REQUIRE(root);
    REQUIRE(viewport.screen().stop(cch::tui::TuiStopOptions{.preserve_screen = true}));
    auto* original_editor = viewport.editor();
    REQUIRE(original_editor != nullptr);
    auto taken = viewport.take_editor();
    REQUIRE(taken);
    CHECK(taken.get() == original_editor);
    REQUIRE(root->render(20));

    cch::tui::MainScreen regular(viewport.terminal);
    REQUIRE(regular.add_child(std::move(taken)));
    REQUIRE(regular.set_focus(nullptr));
    REQUIRE(regular.start());
    REQUIRE(regular.render());
    auto returned = regular.take_children();
    REQUIRE(returned.size() == 1);
    REQUIRE(regular.stop(cch::tui::TuiStopOptions{.preserve_screen = true}));

    CHECK(returned.front().get() == original_editor);
    REQUIRE(viewport.set_editor_component(std::move(returned.front())));
    REQUIRE(root->render(20));
    CHECK(viewport.transcript().scroll_top() == scroll_top);
    CHECK_FALSE(viewport.transcript().following_end());
    viewport.reinstall_root(std::move(root));
    REQUIRE(viewport.screen().start());
    REQUIRE(viewport.screen().render());
    const auto after = viewport.rows();
    CHECK(after == before);
    CHECK(viewport.transcript().scroll_top() == scroll_top);
    CHECK_FALSE(viewport.transcript().following_end());
}

TEST_CASE("ChatViewport measures the whole document and follows appended rows",
        "[coding_agent][tui][fullscreen][viewport][issue1036][spec]") {
    InstalledViewport viewport(20,
            8,
            Regions{
                    .document = {"doc-01",
                            "doc-02",
                            "doc-03",
                            "doc-04",
                            "doc-05",
                            "doc-06",
                            "doc-07",
                            "doc-08",
                            "doc-09",
                            "doc-10",
                            "doc-11",
                            "doc-12"},
                    .status = "status-1",
                    .editor = "editor-1",
                    .footer = {"footer-1", "footer-2"},
            });
    viewport.render();

    // The document renders unbounded inside its viewport: bounding it to the
    // visible rows would destroy transcript content the viewport can scroll to.
    CHECK(viewport.transcript().content_height() == 12);
    CHECK(viewport.transcript().viewport_height() == 2);
    CHECK(viewport.transcript().scroll_top() == 10);
    CHECK(viewport.transcript().following_end());

    viewport.set_document({"doc-01",
            "doc-02",
            "doc-03",
            "doc-04",
            "doc-05",
            "doc-06",
            "doc-07",
            "doc-08",
            "doc-09",
            "doc-10",
            "doc-11",
            "doc-12",
            "doc-13",
            "doc-14",
            "doc-15",
            "doc-16"});
    viewport.render();
    CHECK(viewport.transcript().content_height() == 16);
    CHECK(viewport.transcript().scroll_top() == 14);
    CHECK(viewport.transcript().following_end());
    CHECK(viewport.rows().front() == "doc-15");
}

TEST_CASE("ChatViewport floors the dock editor at three rows and grows with its content",
        "[coding_agent][tui][fullscreen][viewport][issue1036][spec]") {
    InstalledViewport viewport(20,
            12,
            Regions{
                    .document = {"doc-1", "doc-2"},
                    .editor = "prompt",
                    .footer = {"footer-1", "footer-2"},
            });
    viewport.render();
    auto rows = viewport.rows();
    REQUIRE(rows.size() == 12);
    CHECK(std::vector<std::string>{rows.begin() + 7, rows.begin() + 10} == std::vector<std::string>{"prompt", "", ""});

    viewport.set_editor(joined({"prompt-1", "prompt-2", "prompt-3", "prompt-4", "prompt-5", "prompt-6", "prompt-7"}));
    viewport.render();
    rows = viewport.rows();
    CHECK(std::vector<std::string>{rows.begin() + 3, rows.begin() + 10} ==
            std::vector<std::string>{
                    "prompt-1", "prompt-2", "prompt-3", "prompt-4", "prompt-5", "prompt-6", "prompt-7"});
}

TEST_CASE("ChatViewport omits absent widget regions and keeps their frozen dock order",
        "[coding_agent][tui][fullscreen][viewport][issue1036][spec]") {
    InstalledViewport without_widgets(24,
            10,
            Regions{
                    .document = {"doc-1", "doc-2", "doc-3", "doc-4", "doc-5", "doc-6"},
                    .pending = "pending-1",
                    .status = "status-1",
                    .editor = "editor-1",
                    .footer = {"footer-1", "footer-2"},
            });
    without_widgets.render();
    const auto plain_rows = without_widgets.rows();
    CHECK(std::find(plain_rows.begin(), plain_rows.end(), "above-1") == plain_rows.end());
    CHECK(std::find(plain_rows.begin(), plain_rows.end(), "below-1") == plain_rows.end());

    InstalledViewport with_widgets(24,
            10,
            Regions{
                    .document = {"doc-1", "doc-2", "doc-3", "doc-4", "doc-5", "doc-6"},
                    .pending = "pending-1",
                    .status = "status-1",
                    .editor = "editor-1",
                    .footer = {"footer-1", "footer-2"},
                    .widgets_above = std::vector<std::string>{"above-1"},
                    .widgets_below = std::vector<std::string>{"below-1"},
            });
    with_widgets.render();
    const auto rows = with_widgets.rows();
    const std::vector<std::string> order{
            "pending-1", "status-1", "above-1", "editor-1", "below-1", "footer-1", "footer-2"};
    std::vector<std::size_t> positions;
    for (const auto& row : order) {
        const auto at = std::find(rows.begin(), rows.end(), row);
        INFO(std::format("missing dock row: {}", row));
        REQUIRE(at != rows.end());
        positions.push_back(static_cast<std::size_t>(std::distance(rows.begin(), at)));
    }
    CHECK(positions == std::vector<std::size_t>{1, 2, 3, 4, 7, 8, 9});
}

TEST_CASE("ChatViewport defaults the scrollbar to auto and honours an explicit hidden override",
        "[coding_agent][tui][fullscreen][viewport][issue1036][spec]") {
    InstalledViewport automatic(20,
            6,
            Regions{
                    .document = {"doc-01",
                            "doc-02",
                            "doc-03",
                            "doc-04",
                            "doc-05",
                            "doc-06",
                            "doc-07",
                            "doc-08",
                            "doc-09",
                            "doc-10",
                            "doc-11",
                            "doc-12"},
                    .editor = "editor-1",
            });
    automatic.render();
    CHECK(automatic.transcript().scrollbar() == ScrollbarMode::Auto);
    CHECK_FALSE(automatic.transcript().is_scrollbar_visible());
    automatic.wheel_up();
    automatic.render();
    CHECK(automatic.transcript().is_scrollbar_visible());

    InstalledViewport hidden(20,
            6,
            Regions{
                    .document = {"doc-01",
                            "doc-02",
                            "doc-03",
                            "doc-04",
                            "doc-05",
                            "doc-06",
                            "doc-07",
                            "doc-08",
                            "doc-09",
                            "doc-10",
                            "doc-11",
                            "doc-12"},
                    .editor = "editor-1",
                    .scrollbar = ScrollbarMode::Hidden,
            });
    hidden.render();
    CHECK(hidden.transcript().scrollbar() == ScrollbarMode::Hidden);
    hidden.wheel_up();
    hidden.render();
    // The override hides the bar without taking the wheel away from the
    // transcript: the frozen arbiter case only pins the mode and visibility.
    CHECK_FALSE(hidden.transcript().is_scrollbar_visible());
    CHECK(hidden.transcript().scroll_top() > 0);
}

TEST_CASE("ChatViewport rejects a missing region and a non-positive width",
        "[coding_agent][tui][fullscreen][viewport][issue1036][spec]") {
    {
        ChatViewportOptions options;
        auto viewport = create_chat_viewport(std::move(options));
        REQUIRE_FALSE(viewport);
        CHECK(viewport.error().code == support::ErrorCode::Validation);
    }
    {
        ChatViewportOptions options;
        options.document = std::make_unique<cch::tui::Text>("doc-1", 0, 0);
        auto viewport = create_chat_viewport(std::move(options));
        REQUIRE_FALSE(viewport);
        CHECK(viewport.error().code == support::ErrorCode::Validation);
    }
    {
        Regions regions{.document = {"doc-1"}};
        ChatViewportOptions options;
        options.document = std::make_unique<cch::tui::Text>(joined(regions.document), 0, 0);
        options.pending_messages = std::make_unique<cch::tui::Text>("", 0, 0);
        options.status = std::make_unique<cch::tui::Text>("", 0, 0);
        options.editor = std::make_unique<cch::tui::Text>("editor-1", 0, 0);
        options.footer = std::make_unique<cch::tui::Text>("", 0, 0);
        auto viewport = create_chat_viewport(std::move(options));
        REQUIRE(viewport);
        const auto frame = viewport->root->render(0);
        REQUIRE_FALSE(frame);
        CHECK(frame.error().code == support::ErrorCode::Validation);
    }
}

} // namespace cch::coding_agent::tui
