#pragma once

#include <cch/tui/Fuzzy.hpp>
#include <cch/tui/Input.hpp>
#include <cch/tui/SelectList.hpp>
#include <cch/tui/Utils.hpp>

#include <cch/support/Error.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {

/// Optional filter callback returns source indexes in display order. The
/// callback may be move-only and is independent of SelectList's value-prefix
/// filter.
using SelectListSearchFilter = std::move_only_function<std::vector<std::size_t>(
        std::string_view query, const std::vector<cch::tui::SelectItem>& items)>;

/// Selector composition that owns a search Input and passes the ranked rows
/// into the list-only toolkit SelectList.
class SearchableSelectList final : public cch::tui::Component,
                                   public cch::tui::InputHandler,
                                   public cch::tui::Focusable {
public:
    SearchableSelectList(std::vector<cch::tui::SelectItem> items,
            cch::tui::SelectListOptions options = {},
            SelectListSearchFilter filter = {},
            std::optional<std::string> no_match_message = std::nullopt)
        : items_(std::move(items)), filter_(std::move(filter)), no_match_message_(std::move(no_match_message)),
          no_match_style_(std::move(options.theme.no_match)), input_(cch::tui::InputOptions{
                                                                      .keybindings = options.keybindings,
                                                                      .keybinding_context = options.keybinding_context,
                                                              }),
          list_(std::vector<cch::tui::SelectItem>{}, std::move(options)) {
        recompute(true);
    }

    void set_items(std::vector<cch::tui::SelectItem> items) {
        items_ = std::move(items);
        recompute(false);
    }

    void set_filter(std::string query) {
        input_.set_value(std::move(query));
        input_.move_cursor_to_end();
        recompute(true);
    }

    [[nodiscard]] std::string search_query() const { return input_.value(); }
    [[nodiscard]] std::optional<cch::tui::SelectItem> selected_item() const { return list_.selected_item(); }
    [[nodiscard]] const std::vector<cch::tui::SelectItem>& filtered_items() const { return filtered_items_; }
    void set_selected_index(std::size_t index) { list_.set_selected_index(index); }

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        auto input = input_.render(width);
        if (!input) return std::unexpected(input.error());
        auto list = list_.render(width);
        if (!list) return std::unexpected(list.error());
        cch::tui::RenderResult result;
        result.lines.reserve(input->lines.size() + list->lines.size());
        for (auto& line : input->lines)
            result.lines.push_back(std::move(line));
        for (auto& line : list->lines)
            result.lines.push_back(std::move(line));
        if (filtered_items_.empty() && result.lines.size() > 1) {
            const auto text = no_match_message_.value_or("  No matching commands");
            auto message = cch::tui::truncate_text(text, width, "");
            if (!message) return std::unexpected(message.error());
            auto styled = apply_no_match_style(std::move(*message));
            if (!styled) return std::unexpected(styled.error());
            result.lines[1] = std::move(*styled);
        }
        return result;
    }

    void invalidate() override {
        input_.invalidate();
        list_.invalidate();
    }

    [[nodiscard]] cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant& event) override {
        const auto list_outcome = list_.handle_input(event);
        if (list_outcome == cch::tui::InputAdmissionOutcome::Consumed) return list_outcome;
        const auto prior_query = input_.value();
        const auto input_outcome = input_.handle_input(event);
        if (input_.value() != prior_query) recompute(true);
        return input_outcome;
    }

    void set_focused(bool focused) override {
        input_.set_focused(focused);
        list_.set_focused(focused);
    }

    [[nodiscard]] bool focused() const override { return input_.focused(); }
    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override {
        return input_.cursor_location();
    }

private:
    [[nodiscard]] support::Expected<std::string> apply_no_match_style(std::string text) {
        if (!no_match_style_) return text;
        return no_match_style_(std::move(text));
    }

    [[nodiscard]] std::vector<cch::tui::SelectItem> items_from_indices(const std::vector<std::size_t>& indices) const {
        std::vector<cch::tui::SelectItem> result;
        result.reserve(indices.size());
        std::vector<bool> seen(items_.size(), false);
        for (const auto index : indices) {
            if (index >= items_.size() || seen[index]) continue;
            seen[index] = true;
            result.push_back(items_[index]);
        }
        return result;
    }

    void recompute(bool reset_selection) {
        std::vector<std::size_t> indices;
        if (filter_) {
            indices = filter_(input_.value(), items_);
        } else if (input_.value().empty()) {
            indices.reserve(items_.size());
            for (std::size_t index = 0; index < items_.size(); ++index)
                indices.push_back(index);
        } else {
            indices.reserve(items_.size());
            for (std::size_t index = 0; index < items_.size(); ++index)
                indices.push_back(index);
            indices = cch::tui::fuzzy_filter(std::move(indices), input_.value(), [this](std::size_t index) {
                return cch::tui::select_item_search_text(items_[index]);
            });
        }

        filtered_items_ = items_from_indices(indices);
        list_.set_items(filtered_items_);
        if (reset_selection) list_.set_selected_index(0);
    }

    std::vector<cch::tui::SelectItem> items_;
    std::vector<cch::tui::SelectItem> filtered_items_;
    SelectListSearchFilter filter_;
    std::optional<std::string> no_match_message_;
    cch::tui::TextStyleHook no_match_style_;
    cch::tui::Input input_;
    cch::tui::SelectList list_;
};

} // namespace cch::coding_agent::tui
