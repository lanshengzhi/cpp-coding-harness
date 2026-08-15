#pragma once

#include "coding_agent/SessionDiscovery.hpp"
#include "coding_agent/tui/SessionSelectorSearch.hpp"

#include <cch/tui/Component.hpp>
#include <cch/tui/Input.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cch::coding_agent::tui {

class LiveTheme;

using SessionSelectorSelectSink = std::move_only_function<void(std::string)>;
using SessionSelectorCancelSink = std::move_only_function<void()>;
/// pi `onExit`: the host shutdown binding (the selector's quit surface).
using SessionSelectorExitSink = std::move_only_function<void()>;
/// pi `renameSession`: appends the trimmed `session_info` name; returns an
/// error string on failure (the status line shows it).
using SessionSelectorRenameSink = std::move_only_function<support::ExpectedVoid(
    std::string,
    std::string)>;
using SessionSelectorInvalidateSink = std::move_only_function<void()>;

/// One scope's session loader (pi `SessionManager.list` / `listAll`
/// closures). The C++ scans are synchronous, so the loader returns the
/// already-sorted session list.
using SessionListLoader =
    std::move_only_function<std::vector<session_discovery::SessionInfo>()>;

/// The in-session session selector (pi `session-selector.ts`, G3/G2): the
/// threaded session tree with the current-folder/all scope toggle, the
/// threaded/recent/relevance sort cycle, the named filter, the path toggle,
/// rename mode, delete confirmation (trash with unlink fallback), and search
/// through the Input + fuzzy seams (`re:` regex and `"phrase"` tokens).
///
/// Layout follows pi: a three-line header (title + sort/name/scope, then two
/// hint lines that switch to the delete-confirmation or status message), the
/// search Input, a blank line, the session rows (max 10 visible with a
/// scroll indicator), and the two dynamic borders. Rendering and input run
/// on the TUI thread; the loaders are synchronous best-effort scans.
class SessionSelectorComponent final
    : public cch::tui::Component,
      public cch::tui::InputHandler,
      public cch::tui::Focusable,
      public std::enable_shared_from_this<SessionSelectorComponent> {
public:
    SessionSelectorComponent(
        const LiveTheme& theme,
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
        SessionListLoader current_loader,
        SessionListLoader all_loader,
        std::optional<std::filesystem::path> current_session_path,
        SessionSelectorSelectSink on_select,
        SessionSelectorCancelSink on_cancel,
        SessionSelectorExitSink on_exit,
        SessionSelectorRenameSink on_rename,
        SessionSelectorInvalidateSink on_invalidate);
    SessionSelectorComponent& operator=(SessionSelectorComponent&&) = delete;
    ~SessionSelectorComponent() override = default;
    SessionSelectorComponent(const SessionSelectorComponent&) = delete;
    SessionSelectorComponent& operator=(const SessionSelectorComponent&) = delete;

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override {}
    void handle_input(const cch::tui::InputEventVariant& input) override;
    [[nodiscard]] bool accepts_key_releases() const override { return false; }
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override;

    /// The rename-mode Input (pi `getSessionList`-style test seam).
    [[nodiscard]] cch::tui::Input& rename_input() { return rename_input_; }

private:
    struct FlatSessionNode {
        const session_discovery::SessionInfo* session{nullptr};
        std::size_t depth{0};
        bool is_last{false};
        std::vector<bool> ancestor_continues;
    };

    void load_current();
    void load_all();
    void set_scope(bool all);
    void set_sort_mode(SessionSortMode mode);
    void set_name_filter(SessionNameFilter filter);
    void filter_sessions(std::string query);
    void refresh_after_mutation();
    void enter_rename_mode(const session_discovery::SessionInfo& session);
    void exit_rename_mode();
    void confirm_rename(std::string value);
    void start_delete_confirmation();
    void confirm_delete();
    void cancel_delete();
    void set_status_message(std::string message, bool error);
    void build_tree(const std::vector<session_discovery::SessionInfo>& sessions);
    [[nodiscard]] bool is_current_session_path(const std::filesystem::path& path) const;

    const LiveTheme& theme_; // must outlive this component.
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings_;
    SessionListLoader current_loader_;
    SessionListLoader all_loader_;
    std::optional<std::filesystem::path> current_session_path_;
    SessionSelectorSelectSink on_select_;
    SessionSelectorCancelSink on_cancel_;
    SessionSelectorExitSink on_exit_;
    SessionSelectorRenameSink on_rename_;
    SessionSelectorInvalidateSink on_invalidate_;

    cch::tui::Input search_input_;
    cch::tui::Input rename_input_;

    bool scope_all_{false};
    SessionSortMode sort_mode_{SessionSortMode::Threaded};
    SessionNameFilter name_filter_{SessionNameFilter::All};
    std::vector<session_discovery::SessionInfo> current_sessions_;
    std::vector<session_discovery::SessionInfo> all_sessions_;
    /// Stable storage for the flat (non-threaded) filtered list; the display
    /// nodes reference it (the threaded tree references the scope lists).
    std::vector<session_discovery::SessionInfo> active_filtered_storage_;
    std::vector<FlatSessionNode> filtered_sessions_;
    std::size_t selected_index_{0};
    bool show_path_{false};
    bool show_cwd_{false};
    std::optional<std::string> confirming_delete_path_{std::nullopt};
    std::optional<std::pair<std::string, bool>> status_message_{std::nullopt};
    bool mode_rename_{false};
    std::string rename_target_path_;
    bool focused_{false};
};

} // namespace cch::coding_agent::tui
