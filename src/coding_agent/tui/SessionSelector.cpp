#include "SessionSelector.hpp"

#include "KeybindingHints.hpp"
#include "coding_agent/SessionPathPolicy.hpp"
#include "Theme.hpp"

#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/tui/Utils.hpp>

#include <cch/support/Error.hpp>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cch::coding_agent::tui {
namespace {

constexpr std::size_t kMaxVisibleSessions = 10;

/// pi `shortenPath`: replace the home prefix with `~`.
[[nodiscard]] std::string shorten_path(const std::filesystem::path& path) {
    const auto home = coding_agent::home_directory().string();
    const auto text = path.string();
    if (!text.empty() && !home.empty() && text.starts_with(home)) {
        return "~" + text.substr(home.size());
    }
    return text;
}

/// Trim ASCII whitespace and return the trimmed view.
[[nodiscard]] std::string_view trimmed_view(std::string_view text) {
    const auto begin = std::find_if_not(
            text.begin(), text.end(), [](unsigned char character) { return std::isspace(character) != 0; });
    const auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char character) {
        return std::isspace(character) != 0;
    }).base();
    if (begin >= end) return {};
    return text.substr(static_cast<std::size_t>(begin - text.begin()), static_cast<std::size_t>(end - begin));
}

/// pi `formatSessionDate`: "now", `<n>m`, `<n>h`, `<n>d`, `<n>w`,
/// `<n>mo`, or `<n>y` by elapsed time.
[[nodiscard]] std::string format_session_date(
    std::filesystem::file_time_type modified) {
    const auto now = std::filesystem::file_time_type::clock::now();
    const auto elapsed = now - modified;
    using namespace std::chrono;
    const auto mins = duration_cast<minutes>(elapsed).count();
    if (mins < 1) return "now";
    if (mins < 60) return std::format("{}m", mins);
    const auto hrs = duration_cast<hours>(elapsed).count();
    if (hrs < 24) return std::format("{}h", hrs);
    const auto day_count = duration_cast<days>(elapsed).count();
    if (day_count < 7) return std::format("{}d", day_count);
    if (day_count < 30) return std::format("{}w", day_count / 7);
    if (day_count < 365) return std::format("{}mo", day_count / 30);
    return std::format("{}y", day_count / 365);
}

/// pi `canonicalizePath`: the canonical (symlink-resolved) path, or the
/// original when canonicalization fails.
[[nodiscard]] std::filesystem::path canonicalize_path(
    const std::filesystem::path& path) {
    if (path.empty()) return path;
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    return ec ? path : canonical;
}

/// pi's session-row tail: the message count and age, with the cwd (all
/// scope) and the path (path toggle) prepended.
[[nodiscard]] std::string session_row_tail(
        const session_discovery::SessionInfo& session, bool show_cwd, bool show_path) {
    std::string tail = std::to_string(session.message_count) + " " + format_session_date(session.modified);
    if (show_cwd && !session.cwd.empty()) {
        tail = shorten_path(session.cwd) + " " + tail;
    }
    if (show_path) {
        tail = shorten_path(session.path) + " " + tail;
    }
    return tail;
}

/// pi `deleteSessionFile`: try the `trash` CLI first, then fall back to
/// unlink. The trash attempt captures the first stderr line for the error
/// hint (pi `getTrashErrorHint`).
struct DeleteSessionOutcome {
    bool ok{false};
    bool trashed{false};
    std::string error;
};

[[nodiscard]] DeleteSessionOutcome delete_session_file(
    const std::filesystem::path& session_path) {
    // Spawn `trash` synchronously with a stderr pipe (pi spawnSync).
    int stderr_pipe[2]{-1, -1};
    if (::pipe(stderr_pipe) != 0) {
        stderr_pipe[0] = stderr_pipe[1] = -1;
    }
    const auto path_text = session_path.string();
    const auto child = ::fork();
    if (child == 0) {
        if (stderr_pipe[1] >= 0) {
            (void)::dup2(stderr_pipe[1], STDERR_FILENO);
            (void)::close(stderr_pipe[0]);
            (void)::close(stderr_pipe[1]);
        } else {
            const int null_fd = ::open("/dev/null", O_WRONLY);
            if (null_fd >= 0) {
                (void)::dup2(null_fd, STDERR_FILENO);
                (void)::close(null_fd);
            }
        }
        // pi: a leading-dash path gets `--` so trash never parses it as a
        // flag.
        const char* trash_args[4] = {"trash", nullptr, nullptr, nullptr};
        if (!path_text.empty() && path_text.front() == '-') {
            trash_args[1] = "--";
            trash_args[2] = path_text.c_str();
        } else {
            trash_args[1] = path_text.c_str();
        }
        ::execvp("trash", const_cast<char* const*>(trash_args));
        ::_exit(127);
    }
    std::string stderr_text;
    if (child > 0 && stderr_pipe[0] >= 0) {
        (void)::close(stderr_pipe[1]);
        char buffer[4096];
        ssize_t count = 0;
        while ((count = ::read(stderr_pipe[0], buffer, sizeof(buffer))) > 0) {
            stderr_text.append(buffer, static_cast<std::size_t>(count));
            if (stderr_text.size() > 4096) break;
        }
        (void)::close(stderr_pipe[0]);
    }
    int status = 0;
    if (child > 0) {
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
    } else if (stderr_pipe[0] >= 0) {
        (void)::close(stderr_pipe[0]);
    }

    std::error_code exists_ec;
    const bool gone = !std::filesystem::exists(session_path, exists_ec);
    const bool trashed = child > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (trashed || gone) {
        return DeleteSessionOutcome{.ok = true, .trashed = trashed || gone, .error = {}};
    }

    // Fallback: permanent deletion.
    std::error_code unlink_ec;
    if (std::filesystem::remove(session_path, unlink_ec) && !unlink_ec) {
        return DeleteSessionOutcome{.ok = true, .trashed = false, .error = {}};
    }
    std::string hint;
    if (!stderr_text.empty()) {
        auto first_line = stderr_text;
        if (const auto newline = first_line.find('\n'); newline != std::string::npos) {
            first_line.resize(newline);
        }
        hint = " (trash: " + first_line + ")";
    }
    return {
        .ok = false,
        .trashed = false,
        .error = unlink_ec ? unlink_ec.message() + hint : "could not delete session file" + hint,
    };
}

/// pi `normalizeMessageText`: control characters become spaces, then trim.
[[nodiscard]] std::string normalize_message_text(std::string text) {
    for (auto& character : text) {
        const auto value = static_cast<unsigned char>(character);
        if (value < 0x20 || value == 0x7f) {
            character = ' ';
        }
    }
    const auto not_space = [](unsigned char character) {
        return std::isspace(character) == 0;
    };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
}

/// pi `buildSessionTree`: sessions as a forest keyed by canonical path,
/// children attached through `parentSessionPath`, sorted by latest activity
/// (recursively, descending).
struct SessionTreeNode {
    const session_discovery::SessionInfo* session{nullptr};
    std::vector<SessionTreeNode> children;
    std::filesystem::file_time_type latest_activity{};
};

void update_latest_activity(SessionTreeNode& node) {
    auto latest = node.session->modified;
    for (auto& child : node.children) {
        update_latest_activity(child);
        latest = std::max(latest, child.latest_activity);
    }
    node.latest_activity = latest;
}

void sort_tree_nodes(std::vector<SessionTreeNode>& nodes) {
    std::sort(nodes.begin(), nodes.end(), [](const SessionTreeNode& first, const SessionTreeNode& second) {
        return first.latest_activity > second.latest_activity;
    });
    for (auto& node : nodes) {
        sort_tree_nodes(node.children);
    }
}

} // namespace

SessionSelectorComponent::SessionSelectorComponent(const LiveTheme& theme,
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
        SessionListLoader current_loader,
        SessionListLoader all_loader,
        std::optional<std::filesystem::path> current_session_path,
        SessionSelectorSelectSink on_select,
        SessionSelectorCancelSink on_cancel,
        SessionSelectorExitSink on_exit,
        SessionSelectorRenameSink on_rename,
        SessionSelectorInvalidateSink on_invalidate)
    : theme_(theme), keybindings_(std::move(keybindings)), current_loader_(std::move(current_loader)),
      all_loader_(std::move(all_loader)), current_session_path_(std::move(current_session_path)),
      on_select_(std::move(on_select)), on_cancel_(std::move(on_cancel)), on_exit_(std::move(on_exit)),
      on_rename_(std::move(on_rename)), on_invalidate_(std::move(on_invalidate)),
      // pi `renameInput.onSubmit`: confirm the rename and refresh.
      rename_input_(cch::tui::InputOptions{},
              [this](std::string value) -> support::ExpectedVoid {
                  confirm_rename(std::move(value));
                  return {};
              }),
      // List/search presentation lives in the shared SelectList (search
      // enabled): it owns the query text, the filtered rows and the search
      // cursor. The session-specific token/regex matching and relevance
      // ordering ride through the ranking hook; the confirm/cancel sinks
      // re-enter this component. The item set starts empty and is populated
      // below (its construction-time ranking never sees uninitialized
      // state).
      select_list_(std::vector<cch::tui::SelectItem>{},
              cch::tui::SelectListOptions{
                      .max_visible = kMaxVisibleSessions,
                      .theme = theme_.select_list_theme(),
                      .on_select = [this](const cch::tui::SelectItem& item) -> support::ExpectedVoid {
                          select_session(item);
                          return {};
                      },
                      .on_cancel = [this]() -> support::ExpectedVoid {
                          if (on_cancel_) on_cancel_();
                          return {};
                      },
                      .keybindings = keybindings_,
                      .enable_search = true,
                      .search_filter_hook =
                              [this](std::string_view query, const std::vector<cch::tui::SelectItem>& items) {
                                  return rank_sessions(query, items.size());
                              },
              }) {
    if (current_loader_) {
        current_sessions_ = current_loader_();
    }
    refresh_presentation();
}

void SessionSelectorComponent::set_scope(bool all) {
    scope_all_ = all;
    show_cwd_ = all;
    if (all) {
        if (all_loader_ && all_sessions_.empty()) {
            all_sessions_ = all_loader_();
        }
    }
    refresh_presentation();
}

void SessionSelectorComponent::set_sort_mode(SessionSortMode mode) {
    sort_mode_ = mode;
    refresh_presentation();
}

void SessionSelectorComponent::set_name_filter(SessionNameFilter filter) {
    name_filter_ = filter;
    refresh_presentation();
}

/// Rebuild both presentation sources after any change to the scope, the
/// scope lists, the sort mode, or the name filter: the tree/flat rows (the
/// empty-query view) and the SelectList item set (its ranking re-runs
/// against the current query and component state, preserving the selection
/// by session path when still present).
void SessionSelectorComponent::refresh_presentation() {
    filter_sessions();
    select_list_.set_items(build_select_items());
}

void SessionSelectorComponent::build_tree(
    const std::vector<session_discovery::SessionInfo>& sessions) {
    // One node per session; children attach by canonical parent path. The
    // attachment pass runs before any node moves so later children never
    // land on moved-from nodes.
    struct CanonicalEntry {
        std::filesystem::path path;
        std::size_t index;
    };
    std::vector<CanonicalEntry> canonical;
    canonical.reserve(sessions.size());
    for (std::size_t index = 0; index < sessions.size(); ++index) {
        canonical.push_back(CanonicalEntry{
            .path = canonicalize_path(sessions[index].path),
            .index = index,
        });
    }
    std::vector<std::vector<std::size_t>> children_by_index(sessions.size());
    std::vector<std::size_t> root_indices;
    for (std::size_t index = 0; index < sessions.size(); ++index) {
        const auto& session = sessions[index];
        const auto parent = canonicalize_path(
            session.parent_session_path.value_or(std::filesystem::path{}));
        if (parent.empty()) {
            root_indices.push_back(index);
            continue;
        }
        const auto found = std::find_if(
            canonical.begin(), canonical.end(),
            [&](const CanonicalEntry& entry) { return entry.path == parent; });
        if (found == canonical.end()) {
            root_indices.push_back(index);
        } else {
            children_by_index[found->index].push_back(index);
        }
    }

    std::vector<SessionTreeNode> roots;
    roots.reserve(root_indices.size());
    // Build each root recursively by index so a parent node always moves
    // after its own children attached (never from a moved-from slot).
    const auto build_node = [&](auto&& self, std::size_t index) -> SessionTreeNode {
        SessionTreeNode node;
        node.session = &sessions[index];
        node.latest_activity = sessions[index].modified;
        for (const std::size_t child : children_by_index[index]) {
            node.children.push_back(self(self, child));
        }
        return node;
    };
    for (const std::size_t root : root_indices) {
        roots.push_back(build_node(build_node, root));
    }
    for (auto& root : roots) {
        update_latest_activity(root);
    }
    sort_tree_nodes(roots);

    // Flatten the tree into the display list (pi flattenSessionTree).
    filtered_sessions_.clear();
    const auto walk = [&](auto&& self, const SessionTreeNode& node, std::size_t depth,
                          std::vector<bool> ancestor_continues, bool is_last) -> void {
        filtered_sessions_.push_back(FlatSessionNode{
            .session = node.session,
            .depth = depth,
            .is_last = is_last,
            .ancestor_continues = ancestor_continues,
        });
        for (std::size_t child_index = 0; child_index < node.children.size(); ++child_index) {
            auto continues = ancestor_continues;
            continues.push_back(depth > 0 ? !is_last : false);
            self(
                self,
                node.children[child_index],
                depth + 1,
                std::move(continues),
                child_index == node.children.size() - 1);
        }
    };
    for (std::size_t index = 0; index < roots.size(); ++index) {
        walk(walk, roots[index], 0, {}, index == roots.size() - 1);
    }
}

void SessionSelectorComponent::filter_sessions() {
    // The tree nodes reference the member vectors directly (never a local
    // copy); the flat filter path works on a copy so the member storage
    // survives re-filtering.
    auto& sessions = scope_all_ ? all_sessions_ : current_sessions_;
    const auto query = select_list_.search_query();
    const auto trimmed = trimmed_view(query);
    if (sort_mode_ == SessionSortMode::Threaded && trimmed.empty()) {
        if (name_filter_ == SessionNameFilter::Named) {
            // The tree nodes reference the storage, so the named subset must
            // live in the member storage (never a local copy).
            active_filtered_storage_.clear();
            for (const auto& session : sessions) {
                if (has_session_name(session)) {
                    active_filtered_storage_.push_back(session);
                }
            }
            build_tree(active_filtered_storage_);
        } else {
            build_tree(sessions);
        }
    } else {
        auto filtered = filter_and_sort_sessions(
            std::vector<session_discovery::SessionInfo>{sessions},
            query,
            sort_mode_,
            name_filter_);
        filtered_sessions_.clear();
        filtered_sessions_.reserve(filtered.size());
        // The flattened nodes reference the filtered vector — but the filter
        // returns by value, so the session pointers must point at stable
        // storage. Store the filtered list in a member and reference it.
        active_filtered_storage_ = std::move(filtered);
        for (auto& session : active_filtered_storage_) {
            filtered_sessions_.push_back(FlatSessionNode{
                .session = &session,
                .depth = 0,
                .is_last = true,
                .ancestor_continues = {},
            });
        }
    }
    selected_index_ = std::min(
        selected_index_,
        filtered_sessions_.empty() ? 0 : filtered_sessions_.size() - 1);
}

void SessionSelectorComponent::refresh_after_mutation() {
    if (scope_all_) {
        if (all_loader_) {
            all_sessions_ = all_loader_();
        }
    } else if (current_loader_) {
        current_sessions_ = current_loader_();
    }
    refresh_presentation();
    if (on_invalidate_) on_invalidate_();
}

void SessionSelectorComponent::set_status_message(std::string message, bool error) {
    status_message_ = std::make_pair(std::move(message), error);
}

bool SessionSelectorComponent::is_current_session_path(
    const std::filesystem::path& path) const {
    if (!current_session_path_) return false;
    return canonicalize_path(path) == canonicalize_path(*current_session_path_);
}

const std::vector<session_discovery::SessionInfo>& SessionSelectorComponent::scope_sessions() const {
    return scope_all_ ? all_sessions_ : current_sessions_;
}

bool SessionSelectorComponent::search_active() const { return !trimmed_view(select_list_.search_query()).empty(); }

const session_discovery::SessionInfo* SessionSelectorComponent::selected_session() const {
    if (search_active()) {
        const auto item = select_list_.selected_item();
        if (!item) return nullptr;
        for (const auto& session : scope_sessions()) {
            if (session.path.string() == item->value) return &session;
        }
        return nullptr;
    }
    if (filtered_sessions_.empty()) return nullptr;
    return filtered_sessions_[selected_index_].session;
}

std::vector<cch::tui::SelectItem> SessionSelectorComponent::build_select_items() const {
    const auto& sessions = scope_sessions();
    std::vector<cch::tui::SelectItem> items;
    items.reserve(sessions.size());
    for (const auto& session : sessions) {
        const bool has_name = has_session_name(session);
        auto display_text = has_name ? *session.name : session.first_message;
        display_text = normalize_message_text(std::move(display_text));
        // Row styling mirrors the tree rows: named sessions warn, the
        // current session accents, the pending delete target errors.
        std::string label;
        if (is_current_session_path(session.path)) {
            label = theme_.foreground(ThemeToken::Accent, std::move(display_text));
        } else if (has_name) {
            label = theme_.foreground(ThemeToken::Warning, std::move(display_text));
        } else {
            label = std::move(display_text);
        }
        if (confirming_delete_path_ == session.path.string()) {
            label = theme_.foreground(ThemeToken::Error, label);
        }
        items.push_back(cch::tui::SelectItem{
                .value = session.path.string(),
                .label = std::move(label),
                .description = session_row_tail(session, show_cwd_, show_path_),
        });
    }
    return items;
}

std::vector<std::size_t> SessionSelectorComponent::rank_sessions(std::string_view query, std::size_t item_count) const {
    const auto& sessions = scope_sessions();
    // The hook contract: an empty (or whitespace) query returns every item
    // index in order. The component draws its own tree/flat view in that
    // state, so the SelectList body never shows with an empty query.
    if (trimmed_view(query).empty()) {
        std::vector<std::size_t> indices;
        const auto count = std::min(item_count, sessions.size());
        indices.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            indices.push_back(index);
        }
        return indices;
    }
    auto ordered = filter_and_sort_sessions(
            std::vector<session_discovery::SessionInfo>{sessions}, query, sort_mode_, name_filter_);
    // filter_and_sort_sessions works on copies, so map each ordered session
    // back to the item index of its source slot by path.
    std::vector<std::size_t> indices;
    indices.reserve(ordered.size());
    for (const auto& session : ordered) {
        const auto limit = std::min(item_count, sessions.size());
        for (std::size_t index = 0; index < limit; ++index) {
            if (sessions[index].path == session.path) {
                indices.push_back(index);
                break;
            }
        }
    }
    return indices;
}

void SessionSelectorComponent::select_session(const cch::tui::SelectItem& item) {
    // The SelectList rows carry the session path as `value`; only sessions
    // still present in the active scope resolve (mirrors the tree Enter
    // guard).
    for (const auto& session : scope_sessions()) {
        if (session.path.string() == item.value) {
            if (on_select_) on_select_(session.path.string());
            return;
        }
    }
}

void SessionSelectorComponent::start_delete_confirmation() {
    const auto* selected = selected_session();
    if (selected == nullptr) return;
    if (is_current_session_path(selected->path)) {
        set_status_message("Cannot delete the currently active session", true);
        if (on_invalidate_) on_invalidate_();
        return;
    }
    confirming_delete_path_ = selected->path.string();
    // Rebuild the item set so the confirming row carries the error styling
    // in the search view (the tree view checks the flag at render time).
    select_list_.set_items(build_select_items());
    if (on_invalidate_) on_invalidate_();
}

void SessionSelectorComponent::confirm_delete() {
    if (!confirming_delete_path_) return;
    const auto path = std::filesystem::path{*confirming_delete_path_};
    confirming_delete_path_ = std::nullopt;
    const auto outcome = delete_session_file(path);
    if (outcome.ok) {
        std::erase_if(current_sessions_, [&](const session_discovery::SessionInfo& session) {
            return session.path == path;
        });
        std::erase_if(all_sessions_, [&](const session_discovery::SessionInfo& session) {
            return session.path == path;
        });
        refresh_after_mutation();
        set_status_message(
            outcome.trashed ? "Session moved to trash" : "Session deleted", false);
    } else {
        set_status_message("Failed to delete: " + outcome.error, true);
    }
    if (on_invalidate_) on_invalidate_();
}

void SessionSelectorComponent::cancel_delete() {
    confirming_delete_path_ = std::nullopt;
    // Drop the error styling from the search-view rows again.
    select_list_.set_items(build_select_items());
    if (on_invalidate_) on_invalidate_();
}

void SessionSelectorComponent::enter_rename_mode(
    const session_discovery::SessionInfo& session) {
    mode_rename_ = true;
    rename_target_path_ = session.path.string();
    rename_input_.set_value(session.name.value_or(""));
    rename_input_.set_focused(true);
    if (on_invalidate_) on_invalidate_();
}

void SessionSelectorComponent::exit_rename_mode() {
    mode_rename_ = false;
    rename_target_path_.clear();
    if (on_invalidate_) on_invalidate_();
}

void SessionSelectorComponent::confirm_rename(std::string value) {
    const auto target = std::move(rename_target_path_);
    exit_rename_mode();
    if (!on_rename_) return;
    const auto next = std::string{value};
    const auto first = std::find_if_not(next.begin(), next.end(), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
    const auto last = std::find_if_not(next.rbegin(), next.rend(), [](unsigned char character) {
        return std::isspace(character) != 0;
    }).base();
    if (first >= last) return;
    if (auto renamed = on_rename_(
            target,
            next.substr(
                static_cast<std::size_t>(first - next.begin()),
                static_cast<std::size_t>(last - first)));
        !renamed) {
        set_status_message(renamed.error().message, true);
    }
    refresh_after_mutation();
}

void SessionSelectorComponent::set_focused(bool focused) {
    focused_ = focused;
    if (mode_rename_) {
        rename_input_.set_focused(focused);
    } else {
        select_list_.set_focused(focused);
    }
}

bool SessionSelectorComponent::focused() const {
    return focused_;
}

std::optional<cch::tui::CursorPosition> SessionSelectorComponent::cursor_location() const {
    if (mode_rename_) {
        const auto cursor = rename_input_.cursor_location();
        if (!cursor || !rename_input_row_) return std::nullopt;
        return cch::tui::CursorPosition{
                .column = cursor->column,
                .row = *rename_input_row_ + cursor->row,
        };
    }
    // The search cursor lives in the embedded SelectList (its own search
    // line coordinate); the rows this component emits above it (border,
    // header, hint rows, spacer) are added on top. The offset is recorded
    // when the search line renders — never a magic constant.
    const auto cursor = select_list_.cursor_location();
    if (!cursor) return std::nullopt;
    return cch::tui::CursorPosition{
            .column = cursor->column,
            .row = cursor->row + cursor_row_offset_,
    };
}

void SessionSelectorComponent::handle_input(const cch::tui::InputEventVariant& input) {
    const auto* key = std::get_if<cch::tui::KeyEvent>(&input);

    if (mode_rename_) {
        if (key == nullptr) return;
        if (keybindings_->matches(*key, "tui.select.cancel")) {
            exit_rename_mode();
            return;
        }
        rename_input_.handle_input(input);
        return;
    }

    // Delete confirmation intercepts all keys (pi).
    if (confirming_delete_path_) {
        if (key == nullptr) return;
        if (keybindings_->matches(*key, "tui.select.confirm")) {
            confirm_delete();
        } else if (keybindings_->matches(*key, "tui.select.cancel")) {
            cancel_delete();
        }
        return;
    }

    if (key == nullptr) {
        // Pasted text (and any other non-key event) edits the search query;
        // the SelectList owns the search Input in both the tree and search
        // views, so the paste flows there from either.
        forward_to_select_list(input);
        return;
    }

    // Domain actions stay component-level pre-dispatch in both views (pi
    // routes them before the list sees the key).
    if (keybindings_->matches(*key, "tui.input.tab")) {
        set_scope(!scope_all_);
        if (on_invalidate_) on_invalidate_();
        return;
    }
    if (keybindings_->matches(*key, "app.session.toggleSort")) {
        set_sort_mode(
            sort_mode_ == SessionSortMode::Threaded
                ? SessionSortMode::Recent
                : sort_mode_ == SessionSortMode::Recent
                    ? SessionSortMode::Relevance
                    : SessionSortMode::Threaded);
        if (on_invalidate_) on_invalidate_();
        return;
    }
    if (keybindings_->matches(*key, "app.session.toggleNamedFilter")) {
        set_name_filter(
            name_filter_ == SessionNameFilter::All
                ? SessionNameFilter::Named
                : SessionNameFilter::All);
        if (on_invalidate_) on_invalidate_();
        return;
    }
    if (keybindings_->matches(*key, "app.session.togglePath")) {
        show_path_ = !show_path_;
        select_list_.set_items(build_select_items());
        if (on_invalidate_) on_invalidate_();
        return;
    }
    if (keybindings_->matches(*key, "app.session.delete")) {
        start_delete_confirmation();
        return;
    }
    if (keybindings_->matches(*key, "app.session.rename")) {
        const auto* selected = selected_session();
        if (selected != nullptr) {
            enter_rename_mode(*selected);
        }
        return;
    }
    if (keybindings_->matches(*key, "app.session.deleteNoninvasive")) {
        // "Delete session when the query is empty": while a query is active
        // the key edits the search text (pi deleteKey) instead.
        if (!select_list_.search_query().empty()) {
            forward_to_select_list(input);
        } else {
            start_delete_confirmation();
        }
        return;
    }

    if (search_active()) {
        // Search view: navigation, confirm/cancel and search editing belong
        // to the SelectList (its sinks re-enter this component).
        forward_to_select_list(input);
        return;
    }

    // Tree view: navigation and selection keys stay component-level.
    if (keybindings_->matches(*key, "tui.select.up")) {
        if (!filtered_sessions_.empty()) {
            selected_index_ = selected_index_ == 0 ? 0 : selected_index_ - 1;
        }
        if (on_invalidate_) on_invalidate_();
        return;
    }
    if (keybindings_->matches(*key, "tui.select.down")) {
        if (!filtered_sessions_.empty()) {
            selected_index_ = std::min(
                filtered_sessions_.size() - 1, selected_index_ + 1);
        }
        if (on_invalidate_) on_invalidate_();
        return;
    }
    if (keybindings_->matches(*key, "tui.select.pageUp")) {
        selected_index_ = selected_index_ > kMaxVisibleSessions
            ? selected_index_ - kMaxVisibleSessions
            : 0;
        if (on_invalidate_) on_invalidate_();
        return;
    }
    if (keybindings_->matches(*key, "tui.select.pageDown")) {
        if (!filtered_sessions_.empty()) {
            selected_index_ = std::min(
                filtered_sessions_.size() - 1,
                selected_index_ + kMaxVisibleSessions);
        }
        if (on_invalidate_) on_invalidate_();
        return;
    }
    if (keybindings_->matches(*key, "tui.select.confirm")) {
        const auto* selected = selected_session();
        if (selected != nullptr && on_select_) {
            on_select_(selected->path.string());
        }
        return;
    }
    if (keybindings_->matches(*key, "tui.select.cancel")) {
        if (on_cancel_) {
            on_cancel_();
        }
        return;
    }
    // Remaining keys edit the search query through the SelectList's Input
    // (typing the first character hands the view over to the search list).
    forward_to_select_list(input);
}

/// Forward one event to the embedded SelectList and reconcile the
/// tree/search seam: clearing an active query hands the view back to this
/// component's tree/flat rows (rebuilt here so the next render draws the
/// empty-query state with the current scope/sort/filter), with the
/// selection back on the top row (SelectList's own clear-query policy).
void SessionSelectorComponent::forward_to_select_list(const cch::tui::InputEventVariant& input) {
    const bool was_search = search_active();
    select_list_.handle_input(input);
    if (was_search && !search_active()) {
        filter_sessions();
        selected_index_ = 0;
    }
    if (on_invalidate_) on_invalidate_();
}

std::string SessionSelectorComponent::empty_state_row(std::size_t width) const {
    // pi's empty messages depend on the named filter and the scope only.
    std::string message;
    if (name_filter_ == SessionNameFilter::Named) {
        const auto toggle_key = format_key_text(keybindings_->key_text("app.session.toggleNamedFilter"), true);
        if (show_cwd_) {
            message = "  No named sessions found. Press " + toggle_key + " to show all.";
        } else {
            message =
                    "  No named sessions in current folder. Press " + toggle_key + " to show all, or Tab to view all.";
        }
    } else if (show_cwd_) {
        message = "  No sessions found";
    } else {
        message = "  No sessions in current folder. Press Tab to view all.";
    }
    return theme_.foreground(
            ThemeToken::Muted, cch::tui::truncate_text(message, width, "…").value_or(std::move(message)));
}

support::Expected<cch::tui::RenderResult> SessionSelectorComponent::render(
    std::size_t width) {
    std::vector<std::string> lines;
    const auto border = [&]() {
        std::string rule;
        rule.reserve(width * 3);
        const auto count = width > 0 ? width : std::size_t{1};
        for (std::size_t index = 0; index < count; ++index) rule += "─";
        return theme_.foreground(ThemeToken::Border, rule);
    };
    rename_input_row_.reset();

    // pi buildBaseLayout: spacer + dynamic border + spacer + header + list.
    lines.push_back("");
    lines.push_back(border());
    lines.push_back("");

    // ── Header ────────────────────────────────────────────────────────────
    const auto title = scope_all_
        ? "Resume Session (All)"
        : "Resume Session (Current Folder)";
    const auto sort_label = sort_mode_ == SessionSortMode::Threaded
        ? "Threaded"
        : sort_mode_ == SessionSortMode::Recent ? "Recent" : "Fuzzy";
    const auto name_label = name_filter_ == SessionNameFilter::All ? "All" : "Named";
    const auto right_text = [&]() {
        std::string text = scope_all_
            ? theme_.foreground(ThemeToken::Muted, "○ Current Folder | ") +
                theme_.foreground(ThemeToken::Accent, "◉ All")
            : theme_.foreground(ThemeToken::Accent, "◉ Current Folder") +
                theme_.foreground(ThemeToken::Muted, " | ○ All");
        text += "  ";
        text += theme_.foreground(ThemeToken::Muted, "Name: ") +
            theme_.foreground(ThemeToken::Accent, name_label);
        text += "  ";
        text += theme_.foreground(ThemeToken::Muted, "Sort: ") +
            theme_.foreground(ThemeToken::Accent, sort_label);
        return text;
    }();
    const auto left_text = theme_.foreground(ThemeToken::Accent, title);
    const auto right_width = cch::tui::visible_width(right_text);
    const auto left_width =
        width > right_width + 1 ? width - right_width - 1 : std::size_t{0};
    auto truncated_left = cch::tui::truncate_text(left_text, left_width, "");
    if (!truncated_left) return std::unexpected(truncated_left.error());
    auto left = std::move(*truncated_left);
    const auto spacing = width > cch::tui::visible_width(left) + right_width
        ? width - cch::tui::visible_width(left) - right_width
        : std::size_t{0};
    lines.push_back(left + std::string(spacing, ' ') + right_text);

    // Hint lines (all branches truncate to width).
    std::string hint_line_1;
    std::string hint_line_2;
    if (confirming_delete_path_) {
        const auto confirm_hint =
            "Delete session? " +
            key_hint(theme_, *keybindings_, "tui.select.confirm", "confirm") +
            " · " +
            key_hint(theme_, *keybindings_, "tui.select.cancel", "cancel");
        hint_line_1 = theme_.foreground(
            ThemeToken::Error,
            cch::tui::truncate_text(confirm_hint, width, "…").value_or(confirm_hint));
        hint_line_2 = "";
    } else if (status_message_) {
        const auto color = status_message_->second ? ThemeToken::Error : ThemeToken::Accent;
        hint_line_1 = theme_.foreground(
            color,
            cch::tui::truncate_text(status_message_->first, width, "…").value_or(
                status_message_->first));
        hint_line_2 = "";
    } else {
        const auto sep = theme_.foreground(ThemeToken::Muted, " · ");
        const auto hint1 =
            key_hint(theme_, *keybindings_, "tui.input.tab", "scope") +
            sep + theme_.foreground(
                      ThemeToken::Muted, "re:<pattern> regex · \"phrase\" exact");
        const std::string path_state = show_path_ ? "(on)" : "(off)";
        auto hint2 =
            key_hint(theme_, *keybindings_, "app.session.toggleSort", "sort") +
            sep +
            key_hint(theme_, *keybindings_, "app.session.toggleNamedFilter", "named") +
            sep +
            key_hint(theme_, *keybindings_, "app.session.delete", "delete") +
            sep +
            key_hint(
                theme_, *keybindings_, "app.session.togglePath",
                "path " + path_state);
        if (on_rename_) {
            hint2 += sep + key_hint(theme_, *keybindings_, "app.session.rename", "rename");
        }
        hint_line_1 = cch::tui::truncate_text(hint1, width, "…").value_or(hint1);
        hint_line_2 = cch::tui::truncate_text(hint2, width, "…").value_or(hint2);
    }
    lines.push_back(hint_line_1);
    lines.push_back(hint_line_2);
    lines.push_back("");

    // ── Search input ──────────────────────────────────────────────────────
    // The search line renders through the embedded SelectList in every state
    // (it owns the query text); record its row so cursor_location can add
    // the component-owned rows above it (never a magic constant). While a
    // search query is active the SelectList also renders the filtered rows
    // (its own body); otherwise only its search row is kept and this
    // component draws the tree/flat view (or the rename mode) below.
    cursor_row_offset_ = lines.size();
    const auto search_view = !mode_rename_ && search_active();
    const auto select_begin = lines.size();
    auto select_rendered = select_list_.render(width);
    if (!select_rendered) return std::unexpected(select_rendered.error());
    if (search_view) {
        for (auto& line : select_rendered->lines) {
            lines.push_back(std::move(line));
        }
        // The SelectList's generic no-match row is swapped for the domain
        // empty-state message: its wording depends on the name filter and
        // scope, which change while the list is open (the toolkit row text
        // is fixed at construction). With search enabled and no chrome the
        // no-match row is the line directly after the search row.
        if (!select_list_.selected_item() && lines.size() >= select_begin + 2) {
            lines[select_begin + 1] = empty_state_row(width);
        }
    } else if (!select_rendered->lines.empty()) {
        lines.push_back(std::move(select_rendered->lines.front()));
    }
    lines.push_back("");

    if (mode_rename_) {
        lines.push_back(theme_.foreground(ThemeToken::Accent, "Rename Session"));
        lines.push_back("");
        rename_input_row_ = lines.size();
        auto rename_lines = rename_input_.render(width);
        if (!rename_lines) return std::unexpected(rename_lines.error());
        lines.insert(
            lines.end(),
            std::make_move_iterator(rename_lines->lines.begin()),
            std::make_move_iterator(rename_lines->lines.end()));
        lines.push_back("");
        lines.push_back(theme_.foreground(
            ThemeToken::Muted,
            format_key_text(keybindings_->key_text("tui.select.confirm"), true) +
                " to save · " +
                format_key_text(keybindings_->key_text("tui.select.cancel"), true) +
                " to cancel"));
        return cch::tui::RenderResult{.lines = std::move(lines)};
    }

    if (search_view) {
        lines.push_back(border());
        return cch::tui::RenderResult{.lines = std::move(lines)};
    }

    // ── Session list (empty-query view) ───────────────────────────────────
    if (filtered_sessions_.empty()) {
        lines.push_back(empty_state_row(width));
        lines.push_back("");
        lines.push_back(border());
        return cch::tui::RenderResult{.lines = std::move(lines)};
    }

    const auto start_index = std::max<std::size_t>(
        0,
        std::min(
            selected_index_ > kMaxVisibleSessions / 2
                ? selected_index_ - kMaxVisibleSessions / 2
                : 0,
            filtered_sessions_.size() > kMaxVisibleSessions
                ? filtered_sessions_.size() - kMaxVisibleSessions
                : 0));
    const auto end_index = std::min(
        start_index + kMaxVisibleSessions, filtered_sessions_.size());

    for (std::size_t index = start_index; index < end_index; ++index) {
        const auto& node = filtered_sessions_[index];
        const auto& session = *node.session;
        const bool is_selected = index == selected_index_;
        const bool is_confirming_delete =
            confirming_delete_path_ == session.path.string();
        const bool is_current = is_current_session_path(session.path);

        // Tree prefix (pi buildTreePrefix: roots carry none).
        std::string prefix;
        if (node.depth > 0) {
            for (const bool continues : node.ancestor_continues) {
                prefix += continues ? "│  " : "   ";
            }
            prefix += node.is_last ? "└─ " : "├─ ";
        }

        // Display text: name or first message.
        const auto has_name = has_session_name(session);
        auto display_text = has_name
            ? *session.name
            : session.first_message;
        display_text = normalize_message_text(std::move(display_text));

        // Right side: message count and age, cwd, path.
        const auto right_part = session_row_tail(session, show_cwd_, show_path_);

        const auto cursor = is_selected
            ? theme_.foreground(ThemeToken::Accent, "› ")
            : "  ";
        const auto prefix_width = cch::tui::visible_width(prefix);
        const auto right_width = cch::tui::visible_width(right_part) + 2;
        const auto available_for_message = width > 2 + prefix_width + right_width
            ? width - 2 - prefix_width - right_width
            : std::size_t{10};
        auto truncated_message = cch::tui::truncate_text(
            display_text, std::max<std::size_t>(10, available_for_message), "…");
        if (!truncated_message) return std::unexpected(truncated_message.error());

        std::string styled_message;
        if (is_confirming_delete) {
            styled_message = theme_.foreground(ThemeToken::Error, *truncated_message);
        } else if (is_current) {
            styled_message = theme_.foreground(ThemeToken::Accent, *truncated_message);
        } else if (has_name) {
            styled_message = theme_.foreground(ThemeToken::Warning, *truncated_message);
        } else {
            styled_message = *truncated_message;
        }
        if (is_selected) {
            styled_message = "\x1b[1m" + styled_message + "\x1b[22m";
        }

        const auto left_part =
            cursor + theme_.foreground(ThemeToken::Dim, prefix) + styled_message;
        const auto left_width = cch::tui::visible_width(left_part);
        const auto spacing = width > left_width + cch::tui::visible_width(right_part)
            ? std::max<std::size_t>(1, width - left_width - cch::tui::visible_width(right_part))
            : std::size_t{1};
        auto styled_right = theme_.foreground(
            is_confirming_delete ? ThemeToken::Error : ThemeToken::Dim, right_part);
        auto line = left_part + std::string(spacing, ' ') + styled_right;
        if (is_selected) {
            line = theme_.background(ThemeToken::SelectedBg, line);
        }
        lines.push_back(cch::tui::truncate_text(line, width, "").value_or(line));
    }

    // Scroll indicator.
    if (start_index > 0 || end_index < filtered_sessions_.size()) {
        const auto scroll_text = std::format(
            "  ({}/{})", selected_index_ + 1, filtered_sessions_.size());
        lines.push_back(theme_.foreground(
            ThemeToken::Muted,
            cch::tui::truncate_text(scroll_text, width, "").value_or(scroll_text)));
    }
    lines.push_back("");
    lines.push_back(border());

    return cch::tui::RenderResult{.lines = std::move(lines)};
}

} // namespace cch::coding_agent::tui
