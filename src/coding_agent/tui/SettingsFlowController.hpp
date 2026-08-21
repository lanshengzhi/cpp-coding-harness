#pragma once

// The Native TUI settings flow (pi `showSettingsSelector` +
// `toggleThinkingBlockVisibility` + `cycleThinkingLevel`, #501 spec;
// extraction #506 from the InteractiveState monolith): the settings
// selector over the #327 settings subset plus the two graduated render
// settings, presented through the ModalPresenter seam and persisted through
// the two-scope settings manager. The host owns the view, the render
// settings storage, and the chat/autocomplete rebuilds behind
// SettingsFlowHostHooks.
//
// Repository-private `cch_coding_agent` implementation header: not part of
// an Owner Interface, not installed, never exported.

#include "coding_agent/tui/ModalPresenter.hpp"

#include <boost/asio/any_io_executor.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace cch::coding_agent {
class AgentSession;
class SettingsManager;
} // namespace cch::coding_agent

namespace cch::coding_agent::tui {

class SharedKeybindings;
class ThemeController;

/// Host operations used by SettingsFlowController. The controller owns the
/// selector composition and the thinking/render-setting flows; the host
/// supplies the session, the overlay gate, the render-settings storage, and
/// the chat/autocomplete rebuilds without exposing the Terminal or the
/// interactive engine. Hooks capture the host weakly; a null hook fails
/// closed (the step no-ops).
struct SettingsFlowHostHooks {
    /// Whether the host run is live with a composed view.
    std::move_only_function<bool()> is_live{nullptr};
    /// Marshal one input-thread callback onto the host executor with the
    /// host liveness gate; the action is dropped once the host stops.
    std::move_only_function<void(std::move_only_function<void()>)> post_on_executor{nullptr};
    /// Resolve the current session at execution time (borrowed; null before
    /// the boot session binds).
    std::move_only_function<AgentSession*()> current_session{nullptr};
    /// Whether a modal overlay is active (the selector does not open over
    /// one, matching the pre-extraction guard).
    std::move_only_function<bool()> overlay_active{nullptr};
    /// The current pi `hideThinkingBlock` / `outputPad` render settings the
    /// host view renders with.
    std::move_only_function<bool()> hide_thinking_block{nullptr};
    std::move_only_function<std::size_t()> output_pad{nullptr};
    /// Store one render setting on the host; the live apply follows through
    /// `rebuild_chat`.
    std::move_only_function<void(bool)> set_hide_thinking_block{nullptr};
    std::move_only_function<void(std::size_t)> set_output_pad{nullptr};
    /// pi `rebuildChatFromMessages`: rebuild the chat from the session
    /// snapshot with the current render settings.
    std::move_only_function<void()> rebuild_chat{nullptr};
    /// pi `setupAutocompleteProvider` after a settings change.
    std::move_only_function<void()> rebuild_autocomplete_provider{nullptr};
};

/// Native TUI settings flow controller (#506). Synchronous like the pi
/// original (no coroutine flows): the selector renders in the editor slot
/// (pi's `showSelector` editorContainer swap) and its callbacks re-enter on
/// the host executor through the hooks. The settings manager and theme
/// controller are borrowed from the host and must outlive the controller.
class SettingsFlowController final
    : public std::enable_shared_from_this<SettingsFlowController> {
public:
    SettingsFlowController(
        boost::asio::any_io_executor executor,
        ModalPresenter& presenter,
        std::weak_ptr<void> host_lifetime,
        SettingsFlowHostHooks hooks,
        std::shared_ptr<SharedKeybindings> keybindings,
        coding_agent::SettingsManager* settings_manager,
        ThemeController* theme_controller);
    SettingsFlowController(SettingsFlowController&&) = delete;
    SettingsFlowController& operator=(SettingsFlowController&&) = delete;
    ~SettingsFlowController() = default;
    SettingsFlowController(const SettingsFlowController&) = delete;
    SettingsFlowController& operator=(const SettingsFlowController&) = delete;

    /// pi `showSettingsSelector`: the settings selector over the #327
    /// settings subset plus the two graduated render settings; the Theme
    /// item opens the G5 single-mode ThemeSubmenu with in-memory preview, a
    /// global-scope settings commit on confirm, and cancel-does-not-revert.
    /// Executor-confined.
    void show_settings_selector();

    /// pi `toggleThinkingBlockVisibility`: flip the local render setting,
    /// persist it through the settings manager, rebuild the chat from the
    /// session (streaming message included), and report the pi status line.
    /// Executor-confined.
    void toggle_thinking_block_visibility();

    /// pi `cycleThinkingLevel` presentation: `Current model does not support
    /// thinking` when the model has no reasoning, else
    /// `Thinking level: <level>`. Executor-confined.
    void cycle_thinking_level();

private:
    /// pi `setHideThinkingBlock` + live chat rebuild: persist the global
    /// `hideThinkingBlock` setting and rebuild the chat from the session
    /// snapshot so the assistant messages re-render with the new visibility.
    void set_hide_thinking_block_setting(bool hidden);
    /// pi `setOutputPad` + live chat rebuild.
    void set_output_pad_setting(std::size_t padding);

    void post(std::move_only_function<void()> action);
    [[nodiscard]] AgentSession* current_session();
    void show_error(const std::string& text);

    boost::asio::any_io_executor executor_;
    ModalPresenter* presenter_; // kept alive by host_lifetime_ across callbacks.
    std::weak_ptr<void> host_lifetime_;
    SettingsFlowHostHooks hooks_;
    std::shared_ptr<SharedKeybindings> keybindings_;
    coding_agent::SettingsManager* settings_manager_; // may be null; must outlive the controller.
    ThemeController* theme_controller_; // may be null; must outlive the controller.
};

} // namespace cch::coding_agent::tui
