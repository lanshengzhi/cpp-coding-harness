#pragma once

#include "coding_agent/tui/Theme.hpp"

#include <cch/harness/session/SessionTree.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Input.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/util/Error.hpp>
#include <cch/util/JsonValue.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace cch::coding_agent::tui {

/// pi `onSelect`: one tree entry chosen for navigation.
using TreeSelectSink = std::move_only_function<void(std::string)>;
/// pi `onCancel`: the selector cancelled (no selection).
using TreeCancelSink = std::move_only_function<void()>;
/// pi `onLabelChange` (editLabel): a label entry for `entry_id` was
/// committed; `label` is nullopt when the label was cleared. The host posts
/// the store write to the session executor; the component display already
/// shows the committed label.
using TreeLabelChangeSink = std::move_only_function<void(
    std::string,
    std::optional<std::string>)>;
/// pi `onCopy`: the selected entry's copy text, or nullopt when the entry
/// has no text to copy.
using TreeCopySink = std::move_only_function<void(std::optional<std::string>)>;
using TreeInvalidateSink = std::move_only_function<void()>;

/// Filter mode for tree display (pi tree-selector.ts `FilterMode`).
enum class TreeFilterMode { Default, NoTools, UserOnly, LabeledOnly, All };

/// The in-session session tree selector (pi `tree-selector.ts`, G2 decision
/// 13): the session topology as an ASCII-art tree with indentation,
/// connectors, and continuation gutters; the active path marker; the five
/// filter modes (`default`/`no-tools`/`user-only`/`labeled-only`/`all`), the
/// fold/unfold branch navigation, the search-while-typing query, the label
/// display with the `shift+t` timestamp toggle, the `app.message.copy` entry
/// copy, and the `shift+l` inline label editor ("Label (empty to remove):").
/// The eleven `app.tree.*` actions are matched inside the component through
/// the shared registry, exactly like pi's selector-scoped bindings.
///
/// Layout follows pi: spacer, border, the bold `  Session Tree` title, the
/// semantic TreeHelp rows, the `Type to search:` line, border, spacer, the
/// tree rows (max(5, terminalHeight / 2) visible with the `(N/M)` footer) or
/// the label input, spacer, border. Rendering and input run on the TUI
/// thread; navigation, label persistence, and copy post to the session
/// executor through the sinks.
class TreeSelectorComponent final
    : public cch::tui::Component,
      public cch::tui::InputHandler,
      public cch::tui::Focusable,
      public std::enable_shared_from_this<TreeSelectorComponent> {
public:
    TreeSelectorComponent(
        const LiveTheme& theme,
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
        std::vector<harness::session::SessionTreeNode> tree,
        std::string current_leaf_id,
        std::size_t terminal_height,
        TreeSelectSink on_select,
        TreeCancelSink on_cancel,
        TreeLabelChangeSink on_label_change,
        TreeCopySink on_copy,
        TreeInvalidateSink on_invalidate);
    TreeSelectorComponent& operator=(TreeSelectorComponent&&) = delete;
    ~TreeSelectorComponent() override = default;
    TreeSelectorComponent(const TreeSelectorComponent&) = delete;
    TreeSelectorComponent& operator=(const TreeSelectorComponent&) = delete;

    [[nodiscard]] util::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override {}
    void handle_input(const cch::tui::InputEventVariant& input) override;
    [[nodiscard]] bool accepts_key_releases() const override { return false; }
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override;

    /// The label-edit Input (pi `LabelInput` seam for tests).
    [[nodiscard]] cch::tui::Input& label_input() { return label_input_; }

    /// pi `formatLabelTimestamp`: the label timestamp display — `HH:MM` for
    /// today, `M/D HH:MM` for this year, `YY/M/D HH:MM` otherwise (local
    /// time). Exposed for tests.
    [[nodiscard]] static std::string format_label_timestamp(
        std::int64_t timestamp_ms,
        std::int64_t now_ms);

    /// Gutter info: position (displayIndent level where the connector was
    /// shown) and whether to show `│` (pi `GutterInfo`).
    struct GutterInfo {
        std::size_t position{0};
        bool show{false};
    };

    /// Flattened tree node for navigation (pi `FlatNode`).
    struct FlatNode {
        harness::session::SessionTreeNode* node{nullptr};
        /// Indentation level (each level = 3 chars).
        std::size_t indent{0};
        /// Whether to show connector (`├─` or `└─`) — true if the parent has
        /// multiple children.
        bool show_connector{false};
        /// If show_connector, true = last sibling (`└─`), false = not last
        /// (`├─`).
        bool is_last{false};
        /// Gutter info for each ancestor branch point.
        std::vector<GutterInfo> gutters;
        /// True if this node is a root under a virtual branching root
        /// (multiple roots).
        bool is_virtual_root_child{false};
    };

    /// Tool call info for the toolResult display lookup (pi `ToolCallInfo`).
    struct ToolCallInfo {
        std::string name;
        util::JsonValue arguments;
    };

    /// One horizontal row of the tree viewport (pi `HorizontalViewportRow`).
    struct HorizontalViewportRow {
        std::string gutter;
        std::string body;
        std::size_t anchor_col{0};
        std::size_t body_width{0};
        bool is_selected{false};
    };

    /// The inline label editor state (pi `LabelInput`).
    struct LabelEditState {
        bool active{false};
        std::string entry_id;
    };

private:
    void flatten_tree();
    void build_active_path();
    void apply_filter();
    void recalculate_visual_structure();
    void update_selection_preserving(std::optional<std::string> preferred_id);
    void show_label_input(std::string entry_id, std::optional<std::string> current_label);
    void hide_label_input();
    void copy_selected();
    void update_node_label(std::string entry_id, std::optional<std::string> label);
    [[nodiscard]] std::optional<std::string> selected_entry_id() const;
    [[nodiscard]] std::size_t find_nearest_visible_index(std::optional<std::string> entry_id) const;
    [[nodiscard]] bool is_foldable(std::string_view entry_id) const;
    [[nodiscard]] std::size_t find_branch_segment_start(std::string_view direction) const;
    void cycle_filter_mode(int direction);
    [[nodiscard]] std::string status_labels() const;
    [[nodiscard]] std::string get_searchable_text(const harness::session::SessionTreeNode& node) const;
    [[nodiscard]] std::string get_entry_display_text(
        const harness::session::SessionTreeNode& node,
        bool is_selected) const;
    [[nodiscard]] std::optional<std::string> get_entry_copy_text(
        const harness::session::SessionTreeNode& node) const;
    [[nodiscard]] std::string extract_content(
        const harness::session::SessionTreeNode& node) const;
    [[nodiscard]] std::string format_tool_call(std::string_view name, const util::JsonValue& arguments) const;
    [[nodiscard]] std::string normalize(std::string text) const;

    const LiveTheme& theme_; // must outlive this component.
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings_;
    TreeSelectSink on_select_;
    TreeCancelSink on_cancel_;
    TreeLabelChangeSink on_label_change_;
    TreeCopySink on_copy_;
    TreeInvalidateSink on_invalidate_;

    std::vector<harness::session::SessionTreeNode> tree_;
    std::vector<FlatNode> flat_nodes_;
    std::vector<FlatNode> filtered_nodes_;
    std::size_t selected_index_{0};
    std::string current_leaf_id_;
    std::size_t max_visible_lines_{5};
    TreeFilterMode filter_mode_{TreeFilterMode::Default};
    std::string search_query_;
    /// tool_call_id -> tool call info from assistant content.
    std::unordered_map<std::string, ToolCallInfo> tool_call_map_;
    bool multiple_roots_{false};
    bool show_label_timestamps_{false};
    std::unordered_set<std::string> active_path_ids_;
    /// Visible-tree ancestor/descendant maps for navigation (pi
    /// `visibleParentMap` / `visibleChildrenMap`).
    std::unordered_map<std::string, std::optional<std::string>> visible_parent_map_;
    std::unordered_map<std::optional<std::string>, std::vector<std::string>> visible_children_map_;
    std::optional<std::string> last_selected_id_{std::nullopt};
    std::unordered_set<std::string> folded_nodes_;
    /// The label-edit Input (pi `LabelInput`); engaged only while
    /// `label_state_.active`.
    cch::tui::Input label_input_;
    LabelEditState label_state_;
    bool focused_{false};
};

} // namespace cch::coding_agent::tui
