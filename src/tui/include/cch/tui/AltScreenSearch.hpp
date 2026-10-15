#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Input.hpp>
#include <cch/tui/Keybindings.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::tui {

namespace detail {
struct AltScreenSearchSourceSpan {
    std::size_t text_start{0};
    std::size_t text_end{0};
    std::size_t row{0};
    std::size_t start_col{0};
    std::size_t end_col{0};
    bool linear_columns{false};
};

struct AltScreenSearchCorpus {
    std::string text{};
    std::vector<AltScreenSearchSourceSpan> spans{};
};
} // namespace detail

struct SearchSegment {
    std::size_t row{0};
    std::size_t start_col{0};
    std::size_t end_col{0};

    bool operator==(const SearchSegment&) const = default;
};

struct SearchMatch {
    std::vector<SearchSegment> segments{};

    bool operator==(const SearchMatch&) const = default;
};

struct SearchResult {
    std::vector<SearchMatch> matches{};
    bool changed{false};
};

/// Cache rendered transcript rows and their normalized literal query matches.
class SearchIndex final {
public:
    [[nodiscard]] SearchResult search(const std::vector<std::string>& lines, std::string_view query);

private:
    std::vector<std::string> source_lines_{};
    detail::AltScreenSearchCorpus corpus_{};
    std::string normalized_query_{};
    std::vector<SearchMatch> matches_{};
    bool initialized_{false};
};

/// Find normalized literal matches in rendered transcript rows (pi
/// `alt-screen-search.ts:186-189`).
[[nodiscard]] std::vector<SearchMatch> find_alt_screen_search_matches(
        const std::vector<std::string>& lines, std::string_view query);

/// Stable identity used to retain a selection as streamed rows change (pi
/// `alt-screen-search.ts:191-195`).
[[nodiscard]] std::string alt_screen_search_match_key(const SearchMatch& match);

using SearchNavigationButtonStyle = std::function<std::string(std::string, bool)>;

/// Focusable three-row search panel (pi `alt-screen-search.ts:197-327`).
class AltScreenSearchComponent final : public Component, public InputHandler, public Focusable {
public:
    AltScreenSearchComponent(std::function<void(std::string)> on_query_change,
            const KeybindingRegistry& keybindings,
            std::shared_ptr<KeybindingsContext> keybinding_context = {},
            SearchNavigationButtonStyle navigation_button_style = {});
    ~AltScreenSearchComponent() override = default;

    [[nodiscard]] std::string query() const;
    void set_result(std::ptrdiff_t index, std::size_t count);
    [[nodiscard]] std::optional<int> navigation_direction_at(std::size_t row, std::size_t column) const;
    [[nodiscard]] bool set_hovered_navigation_direction(std::optional<int> direction);

    [[nodiscard]] support::Expected<RenderResult> render(std::size_t width) override;
    void invalidate() override;
    InputAdmissionOutcome handle_input(const InputEventVariant& input) override;
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    [[nodiscard]] std::optional<CursorPosition> cursor_location() const override;

private:
    [[nodiscard]] std::string key_label(std::string_view id) const;

    Input input_;
    const KeybindingRegistry& keybindings_;
    std::shared_ptr<KeybindingsContext> keybinding_context_{};
    std::function<void(std::string)> on_query_change_;
    SearchNavigationButtonStyle navigation_button_style_;
    std::size_t result_count_{0};
    std::ptrdiff_t result_index_{-1};
    std::ptrdiff_t previous_button_start_{-1};
    std::ptrdiff_t previous_button_end_{-1};
    std::ptrdiff_t next_button_start_{-1};
    std::ptrdiff_t next_button_end_{-1};
    std::optional<int> hovered_navigation_direction_{};
    bool focused_{false};
};

} // namespace cch::tui
