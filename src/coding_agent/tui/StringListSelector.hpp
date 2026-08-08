#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/util/Error.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cch::coding_agent::tui {

class LiveTheme;

using StringListSelectSink = std::move_only_function<void(std::string)>;
using StringListCancelSink = std::move_only_function<void()>;
using StringListToggleSink = std::move_only_function<void()>;

struct StringListSelectorOptions {
    /// Invoked when the `app.tools.expand` binding fires inside the selector
    /// (pi `ExtensionSelectorOptions.onToggleToolsExpanded`); absent = the
    /// binding is consumed with no effect, exactly like pi's `?.()` call.
    StringListToggleSink on_toggle_tools_expanded{};
};

/// The generic string-list selector (pi `extension-selector.ts` — pi's own
/// login auth-type picker / `select`-type AuthPrompt / boot trust prompt
/// surface, **not** extension-only; the C++ name drops the misleading
/// "extension" label, G2 decision 4). Renders a titled option list between
/// dynamic borders with keyboard navigation; selection returns the chosen
/// string, cancellation the cancel sink.
class StringListSelector final
    : public cch::tui::Component,
      public cch::tui::InputHandler,
      public cch::tui::Focusable {
public:
    StringListSelector(
        const LiveTheme& theme,
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
        std::string title,
        std::vector<std::string> options,
        StringListSelectSink on_select,
        StringListCancelSink on_cancel,
        StringListSelectorOptions selector_options = {});
    StringListSelector(StringListSelector&&) = delete;
    StringListSelector& operator=(StringListSelector&&) = delete;
    ~StringListSelector() override = default;
    StringListSelector(const StringListSelector&) = delete;
    StringListSelector& operator=(const StringListSelector&) = delete;

    [[nodiscard]] util::Expected<cch::tui::RenderResult> render(std::size_t width) override;
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
    std::string title_;
    std::vector<std::string> options_;
    StringListSelectSink on_select_;
    StringListCancelSink on_cancel_;
    StringListSelectorOptions selector_options_;
    std::size_t selected_index_{0};
    bool focused_{false};
};

} // namespace cch::coding_agent::tui
