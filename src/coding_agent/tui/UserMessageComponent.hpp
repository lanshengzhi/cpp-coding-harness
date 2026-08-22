#pragma once

#include <cch/ai/Content.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Container.hpp>
#include <cch/tui/Image.hpp>
#include <cch/tui/Markdown.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cch::coding_agent::tui {

class LiveTheme;

/// pi `user-message.ts`: one user message rendered in a padded box with the
/// `userMessageBg` background, `userMessageText` markdown, and OSC 133 A/B/C
/// prompt zones around the rendered block. Image content blocks render
/// inline after the box (the C++ terminal-image surface).
class UserMessageComponent final : public cch::tui::Component {
public:
    /// Theme styling is read at construction time.
    UserMessageComponent(
        const LiveTheme& theme,
        std::variant<std::string, std::vector<ai::Content>> content,
        std::size_t output_pad = 1);
    ~UserMessageComponent() override;

    UserMessageComponent(const UserMessageComponent&) = delete;
    UserMessageComponent& operator=(const UserMessageComponent&) = delete;

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override;

private:
    struct ImageSlot {
        std::unique_ptr<cch::tui::Image> component;
        std::string data;
        std::string mime_type;
    };

    cch::tui::Box box_;
    std::unique_ptr<cch::tui::Markdown> markdown_;
    // In content order so multi-image messages render in source order.
    std::vector<std::unique_ptr<ImageSlot>> image_slots_;
};

} // namespace cch::coding_agent::tui
