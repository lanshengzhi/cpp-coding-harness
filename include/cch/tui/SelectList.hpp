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
#include <vector>

namespace cch::tui {

struct SelectItem {
    std::string value{};
    std::string label{};
    std::optional<std::string> description{std::nullopt};

    bool operator==(const SelectItem&) const = default;
};

using SelectItemSink = std::move_only_function<void(const SelectItem&)>;
using SelectCancelSink = std::move_only_function<void()>;
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
};

/// A filterable, width-bounded selection list controlled by semantic keys.
class SelectList final : public Component, public InputHandler, public Focusable {
public:
    explicit SelectList(std::vector<SelectItem> items, SelectListOptions options = {});
    SelectList(SelectList&&) noexcept;
    SelectList& operator=(SelectList&&) noexcept;
    ~SelectList() override;

    SelectList(const SelectList&) = delete;
    SelectList& operator=(const SelectList&) = delete;

    void set_filter(std::string filter);
    void set_selected_index(std::size_t index);
    [[nodiscard]] std::optional<SelectItem> selected_item() const;

    [[nodiscard]] support::Expected<RenderResult> render(std::size_t width) override;
    void invalidate() override;
    void handle_input(const InputEventVariant& input) override;
    [[nodiscard]] bool accepts_key_releases() const override;
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace cch::tui
