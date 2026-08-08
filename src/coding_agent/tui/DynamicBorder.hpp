#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Style.hpp>
#include <cch/util/Error.hpp>

#include <cstddef>

namespace cch::coding_agent::tui {

/// pi `dynamic-border.ts`: a horizontal rule that fills the viewport width,
/// colored by the injected hook (pi's default is `theme.fg("border")`).
class DynamicBorder final : public cch::tui::Component {
public:
    explicit DynamicBorder(cch::tui::TextStyleHook color)
        : color_(std::move(color)) {}

    [[nodiscard]] util::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        // U+2500 BOX DRAWINGS LIGHT HORIZONTAL, repeated to the width.
        std::string rule;
        rule.reserve(width * 3);
        const auto count = width > 0 ? width : 1;
        for (std::size_t index = 0; index < count; ++index) rule += "─";
        return cch::tui::RenderResult{
            .lines = {color_(std::move(rule))},
        };
    }

    void invalidate() override {}

private:
    cch::tui::TextStyleHook color_;
};

} // namespace cch::coding_agent::tui
