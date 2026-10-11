#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Style.hpp>

#include <cch/support/Error.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::tui {

struct SelectItem {
    std::string value{};
    std::string label{};
    std::optional<std::string> description{std::nullopt};
    /// Search text for selectors that rank items outside SelectList.
    std::optional<std::string> search_text{std::nullopt};

    bool operator==(const SelectItem&) const = default;
};

/// Search text shared by selectors that filter items before passing them to
/// SelectList. An explicit `search_text` replaces the visible item fields.
[[nodiscard]] std::string select_item_search_text(const SelectItem& item);

using SelectItemSink = std::move_only_function<support::ExpectedVoid(const SelectItem&)>;
using SelectCancelSink = std::move_only_function<support::ExpectedVoid()>;
using SelectPrimaryTruncateHook = std::move_only_function<std::string(
    std::string,
    std::size_t,
    std::size_t,
    const SelectItem&,
    bool)>;

struct SelectListTheme {
    TextStyleHook selected_text{};
    TextStyleHook description{};
    TextStyleHook scroll_info{};
    TextStyleHook no_match{};
};

struct SelectListLayoutOptions {
    std::optional<std::size_t> min_primary_column_width{std::nullopt};
    std::optional<std::size_t> max_primary_column_width{std::nullopt};
    SelectPrimaryTruncateHook truncate_primary{};
};

struct SelectListOptions {
    std::size_t max_visible{5};
    SelectListTheme theme{};
    SelectListLayoutOptions layout{};
    SelectItemSink on_select{};
    SelectCancelSink on_cancel{};
    SelectItemSink on_selection_change{};
    std::shared_ptr<const KeybindingRegistry> keybindings{};
    std::shared_ptr<KeybindingsContext> keybinding_context{};
};

/// A width-bounded selection list controlled by semantic keys. Search and
/// surrounding presentation belong to the component that composes this list.
class SelectList final : public Component, public InputHandler, public Focusable {
public:
    explicit SelectList(std::vector<SelectItem> items, SelectListOptions options = {});
    SelectList(SelectList&&) noexcept;
    SelectList& operator=(SelectList&&) noexcept;
    ~SelectList() override;

    SelectList(const SelectList&) = delete;
    SelectList& operator=(const SelectList&) = delete;

    /// Replace the items, preserving the selected value when it remains
    /// present, or clamping the prior selection position into the new list.
    void set_items(std::vector<SelectItem> items);
    void set_selected_index(std::size_t index);
    [[nodiscard]] std::optional<SelectItem> selected_item() const;

    [[nodiscard]] support::Expected<RenderResult> render(std::size_t width) override;
    void invalidate() override;
    InputAdmissionOutcome handle_input(const InputEventVariant& input) override;
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    [[nodiscard]] std::optional<CursorPosition> cursor_location() const override;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace cch::tui
