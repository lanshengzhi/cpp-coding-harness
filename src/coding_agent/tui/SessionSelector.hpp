#pragma once

#include "coding_agent/SessionDiscovery.hpp"
#include "coding_agent/tui/SessionSelectorSearch.hpp"

#include <cch/tui/Component.hpp>
#include <cch/tui/Input.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/SelectList.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
/// through the shared `cch::tui::SelectList`.
///
/// Presentation seam: the query text, the filtered flat list, and the cursor
/// delegate to an embedded SelectList (`enable_search`). Session search keeps
/// its own token/regex matcher and relevance ordering (`SessionSelectorSearch`
/// — untouched) through `SelectListOptions.search_filter_hook`, which returns
/// the filtered item indices in rank order. The SelectList renders only while
/// a search query is active (its embedded Input is the single owner of the
/// query text, so typing reaches it from the tree view too); while the query
/// is empty/whitespace this component draws its own view — the threaded tree
/// (or the plain flat list for the Recent/Fuzzy sorts) with the count/age/
/// cwd/path rows, the scope/sort/name-filter header and hint rows, the rename
/// mode, the delete confirmation, and the status messages.
///
/// Layout follows pi: a three-line header (title + sort/name/scope, then two
/// hint lines that switch to the delete-confirmation or status message), the
/// search line, and the list body (tree rows for an empty query, SelectList
/// rows for a search) between the two dynamic borders. Rendering and input
/// run on the TUI thread; the loaders are synchronous best-effort scans.
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
    /// The active input's cursor translated into this component's own line
    /// coordinates. In the search/tree views the cursor belongs to the
    /// embedded SelectList's search Input: SelectList reports the row of its
    /// search line, and the component-owned rows above it (border, header,
    /// hint rows, spacer — recorded at render, never a magic constant) are
    /// added on top. In rename mode the cursor belongs to the rename Input,
    /// offset by the rows emitted above its line.
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

    void set_scope(bool all);
    void set_sort_mode(SessionSortMode mode);
    void set_name_filter(SessionNameFilter filter);
    /// Rebuild the tree/flat rows for the current (empty) query and the
    /// SelectList item set for the active scope after any state change.
    void refresh_presentation();
    void filter_sessions();
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
    /// The active scope's session list (current folder unless scope All).
    [[nodiscard]] const std::vector<session_discovery::SessionInfo>& scope_sessions() const;
    /// The session under the current selection: the SelectList's selected
    /// item while a search is active, else the tree/flat view's row.
    [[nodiscard]] const session_discovery::SessionInfo* selected_session() const;
    /// Resolve one SelectList row (value = session path) to the session and
    /// fire the select sink (Enter on a search result).
    void select_session(const cch::tui::SelectItem& item);
    [[nodiscard]] bool search_active() const;
    /// The scope sessions as SelectList items (value = session path; label =
    /// the display text; description = the count/age/cwd/path tail).
    [[nodiscard]] std::vector<cch::tui::SelectItem> build_select_items() const;
    /// The SelectList ranking hook: SessionSelectorSearch's token/regex
    /// matching plus its relevance ordering over the active scope's
    /// sessions, mapped back to item indices. An empty/whitespace query
    /// returns every item index in order (hook contract; the tree view draws
    /// that state). `item_count` bounds the returned indices to the item set.
    [[nodiscard]] std::vector<std::size_t> rank_sessions(std::string_view query, std::size_t item_count) const;
    /// The domain empty-state row (search no-match or empty tree/flat view),
    /// styled and truncated to the render width (pi wording).
    [[nodiscard]] std::string empty_state_row(std::size_t width) const;
    /// Forward one event to the embedded SelectList and reconcile the
    /// tree/search seam: when the edit clears an active query, the tree/flat
    /// rows are rebuilt for the next render.
    void forward_to_select_list(const cch::tui::InputEventVariant& input);

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

    /// The embedded search/list widget. It owns the query text and renders
    /// the filtered flat list while a query is active; see the class comment
    /// for the composition seam. Its construction-time ranking never touches
    /// uninitialized members (the initial item set is empty), so it can be
    /// declared alongside the state below it.
    cch::tui::SelectList select_list_;
    /// Row of the search line in the last render's output (rows emitted
    /// above it); cursor_location adds it to SelectList's reported row.
    std::size_t cursor_row_offset_{0};
    /// Row of the rename Input's line in the last rename-mode render output.
    std::optional<std::size_t> rename_input_row_{std::nullopt};
};

} // namespace cch::coding_agent::tui
