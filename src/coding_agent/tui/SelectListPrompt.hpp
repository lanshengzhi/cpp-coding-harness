#pragma once

#include "DynamicBorder.hpp"

#include <cch/tui/Component.hpp>
#include <cch/tui/Container.hpp>
#include <cch/tui/SelectList.hpp>
#include <cch/tui/Text.hpp>

#include <memory>
#include <string>
#include <utility>

namespace cch::coding_agent::tui {

/// Prompt composition for temporary selectors that need title and hint rows.
class SelectListPrompt final : public cch::tui::Component, public cch::tui::InputHandler, public cch::tui::Focusable {
public:
    SelectListPrompt(std::string title,
            std::string hint,
            cch::tui::TextStyleHook border_style,
            std::unique_ptr<cch::tui::Component> list)
        : title_(std::move(title), 0, 0), hint_(std::move(hint), 0, 0), content_(std::move(list)) {
        auto shared_style = std::make_shared<cch::tui::TextStyleHook>(std::move(border_style));
        const auto make_border_style = [shared_style](std::string line) {
            return *shared_style ? (*shared_style)(std::move(line)) : std::move(line);
        };
        top_border_ = std::make_unique<DynamicBorder>(make_border_style);
        bottom_border_ = std::make_unique<DynamicBorder>(make_border_style);
    }

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        cch::tui::RenderResult result;
        const auto append = [&result, width](cch::tui::Component& component) -> support::ExpectedVoid {
            auto rendered = component.render(width);
            if (!rendered) return std::unexpected(rendered.error());
            for (auto& line : rendered->lines)
                result.lines.push_back(std::move(line));
            return {};
        };
        cch::tui::Spacer blank_row(1);
        if (auto added = append(*top_border_); !added) return std::unexpected(added.error());
        if (auto added = append(blank_row); !added) return std::unexpected(added.error());
        if (auto added = append(title_); !added) return std::unexpected(added.error());
        if (auto added = append(blank_row); !added) return std::unexpected(added.error());
        if (auto added = append(hint_); !added) return std::unexpected(added.error());
        if (auto added = append(blank_row); !added) return std::unexpected(added.error());
        list_row_offset_ = result.lines.size();
        if (auto added = append(*content_); !added) return std::unexpected(added.error());
        if (auto added = append(*bottom_border_); !added) return std::unexpected(added.error());
        return result;
    }

    void invalidate() override {
        top_border_->invalidate();
        title_.invalidate();
        hint_.invalidate();
        content_->invalidate();
        bottom_border_->invalidate();
    }

    [[nodiscard]] cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant& input) override {
        if (auto* input_handler = dynamic_cast<cch::tui::InputHandler*>(content_.get()); input_handler != nullptr) {
            return input_handler->handle_input(input);
        }
        return cch::tui::InputAdmissionOutcome::Unhandled;
    }

    void set_focused(bool focused) override {
        if (auto* focusable = dynamic_cast<cch::tui::Focusable*>(content_.get()); focusable != nullptr) {
            focusable->set_focused(focused);
        }
    }

    [[nodiscard]] bool focused() const override {
        const auto* focusable = dynamic_cast<const cch::tui::Focusable*>(content_.get());
        return focusable != nullptr && focusable->focused();
    }
    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override {
        const auto* focusable = dynamic_cast<const cch::tui::Focusable*>(content_.get());
        if (focusable == nullptr) return std::nullopt;
        const auto cursor = focusable->cursor_location();
        if (!cursor) return std::nullopt;
        return cch::tui::CursorPosition{
                .column = cursor->column,
                .row = cursor->row + list_row_offset_,
        };
    }

private:
    cch::tui::Text title_;
    cch::tui::Text hint_;
    std::unique_ptr<cch::tui::Component> content_;
    std::unique_ptr<DynamicBorder> top_border_;
    std::unique_ptr<DynamicBorder> bottom_border_;
    std::size_t list_row_offset_{0};
};

} // namespace cch::coding_agent::tui
