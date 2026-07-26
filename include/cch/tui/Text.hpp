#pragma once

#include <cch/tui/Component.hpp>

#include <string>
#include <string_view>

namespace cch::tui {

class Text final : public Component {
public:
    explicit Text(std::string text = {});

    void set_text(std::string text);
    [[nodiscard]] std::string_view text() const;
    [[nodiscard]] util::Expected<std::vector<std::string>> render(std::size_t width) override;
    void invalidate() override;

private:
    std::string text_;
};

} // namespace cch::tui
