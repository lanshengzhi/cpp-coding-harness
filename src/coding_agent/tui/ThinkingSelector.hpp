#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/SelectList.hpp>

#include <cch/support/Error.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cch::coding_agent::tui {

class LiveTheme;

using ThinkingSelectorSelectSink = std::move_only_function<void(std::string)>;
using ThinkingSelectorSelectAsDefaultSink = std::move_only_function<void(std::string)>;
using ThinkingSelectorCancelSink = std::move_only_function<void()>;

/// The thinking-level selector (pi `thinking-selector.ts`): opened by
/// `/thinking` with no argument, it lists the active model's available
/// levels with pi's per-level descriptions and the current-level marker,
/// plus the ` · default` marker on the settings default level. Enter
/// (`tui.select.confirm`) selects the highlighted level through `on_select`;
/// `app.thinking.save` (Ctrl+S) fires `on_select_as_default` (pi's
/// `onSelectAsDefault`); Escape cancels. The session-only vs persisted
/// distinction and the pi statuses (`Thinking level: <level>` vs `Default
/// thinking level: <level>`) ride the flow controller's mutation options
/// (#774); Shift+Tab keeps cycling in-session outside this selector.
///
/// The list rows and the search input are delegated to the shared
/// `cch::tui::SelectList` (search enabled); the component owns the chrome
/// around it (border, title, cycle hint, save hint) because the save hint
/// renders only when the save-as-default sink exists.
///
/// Threading: constructed and driven on the TUI thread; the SelectList (and
/// its embedded search Input) is only touched from the TUI thread.
class ThinkingSelectorComponent final : public cch::tui::Component,
                                        public cch::tui::InputHandler,
                                        public cch::tui::Focusable {
public:
    ThinkingSelectorComponent(const LiveTheme& theme,
            std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
            std::string current_level,
            std::vector<std::string> available_levels,
            ThinkingSelectorSelectSink on_select,
            ThinkingSelectorCancelSink on_cancel,
            ThinkingSelectorSelectAsDefaultSink on_select_as_default,
            std::optional<std::string> default_level);
    ThinkingSelectorComponent(ThinkingSelectorComponent&&) = delete;
    ThinkingSelectorComponent& operator=(ThinkingSelectorComponent&&) = delete;
    ~ThinkingSelectorComponent() override = default;
    ThinkingSelectorComponent(const ThinkingSelectorComponent&) = delete;
    ThinkingSelectorComponent& operator=(const ThinkingSelectorComponent&) = delete;

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override;
    cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant& input) override;
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    /// The search input's cursor translated into this component's own line
    /// coordinates: SelectList reports the row of its search line, and the
    /// rows this component emits above the SelectList (border, spacer, title,
    /// spacer, cycle hint, spacer) are added on top. Reported only when
    /// focused, rendered, and the SelectList itself reports a cursor.
    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override;

private:
    /// pi's item composition: the `✓ ` current marker leads the label and
    /// the description carries pi's per-level wording with the ` · default`
    /// marker on the settings default level.
    [[nodiscard]] std::vector<cch::tui::SelectItem> build_items() const;

    const LiveTheme& theme_; // must outlive this component.
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings_;
    std::string current_level_;
    std::vector<std::string> available_levels_;
    ThinkingSelectorSelectSink on_select_;
    ThinkingSelectorCancelSink on_cancel_;
    ThinkingSelectorSelectAsDefaultSink on_select_as_default_;
    std::optional<std::string> default_level_;
    cch::tui::SelectList select_list_;
    /// Rows this component emitted above the SelectList in the last
    /// successful render; cursor_location adds it to SelectList's row.
    std::size_t cursor_row_offset_{0};
    bool focused_{false};
};

} // namespace cch::coding_agent::tui
