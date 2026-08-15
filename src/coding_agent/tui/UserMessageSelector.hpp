#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cch::coding_agent::tui {

class LiveTheme;

/// One fork-selectable user message (pi `UserMessageItem`).
struct UserForkItem {
    std::string entry_id;
    std::string text;
};

using UserMessageSelectSink = std::move_only_function<void(std::string)>;
using UserMessageCancelSink = std::move_only_function<void()>;
using UserMessageInvalidateSink = std::move_only_function<void()>;

/// The user-message selector (pi `user-message-selector.ts`): the fork
/// picker listing every forkable user message, newest preselected, with the
/// "Fork from Message" header and the `Message N of M` metadata lines.
/// Selection returns the entry id; an empty list auto-cancels.
class UserMessageSelectorComponent final
    : public cch::tui::Component,
      public cch::tui::InputHandler,
      public cch::tui::Focusable,
      public std::enable_shared_from_this<UserMessageSelectorComponent> {
public:
    UserMessageSelectorComponent(
        const LiveTheme& theme,
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
        std::vector<UserForkItem> messages,
        std::optional<std::string> initial_selected_id,
        UserMessageSelectSink on_select,
        UserMessageCancelSink on_cancel,
        UserMessageInvalidateSink on_invalidate);
    UserMessageSelectorComponent(UserMessageSelectorComponent&&) = delete;
    UserMessageSelectorComponent& operator=(UserMessageSelectorComponent&&) = delete;
    ~UserMessageSelectorComponent() override = default;
    UserMessageSelectorComponent(const UserMessageSelectorComponent&) = delete;
    UserMessageSelectorComponent& operator=(const UserMessageSelectorComponent&) = delete;

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override {}
    void handle_input(const cch::tui::InputEventVariant& input) override;
    [[nodiscard]] bool accepts_key_releases() const override { return false; }
    void set_focused(bool focused) override { focused_ = focused; }
    [[nodiscard]] bool focused() const override { return focused_; }
    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override {
        return std::nullopt;
    }

private:
    const LiveTheme& theme_; // must outlive this component.
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings_;
    std::vector<UserForkItem> messages_;
    UserMessageSelectSink on_select_;
    UserMessageCancelSink on_cancel_;
    UserMessageInvalidateSink on_invalidate_;
    std::size_t selected_index_{0};
    bool focused_{false};
};

} // namespace cch::coding_agent::tui
