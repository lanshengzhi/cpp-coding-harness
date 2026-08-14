#include "coding_agent/tui/TreeSelector.hpp"

#include "coding_agent/tui/KeybindingHints.hpp"
#include "util/BoundedText.hpp"
#include "util/Json.hpp"

#include <cch/tui/Utils.hpp>

#include <cch/util/Error.hpp>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <format>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

namespace cch::coding_agent::tui {
namespace {

constexpr std::size_t kTreeGutterWidth = 2;
constexpr std::size_t kMinVisibleAnchorContentWidth = 4;
constexpr std::size_t kMaxVisibleAnchorContentWidth = 20;
constexpr std::size_t kMinAnchorContextWidth = 2;
constexpr std::size_t kMaxAnchorContextWidth = 12;
/// pi's tree display budgets: 200 chars for entry content, 50 for the bash
/// command, 40 for tool-argument JSON (through util::bounded_text).
constexpr std::size_t kEntryContentDisplayBytes = 200;
constexpr std::size_t kBashCommandDisplayBytes = 50;
constexpr std::size_t kToolArgumentsDisplayBytes = 40;

/// Printable keys: plain and shift-modified characters only (the private
/// `tui::detail::is_printable` mirror; named keys and ctrl/alt-modified
/// events are handled by the keybinding actions or rejected).
[[nodiscard]] bool is_printable_key(const cch::tui::KeyEvent& event) {
    if (event.ctrl || event.alt || event.key.empty()) return false;
    return event.key != "enter" && event.key != "tab" && event.key != "escape" &&
        event.key != "backspace" && event.key != "delete" && event.key != "insert" &&
        event.key != "clear" && event.key != "home" && event.key != "end" &&
        event.key != "pageUp" && event.key != "pageDown" && event.key != "up" &&
        event.key != "down" && event.key != "left" && event.key != "right";
}

[[nodiscard]] std::string printable_key_text(const cch::tui::KeyEvent& event) {
    if (event.key == "space") return " ";
    if (event.shift && event.key.size() == 1) {
        const auto letter = static_cast<unsigned char>(event.key.front());
        if (letter >= 'a' && letter <= 'z') {
            return std::string(1, static_cast<char>(letter - 'a' + 'A'));
        }
    }
    return event.key;
}

/// The concatenated text across a message content block list (pi
/// `extractFullContent`).
[[nodiscard]] std::string extract_content_text(const ai::MessageVariant& message) {
    std::string result;
    if (const auto* user = std::get_if<ai::UserMessage>(&message)) {
        if (const auto* text = std::get_if<std::string>(&user->content)) {
            result = *text;
        } else {
            result = ai::text_from_content(std::get<std::vector<ai::Content>>(user->content));
        }
    } else if (const auto* assistant = std::get_if<ai::AssistantMessage>(&message)) {
        result = ai::text_from_assistant_content(assistant->content);
    } else if (const auto* tool = std::get_if<ai::ToolResultMessage>(&message)) {
        result = ai::text_from_content(tool->content);
    }
    return result;
}

/// Whether a message carries non-blank text content (pi `hasTextContent`).
[[nodiscard]] bool has_text_content(const ai::MessageVariant& message) {
    auto text = extract_content_text(message);
    const auto first = text.find_first_not_of(" \t\r\n");
    return first != std::string::npos;
}

/// `~`-shorten a path against HOME (pi formatToolCall `shortenPath`).
[[nodiscard]] std::string shorten_path(std::string path) {
    if (path.empty()) return path;
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
        const std::string home_prefix{home};
        if (path.starts_with(home_prefix) &&
            path.size() > home_prefix.size() &&
            path[home_prefix.size()] == '/') {
            return "~" + path.substr(home_prefix.size());
        }
    }
    return path;
}

/// One argument value from the tool-call arguments object (pi `args.path ||
/// args.file_path || ""`).
[[nodiscard]] std::string argument_string(
    const util::JsonValue& arguments,
    std::string_view path_key) {
    if (!arguments.holds<util::JsonValue::object_t>()) return {};
    for (const auto& [key, value] : arguments.get_object()) {
        if (key == path_key) {
            if (const auto* text = value.get_if<std::string>()) {
                return *text;
            }
        }
    }
    return {};
}

} // namespace

// ── TreeSelectorComponent implementation ─────────────────────────────────────

TreeSelectorComponent::TreeSelectorComponent(
    const LiveTheme& theme,
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
    std::vector<harness::session::SessionTreeNode> tree,
    std::string current_leaf_id,
    std::size_t terminal_height,
    TreeSelectSink on_select,
    TreeCancelSink on_cancel,
    TreeLabelChangeSink on_label_change,
    TreeCopySink on_copy,
    TreeInvalidateSink on_invalidate)
    : theme_(theme),
      keybindings_(std::move(keybindings)),
      on_select_(std::move(on_select)),
      on_cancel_(std::move(on_cancel)),
      on_label_change_(std::move(on_label_change)),
      on_copy_(std::move(on_copy)),
      on_invalidate_(std::move(on_invalidate)),
      tree_(std::move(tree)),
      current_leaf_id_(std::move(current_leaf_id)),
      max_visible_lines_(std::max<std::size_t>(5, terminal_height / 2)),
      multiple_roots_(tree_.size() > 1),
      label_input_(cch::tui::InputOptions{.keybindings = keybindings}) {
    flatten_tree();
    build_active_path();
    apply_filter();
    const auto initial = current_leaf_id_.empty()
        ? std::optional<std::string>{}
        : std::optional<std::string>{current_leaf_id_};
    selected_index_ = find_nearest_visible_index(initial);
    if (!filtered_nodes_.empty()) {
        last_selected_id_ = filtered_nodes_[selected_index_].node->entry.entry_id;
    }
}

/// pi `formatLabelTimestamp`: `HH:MM` for today, `M/D HH:MM` for this year,
/// `YY/M/D HH:MM` otherwise (local time).
std::string TreeSelectorComponent::format_label_timestamp(
    std::int64_t timestamp_ms,
    std::int64_t now_ms) {
    const auto date = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{timestamp_ms}};
    const auto now = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{now_ms}};
    const auto date_local = std::chrono::current_zone()->to_local(date);
    const auto now_local = std::chrono::current_zone()->to_local(now);
    const auto date_days = std::chrono::floor<std::chrono::days>(date_local);
    const auto now_days = std::chrono::floor<std::chrono::days>(now_local);
    const auto date_ymd = std::chrono::year_month_day{date_days};
    const auto now_ymd = std::chrono::year_month_day{now_days};
    const auto hours = std::chrono::floor<std::chrono::hours>(date_local - date_days);
    const auto minutes = std::chrono::floor<std::chrono::minutes>(date_local - date_days - hours);
    const auto two = [](int value) {
        return value < 10 ? std::format("0{}", value) : std::format("{}", value);
    };
    const std::string time =
        two(static_cast<int>(hours.count())) + ":" +
        two(static_cast<int>(minutes.count()));
    const auto year = static_cast<int>(date_ymd.year());
    const auto month = static_cast<unsigned>(date_ymd.month());
    const auto day = static_cast<unsigned>(date_ymd.day());
    if (date_ymd == now_ymd) {
        return time;
    }
    if (year == static_cast<int>(now_ymd.year())) {
        return std::format("{}/{} {}", month, day, time);
    }
    const auto year_two = year % 100;
    return std::format("{}/{}/{} {}", two(year_two), month, day, time);
}

void TreeSelectorComponent::flatten_tree() {
    flat_nodes_.clear();
    tool_call_map_.clear();

    // Determine which subtrees contain the active leaf (to sort the current
    // branch first), with an iterative post-order pass (pi flattenTree).
    // tree_ is an owned non-const member, so the walk uses non-const
    // pointers throughout (CODING_STANDARDS 9.6: no const_cast).
    std::map<harness::session::SessionTreeNode*, bool> contains_active;
    std::vector<harness::session::SessionTreeNode*> all_nodes;
    {
        std::vector<harness::session::SessionTreeNode*> pre_order;
        for (auto& root : tree_) pre_order.push_back(&root);
        while (!pre_order.empty()) {
            auto* node = pre_order.back();
            pre_order.pop_back();
            all_nodes.push_back(node);
            for (auto it = node->children.rbegin(); it != node->children.rend(); ++it) {
                pre_order.push_back(&*it);
            }
        }
        for (auto it = all_nodes.rbegin(); it != all_nodes.rend(); ++it) {
            auto* node = *it;
            bool has = !current_leaf_id_.empty() &&
                node->entry.entry_id == current_leaf_id_;
            for (auto& child : node->children) {
                if (contains_active[&child]) has = true;
            }
            contains_active[node] = has;
        }
    }

    // Add roots in reverse order, prioritizing the one containing the active
    // leaf. With multiple roots they become children of a virtual branching
    // root.
    std::vector<harness::session::SessionTreeNode*> ordered_roots;
    for (auto& root : tree_) ordered_roots.push_back(&root);
    std::stable_sort(
        ordered_roots.begin(),
        ordered_roots.end(),
        [&](harness::session::SessionTreeNode* first,
            harness::session::SessionTreeNode* second) {
            // The root containing the active leaf comes first (pi:
            // `Number(containsActive.get(b)) - Number(containsActive.get(a))`).
            return contains_active[first] && !contains_active[second];
        });

    struct StackItem {
        harness::session::SessionTreeNode* node{nullptr};
        std::size_t indent{0};
        bool just_branched{false};
        bool show_connector{false};
        bool is_last{false};
        std::vector<GutterInfo> gutters;
        bool is_virtual_root_child{false};
    };
    std::vector<StackItem> stack;
    for (std::size_t index = ordered_roots.size(); index > 0; --index) {
        auto* root = ordered_roots[index - 1];
        const bool is_last = index == ordered_roots.size();
        stack.push_back(StackItem{
            .node = root,
            .indent = multiple_roots_ ? std::size_t{1} : std::size_t{0},
            .just_branched = multiple_roots_,
            .show_connector = multiple_roots_,
            .is_last = is_last,
            .gutters = {},
            .is_virtual_root_child = multiple_roots_,
        });
    }

    while (!stack.empty()) {
        auto item = std::move(stack.back());
        stack.pop_back();
        auto* node = item.node;

        // Extract tool calls from assistant messages for later lookup.
        if (node->entry.kind == harness::session::SessionEntryKind::Message &&
            node->entry.message.has_value()) {
            if (const auto* assistant =
                    std::get_if<ai::AssistantMessage>(&*node->entry.message)) {
                for (const auto& block : assistant->content) {
                    if (const auto* call = std::get_if<ai::ToolCallContent>(&block)) {
                        tool_call_map_.emplace(
                            call->id,
                            ToolCallInfo{
                                .name = call->name,
                                .arguments = call->arguments.value_or(util::JsonValue{}),
                            });
                    }
                }
            }
        }

        FlatNode flat;
        flat.node = node;
        flat.indent = item.indent;
        flat.show_connector = item.show_connector;
        flat.is_last = item.is_last;
        flat.gutters = std::move(item.gutters);
        flat.is_virtual_root_child = item.is_virtual_root_child;
        flat_nodes_.push_back(std::move(flat));

        // Order children so the branch containing the active leaf comes first.
        std::vector<harness::session::SessionTreeNode*> ordered_children;
        for (auto& child : node->children) ordered_children.push_back(&child);
        std::stable_sort(
            ordered_children.begin(),
            ordered_children.end(),
            [&](harness::session::SessionTreeNode* first,
                harness::session::SessionTreeNode* second) {
                // The branch containing the active leaf comes first (pi:
                // `Number(containsActive.get(b)) - Number(containsActive.get(a))`).
                return contains_active[first] && !contains_active[second];
            });

        const bool multiple_children = ordered_children.size() > 1;
        std::size_t child_indent;
        if (multiple_children) {
            child_indent = item.indent + 1;
        } else if (item.just_branched && item.indent > 0) {
            child_indent = item.indent + 1;
        } else {
            child_indent = item.indent;
        }

        const bool connector_displayed = item.show_connector && !item.is_virtual_root_child;
        const std::size_t current_display_indent =
            multiple_roots_ ? std::max<std::size_t>(0, item.indent - 1) : item.indent;
        const std::size_t connector_position =
            std::max<std::size_t>(0, current_display_indent - 1);
        std::vector<GutterInfo> child_gutters = item.gutters;
        if (connector_displayed) {
            child_gutters.push_back(GutterInfo{
                .position = connector_position,
                .show = !item.is_last,
            });
        }

        for (std::size_t index = ordered_children.size(); index > 0; --index) {
            auto* child = ordered_children[index - 1];
            const bool child_is_last = index == ordered_children.size();
            stack.push_back(StackItem{
                .node = child,
                .indent = child_indent,
                .just_branched = multiple_children,
                .show_connector = multiple_children,
                .is_last = child_is_last,
                .gutters = child_gutters,
                .is_virtual_root_child = false,
            });
        }
    }
}

void TreeSelectorComponent::build_active_path() {
    active_path_ids_.clear();
    if (current_leaf_id_.empty()) return;
    std::map<std::string, const harness::session::SessionTreeNode*> by_id;
    for (auto& node : tree_) {
        std::vector<harness::session::SessionTreeNode*> stack{&node};
        while (!stack.empty()) {
            auto* current = stack.back();
            stack.pop_back();
            by_id.emplace(current->entry.entry_id, current);
            for (auto& child : current->children) stack.push_back(&child);
        }
    }
    std::optional<std::string> current = current_leaf_id_;
    while (current) {
        active_path_ids_.insert(*current);
        auto node = by_id.find(*current);
        if (node == by_id.end()) break;
        const auto& parent = node->second->entry.parent_id;
        current = parent && !parent->empty()
            ? std::optional<std::string>{*parent}
            : std::nullopt;
    }
}

void TreeSelectorComponent::apply_filter() {
    // Update lastSelectedId only when we have a valid selection (this
    // preserves the selection when switching through empty filter results).
    if (!filtered_nodes_.empty()) {
        const auto index = std::min(selected_index_, filtered_nodes_.size() - 1);
        last_selected_id_ = filtered_nodes_[index].node->entry.entry_id;
    }

    std::vector<std::string> search_tokens;
    {
        std::string query = search_query_;
        std::transform(
            query.begin(), query.end(), query.begin(),
            [](unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
        std::size_t position = 0;
        while (position < query.size()) {
            const auto first = query.find_first_not_of(" \t\r\n", position);
            if (first == std::string::npos) break;
            const auto last = query.find_first_of(" \t\r\n", first);
            search_tokens.push_back(
                last == std::string::npos
                    ? query.substr(first)
                    : query.substr(first, last - first));
            position = last == std::string::npos ? query.size() : last;
        }
    }

    std::vector<FlatNode> filtered;
    filtered.reserve(flat_nodes_.size());
    for (const auto& flat : flat_nodes_) {
        const auto& entry = flat.node->entry;
        const bool is_current_leaf = entry.entry_id == current_leaf_id_;

        // Skip assistant messages with only tool calls (no text) unless
        // error/aborted; always show the current leaf so the active position
        // stays visible.
        if (entry.kind == harness::session::SessionEntryKind::Message &&
            entry.message.has_value() && !is_current_leaf) {
            if (const auto* assistant =
                    std::get_if<ai::AssistantMessage>(&*entry.message)) {
                const bool has_text = has_text_content(*entry.message);
                const bool is_error_or_aborted =
                    assistant->stop_reason != ai::AssistantStopReason::Stop &&
                    assistant->stop_reason != ai::AssistantStopReason::ToolUse;
                if (!has_text && !is_error_or_aborted) {
                    continue;
                }
            }
        }

        // Entry types hidden in the default view (settings/bookkeeping).
        const bool is_settings_entry =
            entry.kind == harness::session::SessionEntryKind::Label ||
            entry.kind == harness::session::SessionEntryKind::Custom ||
            entry.kind == harness::session::SessionEntryKind::ModelChange ||
            entry.kind == harness::session::SessionEntryKind::ThinkingLevelChange ||
            entry.kind == harness::session::SessionEntryKind::SessionInfo;

        bool passes_filter = true;
        switch (filter_mode_) {
        case TreeFilterMode::UserOnly:
            passes_filter =
                entry.kind == harness::session::SessionEntryKind::Message &&
                entry.message.has_value() &&
                std::holds_alternative<ai::UserMessage>(*entry.message);
            break;
        case TreeFilterMode::NoTools:
            passes_filter =
                !is_settings_entry &&
                !(entry.kind == harness::session::SessionEntryKind::Message &&
                  entry.message.has_value() &&
                  std::holds_alternative<ai::ToolResultMessage>(*entry.message));
            break;
        case TreeFilterMode::LabeledOnly:
            passes_filter = flat.node->label.has_value();
            break;
        case TreeFilterMode::All:
            passes_filter = true;
            break;
        default:
            passes_filter = !is_settings_entry;
            break;
        }
        if (!passes_filter) continue;

        if (!search_tokens.empty()) {
            auto node_text = get_searchable_text(*flat.node);
            std::transform(
                node_text.begin(), node_text.end(), node_text.begin(),
                [](unsigned char value) {
                    return static_cast<char>(std::tolower(value));
                });
            bool all_match = true;
            for (const auto& token : search_tokens) {
                if (node_text.find(token) == std::string::npos) {
                    all_match = false;
                    break;
                }
            }
            if (!all_match) continue;
        }
        filtered.push_back(flat);
    }

    // Filter out descendants of folded nodes.
    if (!folded_nodes_.empty()) {
        std::unordered_set<std::string> skip_set;
        for (const auto& flat : flat_nodes_) {
            const auto& entry = flat.node->entry;
            if (entry.parent_id.has_value() &&
                (folded_nodes_.contains(*entry.parent_id) ||
                 skip_set.contains(*entry.parent_id))) {
                skip_set.insert(entry.entry_id);
            }
        }
        filtered.erase(
            std::remove_if(
                filtered.begin(), filtered.end(),
                [&](const FlatNode& flat) {
                    return skip_set.contains(flat.node->entry.entry_id);
                }),
            filtered.end());
    }

    filtered_nodes_ = std::move(filtered);
    recalculate_visual_structure();

    // Try to preserve the cursor on the same node, or find the nearest
    // visible ancestor.
    if (last_selected_id_) {
        selected_index_ = find_nearest_visible_index(last_selected_id_);
    } else if (selected_index_ >= filtered_nodes_.size() &&
               !filtered_nodes_.empty()) {
        selected_index_ = filtered_nodes_.size() - 1;
    } else if (filtered_nodes_.empty()) {
        selected_index_ = 0;
    }
    if (!filtered_nodes_.empty()) {
        last_selected_id_ = filtered_nodes_[selected_index_].node->entry.entry_id;
    }
}

void TreeSelectorComponent::recalculate_visual_structure() {
    visible_parent_map_.clear();
    visible_children_map_.clear();
    if (filtered_nodes_.empty()) {
        multiple_roots_ = false;
        return;
    }

    std::unordered_set<std::string> visible_ids;
    for (const auto& flat : filtered_nodes_) {
        visible_ids.insert(flat.node->entry.entry_id);
    }
    std::map<std::string, const harness::session::SessionTreeNode*> by_id;
    for (const auto& node : tree_) {
        std::vector<const harness::session::SessionTreeNode*> stack{&node};
        while (!stack.empty()) {
            const auto* current = stack.back();
            stack.pop_back();
            by_id.emplace(current->entry.entry_id, current);
            for (const auto& child : current->children) stack.push_back(&child);
        }
    }

    // Find the nearest visible ancestor for a node.
    const auto find_visible_ancestor =
        [&](const std::string& node_id) -> std::optional<std::string> {
        std::optional<std::string> current;
        const auto entry = by_id.find(node_id);
        if (entry != by_id.end()) {
            const auto& parent = entry->second->entry.parent_id;
            if (parent && !parent->empty()) current = *parent;
        }
        while (current) {
            if (visible_ids.contains(*current)) return current;
            const auto node = by_id.find(*current);
            if (node == by_id.end()) break;
            const auto& parent = node->second->entry.parent_id;
            current = parent && !parent->empty()
                ? std::optional<std::string>{*parent}
                : std::nullopt;
        }
        return std::nullopt;
    };

    // Build the visible tree: visibleParent (node → nearest visible ancestor
    // or null for roots) and visibleChildren (parent → children in filtered
    // order).
    visible_children_map_.emplace(std::nullopt, std::vector<std::string>{});
    for (const auto& flat : filtered_nodes_) {
        const auto& node_id = flat.node->entry.entry_id;
        const auto ancestor = find_visible_ancestor(node_id);
        visible_parent_map_.emplace(node_id, ancestor);
        auto& children = visible_children_map_[ancestor];
        children.push_back(node_id);
    }

    const auto visible_root_ids = visible_children_map_.at(std::nullopt);
    multiple_roots_ = visible_root_ids.size() > 1;

    std::map<std::string, std::size_t> filtered_index;
    for (std::size_t index = 0; index < filtered_nodes_.size(); ++index) {
        filtered_index.emplace(filtered_nodes_[index].node->entry.entry_id, index);
    }

    struct StackItem {
        std::string node_id;
        std::size_t indent{0};
        bool just_branched{false};
        bool show_connector{false};
        bool is_last{false};
        std::vector<GutterInfo> gutters;
        bool is_virtual_root_child{false};
    };
    std::vector<StackItem> stack;
    for (std::size_t index = visible_root_ids.size(); index > 0; --index) {
        const bool is_last = index == visible_root_ids.size();
        stack.push_back(StackItem{
            .node_id = visible_root_ids[index - 1],
            .indent = multiple_roots_ ? std::size_t{1} : std::size_t{0},
            .just_branched = multiple_roots_,
            .show_connector = multiple_roots_,
            .is_last = is_last,
            .gutters = {},
            .is_virtual_root_child = multiple_roots_,
        });
    }

    while (!stack.empty()) {
        auto item = std::move(stack.back());
        stack.pop_back();
        const auto found = filtered_index.find(item.node_id);
        if (found == filtered_index.end()) continue;
        auto& flat = filtered_nodes_[found->second];
        flat.indent = item.indent;
        flat.show_connector = item.show_connector;
        flat.is_last = item.is_last;
        flat.gutters = item.gutters;
        flat.is_virtual_root_child = item.is_virtual_root_child;

        const auto children_it = visible_children_map_.find(
            std::optional<std::string>{item.node_id});
        const auto& children =
            children_it == visible_children_map_.end()
            ? std::vector<std::string>{}
            : children_it->second;
        const bool multiple_children = children.size() > 1;

        std::size_t child_indent;
        if (multiple_children) {
            child_indent = item.indent + 1;
        } else if (item.just_branched && item.indent > 0) {
            child_indent = item.indent + 1;
        } else {
            child_indent = item.indent;
        }

        const bool connector_displayed = item.show_connector && !item.is_virtual_root_child;
        const std::size_t current_display_indent =
            multiple_roots_ ? std::max<std::size_t>(0, item.indent - 1) : item.indent;
        const std::size_t connector_position =
            std::max<std::size_t>(0, current_display_indent - 1);
        std::vector<GutterInfo> child_gutters = item.gutters;
        if (connector_displayed) {
            child_gutters.push_back(GutterInfo{
                .position = connector_position,
                .show = !item.is_last,
            });
        }

        for (std::size_t index = children.size(); index > 0; --index) {
            const bool child_is_last = index == children.size();
            stack.push_back(StackItem{
                .node_id = children[index - 1],
                .indent = child_indent,
                .just_branched = multiple_children,
                .show_connector = multiple_children,
                .is_last = child_is_last,
                .gutters = child_gutters,
                .is_virtual_root_child = false,
            });
        }
    }
}

std::size_t TreeSelectorComponent::find_nearest_visible_index(
    std::optional<std::string> entry_id) const {
    if (filtered_nodes_.empty()) return 0;
    if (!entry_id) return filtered_nodes_.size() - 1;

    std::map<std::string, const harness::session::SessionTreeNode*> by_id;
    for (const auto& node : tree_) {
        std::vector<const harness::session::SessionTreeNode*> stack{&node};
        while (!stack.empty()) {
            const auto* current = stack.back();
            stack.pop_back();
            by_id.emplace(current->entry.entry_id, current);
            for (const auto& child : current->children) stack.push_back(&child);
        }
    }
    std::map<std::string, std::size_t> visible_index;
    for (std::size_t index = 0; index < filtered_nodes_.size(); ++index) {
        visible_index.emplace(filtered_nodes_[index].node->entry.entry_id, index);
    }

    // Walk from entryId up to root, looking for a visible entry.
    std::optional<std::string> current = entry_id;
    while (current) {
        const auto visible = visible_index.find(*current);
        if (visible != visible_index.end()) return visible->second;
        const auto node = by_id.find(*current);
        if (node == by_id.end()) break;
        const auto& parent = node->second->entry.parent_id;
        current = parent && !parent->empty()
            ? std::optional<std::string>{*parent}
            : std::nullopt;
    }
    return filtered_nodes_.size() - 1;
}

bool TreeSelectorComponent::is_foldable(std::string_view entry_id) const {
    const auto children = visible_children_map_.find(
        std::optional<std::string>{std::string{entry_id}});
    if (children == visible_children_map_.end() || children->second.empty()) {
        return false;
    }
    const auto parent = visible_parent_map_.find(std::string{entry_id});
    if (parent == visible_parent_map_.end() || !parent->second.has_value()) {
        return true;
    }
    const auto siblings = visible_children_map_.find(parent->second);
    return siblings != visible_children_map_.end() && siblings->second.size() > 1;
}

std::size_t TreeSelectorComponent::find_branch_segment_start(
    std::string_view direction) const {
    const auto selected = selected_entry_id();
    if (!selected) return selected_index_;
    std::map<std::string, std::size_t> index_by_id;
    for (std::size_t index = 0; index < filtered_nodes_.size(); ++index) {
        index_by_id.emplace(filtered_nodes_[index].node->entry.entry_id, index);
    }
    std::optional<std::string> current = selected;
    if (direction == "down") {
        while (true) {
            const auto children = visible_children_map_.find(
                std::optional<std::string>{*current});
            const auto& child_list =
                children == visible_children_map_.end()
                ? std::vector<std::string>{}
                : children->second;
            if (child_list.empty()) return index_by_id.at(*current);
            if (child_list.size() > 1) return index_by_id.at(child_list.front());
            current = child_list.front();
        }
    }
    // direction === "up": walk the visible parent chain.
    while (true) {
        const auto parent = visible_parent_map_.find(*current);
        if (parent == visible_parent_map_.end() || !parent->second.has_value()) {
            return index_by_id.at(*current);
        }
        const auto siblings = visible_children_map_.find(parent->second);
        const auto& sibling_list =
            siblings == visible_children_map_.end()
            ? std::vector<std::string>{}
            : siblings->second;
        if (sibling_list.size() > 1) {
            const auto segment_start = index_by_id.find(*current);
            if (segment_start != index_by_id.end() &&
                segment_start->second < selected_index_) {
                return segment_start->second;
            }
        }
        current = parent->second;
    }
}

std::string TreeSelectorComponent::get_searchable_text(
    const harness::session::SessionTreeNode& node) const {
    const auto& entry = node.entry;
    std::vector<std::string> parts;
    if (node.label) parts.push_back(*node.label);

    switch (entry.kind) {
    case harness::session::SessionEntryKind::Message: {
        if (!entry.message) break;
        const ai::MessageVariant& message = *entry.message;
        if (std::holds_alternative<ai::UserMessage>(message)) {
            parts.push_back("user");
            parts.push_back(extract_content_text(message));
        } else if (std::holds_alternative<ai::AssistantMessage>(message)) {
            parts.push_back("assistant");
            parts.push_back(extract_content_text(message));
        } else if (std::holds_alternative<ai::ToolResultMessage>(message)) {
            parts.push_back("toolResult");
            parts.push_back(extract_content_text(message));
        } else if (const auto* bash =
                       std::get_if<ai::BashExecutionMessage>(&message)) {
            parts.push_back("bashExecution");
            if (!bash->command.empty()) parts.push_back(bash->command);
        }
        break;
    }
    case harness::session::SessionEntryKind::CustomMessage: {
        if (const auto* value =
                std::get_if<harness::session::CustomMessageEntryValue>(
                    &entry.value)) {
            parts.push_back(value->custom_type);
            if (const auto* text = std::get_if<std::string>(&value->content)) {
                parts.push_back(*text);
            } else {
                for (const auto& block :
                     std::get<std::vector<
                         harness::session::CustomMessageEntryContentBlock>>(
                         value->content)) {
                    if (const auto* text = std::get_if<ai::TextContent>(&block)) {
                        parts.push_back(text->text);
                    }
                }
            }
        }
        break;
    }
    case harness::session::SessionEntryKind::Compaction:
        parts.push_back("compaction");
        break;
    case harness::session::SessionEntryKind::BranchSummary:
        parts.push_back("branch summary");
        if (const auto* value =
                std::get_if<harness::session::BranchSummaryEntryValue>(
                    &entry.value)) {
            parts.push_back(value->summary);
        }
        break;
    case harness::session::SessionEntryKind::SessionInfo:
        parts.push_back("title");
        if (const auto* value =
                std::get_if<harness::session::SessionInfoEntryValue>(
                    &entry.value)) {
            if (value->name && !value->name->empty()) parts.push_back(*value->name);
        }
        break;
    case harness::session::SessionEntryKind::ModelChange:
        parts.push_back("model");
        if (const auto* value =
                std::get_if<harness::session::ModelChangeValue>(&entry.value)) {
            parts.push_back(value->model_id);
        }
        break;
    case harness::session::SessionEntryKind::ThinkingLevelChange:
        parts.push_back("thinking");
        if (const auto* value =
                std::get_if<harness::session::ThinkingLevelChangeValue>(
                    &entry.value)) {
            parts.push_back(value->thinking_level);
        }
        break;
    case harness::session::SessionEntryKind::Custom:
        parts.push_back("custom");
        if (const auto* value =
                std::get_if<harness::session::CustomEntryValue>(&entry.value)) {
            parts.push_back(value->custom_type);
        }
        break;
    case harness::session::SessionEntryKind::Label:
        parts.push_back("label");
        if (const auto* value =
                std::get_if<harness::session::LabelEntryValue>(&entry.value)) {
            parts.push_back(value->label.value_or(""));
        }
        break;
    default:
        break;
    }

    std::string result;
    for (const auto& part : parts) {
        if (!result.empty()) result.push_back(' ');
        result += part;
    }
    return result;
}

std::optional<std::string> TreeSelectorComponent::selected_entry_id() const {
    if (filtered_nodes_.empty()) return std::nullopt;
    return filtered_nodes_[selected_index_].node->entry.entry_id;
}

void TreeSelectorComponent::copy_selected() {
    if (on_copy_) {
        std::optional<std::string> text;
        if (!filtered_nodes_.empty()) {
            text = get_entry_copy_text(*filtered_nodes_[selected_index_].node);
        }
        on_copy_(std::move(text));
    }
}

void TreeSelectorComponent::update_node_label(
    std::string entry_id,
    std::optional<std::string> label) {
    std::vector<harness::session::SessionTreeNode*> stack;
    for (auto& root : tree_) stack.push_back(&root);
    while (!stack.empty()) {
        auto* node = stack.back();
        stack.pop_back();
        if (node->entry.entry_id == entry_id) {
            node->label = label;
            if (label) {
                const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch());
                node->label_timestamp = now.count();
            } else {
                node->label_timestamp = std::nullopt;
            }
            break;
        }
        for (auto& child : node->children) stack.push_back(&child);
    }
    apply_filter();
}

std::string TreeSelectorComponent::normalize(std::string text) const {
    std::replace_if(
        text.begin(), text.end(),
        [](unsigned char value) { return value == '\n' || value == '\t'; },
        ' ');
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string TreeSelectorComponent::extract_content(
    const harness::session::SessionTreeNode& node) const {
    std::string text;
    if (node.entry.message) {
        text = extract_content_text(*node.entry.message);
    }
    // pi's 200-char display budget for entry content, through the bounded
    // helper (CODING_STANDARDS 10.3 — no ad-hoc substr truncation).
    return util::bounded_text(text, kEntryContentDisplayBytes);
}

std::string TreeSelectorComponent::format_tool_call(
    std::string_view name,
    const util::JsonValue& arguments) const {
    const auto path_argument = [&](std::string_view key) {
        return argument_string(arguments, key);
    };
    if (name == "read") {
        auto path = shorten_path(path_argument("path"));
        if (path.empty()) path = shorten_path(path_argument("file_path"));
        std::string display = path;
        std::optional<double> offset;
        std::optional<double> limit;
        if (arguments.holds<util::JsonValue::object_t>()) {
            for (const auto& [key, value] : arguments.get_object()) {
                if (key == "offset" && value.holds<double>()) {
                    offset = value.get<double>();
                } else if (key == "limit" && value.holds<double>()) {
                    limit = value.get<double>();
                }
            }
        }
        if (offset || limit) {
            const auto start = static_cast<std::int64_t>(offset.value_or(1.0));
            display += ":" + std::to_string(start);
            if (limit) {
                display += "-" +
                    std::to_string(start + static_cast<std::int64_t>(*limit) - 1);
            }
        }
        return std::format("[read: {}]", display);
    }
    if (name == "write") {
        auto path = shorten_path(path_argument("path"));
        if (path.empty()) path = shorten_path(path_argument("file_path"));
        return std::format("[write: {}]", path);
    }
    if (name == "edit") {
        auto path = shorten_path(path_argument("path"));
        if (path.empty()) path = shorten_path(path_argument("file_path"));
        return std::format("[edit: {}]", path);
    }
    if (name == "bash") {
        auto raw = argument_string(arguments, "command");
        std::replace_if(
            raw.begin(), raw.end(),
            [](unsigned char value) { return value == '\n' || value == '\t'; },
            ' ');
        const auto first = raw.find_first_not_of(" \t\r\n");
        if (first != std::string::npos) {
            const auto last = raw.find_last_not_of(" \t\r\n");
            raw = raw.substr(first, last - first + 1);
        } else {
            raw.clear();
        }
        // pi's 50-char command budget with the truncation suffix (10.3).
        return std::format(
            "[bash: {}]", util::bounded_text(raw, kBashCommandDisplayBytes, "..."));
    }
    if (name == "grep") {
        const auto pattern = argument_string(arguments, "pattern");
        auto path = shorten_path(path_argument("path"));
        if (path.empty()) path = ".";
        return std::format("[grep: /{}/ in {}]", pattern, path);
    }
    if (name == "find") {
        const auto pattern = argument_string(arguments, "pattern");
        auto path = shorten_path(path_argument("path"));
        if (path.empty()) path = ".";
        return std::format("[find: {} in {}]", pattern, path);
    }
    if (name == "ls") {
        auto path = shorten_path(path_argument("path"));
        if (path.empty()) path = ".";
        return std::format("[ls: {}]", path);
    }
    // Custom tool: name + truncated JSON args (pi's 40-char budget).
    std::string args;
    if (auto serialized = util::write_json(arguments); serialized) {
        args = std::move(*serialized);
    }
    return std::format(
        "[{}: {}]",
        name,
        util::bounded_text(args, kToolArgumentsDisplayBytes, "..."));
}

std::string TreeSelectorComponent::get_entry_display_text(
    const harness::session::SessionTreeNode& node,
    bool is_selected) const {
    const auto& entry = node.entry;
    std::string result;
    const auto style = [&](std::string text) {
        return is_selected
            ? std::format("\x1b[1m{}\x1b[22m", text)
            : text;
    };

    switch (entry.kind) {
    case harness::session::SessionEntryKind::Message: {
        if (!entry.message) break;
        const ai::MessageVariant& message = *entry.message;
        if (std::holds_alternative<ai::UserMessage>(message)) {
            const auto content = normalize(extract_content(node));
            result = theme_.foreground(ThemeToken::Accent, "user: ") + content;
        } else if (const auto* assistant =
                       std::get_if<ai::AssistantMessage>(&message)) {
            const auto content = normalize(extract_content(node));
            if (!content.empty()) {
                result = theme_.foreground(ThemeToken::Success, "assistant: ") + content;
            } else if (assistant->stop_reason == ai::AssistantStopReason::Aborted) {
                result = theme_.foreground(ThemeToken::Success, "assistant: ") +
                    theme_.foreground(ThemeToken::Muted, "(aborted)");
            } else if (assistant->error_message) {
                auto err = normalize(*assistant->error_message);
                if (err.size() > 80) err.resize(80);
                result = theme_.foreground(ThemeToken::Success, "assistant: ") +
                    theme_.foreground(ThemeToken::Error, err);
            } else {
                result = theme_.foreground(ThemeToken::Success, "assistant: ") +
                    theme_.foreground(ThemeToken::Muted, "(no content)");
            }
        } else if (const auto* tool =
                       std::get_if<ai::ToolResultMessage>(&message)) {
            const auto tool_call = tool_call_map_.find(tool->tool_call_id);
            if (tool_call != tool_call_map_.end()) {
                result = theme_.foreground(
                    ThemeToken::Muted,
                    format_tool_call(tool_call->second.name, tool_call->second.arguments));
            } else {
                result = theme_.foreground(
                    ThemeToken::Muted,
                    std::format("[{}]", tool->tool_name));
            }
        } else if (const auto* bash =
                       std::get_if<ai::BashExecutionMessage>(&message)) {
            result = theme_.foreground(
                ThemeToken::Dim,
                std::format("[bash]: {}", normalize(bash->command)));
        }
        break;
    }
    case harness::session::SessionEntryKind::CustomMessage: {
        if (const auto* value =
                std::get_if<harness::session::CustomMessageEntryValue>(
                    &entry.value)) {
            std::string content;
            if (const auto* text = std::get_if<std::string>(&value->content)) {
                content = *text;
            } else {
                for (const auto& block :
                     std::get<std::vector<
                         harness::session::CustomMessageEntryContentBlock>>(
                         value->content)) {
                    if (const auto* text = std::get_if<ai::TextContent>(&block)) {
                        content += text->text;
                    }
                }
            }
            result = theme_.foreground(
                         ThemeToken::CustomMessageLabel,
                         std::format("[{}]: ", value->custom_type)) +
                normalize(std::move(content));
        }
        break;
    }
    case harness::session::SessionEntryKind::Compaction: {
        std::size_t tokens_before = 0;
        if (const auto* value =
                std::get_if<harness::session::CompactionEntryValue>(
                    &entry.value)) {
            tokens_before = value->tokens_before;
        }
        const auto tokens = static_cast<std::int64_t>(
            (tokens_before + 500) / 1000);
        result = theme_.foreground(
            ThemeToken::BorderAccent,
            std::format("[compaction: {}k tokens]", tokens));
        break;
    }
    case harness::session::SessionEntryKind::BranchSummary:
        result = theme_.foreground(ThemeToken::Warning, "[branch summary]: ");
        if (const auto* value =
                std::get_if<harness::session::BranchSummaryEntryValue>(
                    &entry.value)) {
            result += normalize(value->summary);
        }
        break;
    case harness::session::SessionEntryKind::ModelChange:
        if (const auto* value =
                std::get_if<harness::session::ModelChangeValue>(&entry.value)) {
            result = theme_.foreground(
                ThemeToken::Dim,
                std::format("[model: {}]", value->model_id));
        }
        break;
    case harness::session::SessionEntryKind::ThinkingLevelChange:
        if (const auto* value =
                std::get_if<harness::session::ThinkingLevelChangeValue>(
                    &entry.value)) {
            result = theme_.foreground(
                ThemeToken::Dim,
                std::format("[thinking: {}]", value->thinking_level));
        }
        break;
    case harness::session::SessionEntryKind::Custom:
        if (const auto* value =
                std::get_if<harness::session::CustomEntryValue>(&entry.value)) {
            result = theme_.foreground(
                ThemeToken::Dim,
                std::format("[custom: {}]", value->custom_type));
        }
        break;
    case harness::session::SessionEntryKind::Label:
        if (const auto* value =
                std::get_if<harness::session::LabelEntryValue>(&entry.value)) {
            result = theme_.foreground(
                ThemeToken::Dim,
                std::format("[label: {}]", value->label.value_or("(cleared)")));
        }
        break;
    case harness::session::SessionEntryKind::SessionInfo:
        if (const auto* value =
                std::get_if<harness::session::SessionInfoEntryValue>(
                    &entry.value)) {
            if (value->name && !value->name->empty()) {
                result = theme_.foreground(
                    ThemeToken::Dim, std::format("[title: {}]", *value->name));
            } else {
                result = theme_.foreground(ThemeToken::Dim, "[title: ") +
                    std::format("\x1b[3m{}\x1b[23m",
                                theme_.foreground(ThemeToken::Dim, "empty")) +
                    theme_.foreground(ThemeToken::Dim, "]");
            }
        }
        break;
    default:
        break;
    }
    return style(std::move(result));
}

std::optional<std::string> TreeSelectorComponent::get_entry_copy_text(
    const harness::session::SessionTreeNode& node) const {
    const auto& entry = node.entry;
    std::optional<std::string> text;
    switch (entry.kind) {
    case harness::session::SessionEntryKind::Message: {
        if (!entry.message) break;
        const ai::MessageVariant& message = *entry.message;
        if (const auto* bash = std::get_if<ai::BashExecutionMessage>(&message)) {
            text = bash->command;
        } else if (std::holds_alternative<ai::UserMessage>(message) ||
                   std::holds_alternative<ai::AssistantMessage>(message) ||
                   std::holds_alternative<ai::ToolResultMessage>(message)) {
            text = extract_content_text(message);
            if (text->empty()) {
                if (const auto* assistant =
                        std::get_if<ai::AssistantMessage>(&message)) {
                    if (assistant->error_message) text = *assistant->error_message;
                }
            }
        }
        break;
    }
    case harness::session::SessionEntryKind::CustomMessage: {
        if (const auto* value =
                std::get_if<harness::session::CustomMessageEntryValue>(
                    &entry.value)) {
            std::string content;
            if (const auto* text = std::get_if<std::string>(&value->content)) {
                content = *text;
            } else {
                for (const auto& block :
                     std::get<std::vector<
                         harness::session::CustomMessageEntryContentBlock>>(
                         value->content)) {
                    if (const auto* text = std::get_if<ai::TextContent>(&block)) {
                        content += text->text;
                    }
                }
            }
            text = std::move(content);
        }
        break;
    }
    case harness::session::SessionEntryKind::Compaction:
        if (const auto* value =
                std::get_if<harness::session::CompactionEntryValue>(
                    &entry.value)) {
            text = value->summary;
        }
        break;
    case harness::session::SessionEntryKind::BranchSummary:
        if (const auto* value =
                std::get_if<harness::session::BranchSummaryEntryValue>(
                    &entry.value)) {
            text = value->summary;
        }
        break;
    default:
        break;
    }
    if (text) {
        const auto first = text->find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::nullopt;
        const auto last = text->find_last_not_of(" \t\r\n");
        text = text->substr(first, last - first + 1);
    }
    return text;
}

void TreeSelectorComponent::cycle_filter_mode(int direction) {
    // pi's cycle order: default -> no-tools -> user-only -> labeled-only ->
    // all -> default.
    static constexpr TreeFilterMode kModes[] = {
        TreeFilterMode::Default,
        TreeFilterMode::NoTools,
        TreeFilterMode::UserOnly,
        TreeFilterMode::LabeledOnly,
        TreeFilterMode::All,
    };
    std::ptrdiff_t current_index = -1;
    for (std::size_t index = 0; index < std::size(kModes); ++index) {
        if (kModes[index] == filter_mode_) {
            current_index = static_cast<std::ptrdiff_t>(index);
            break;
        }
    }
    const auto size = static_cast<std::ptrdiff_t>(std::size(kModes));
    filter_mode_ = kModes[(current_index + direction + size) % size];
    folded_nodes_.clear();
    apply_filter();
}

std::string TreeSelectorComponent::status_labels() const {
    std::string labels;
    switch (filter_mode_) {
    case TreeFilterMode::NoTools:
        labels += " [no-tools]";
        break;
    case TreeFilterMode::UserOnly:
        labels += " [user]";
        break;
    case TreeFilterMode::LabeledOnly:
        labels += " [labeled]";
        break;
    case TreeFilterMode::All:
        labels += " [all]";
        break;
    default:
        break;
    }
    if (show_label_timestamps_) {
        labels += " [+label time]";
    }
    return labels;
}

void TreeSelectorComponent::show_label_input(
    std::string entry_id,
    std::optional<std::string> current_label) {
    label_state_.active = true;
    label_state_.entry_id = std::move(entry_id);
    label_input_.set_value(current_label.value_or(""));
    label_input_.set_focused(focused_);
    if (on_invalidate_) on_invalidate_();
}

void TreeSelectorComponent::hide_label_input() {
    label_state_.active = false;
    label_state_.entry_id.clear();
    if (on_invalidate_) on_invalidate_();
}

void TreeSelectorComponent::handle_input(const cch::tui::InputEventVariant& input) {
    if (label_state_.active) {
        const auto* key = std::get_if<cch::tui::KeyEvent>(&input);
        if (key != nullptr && key->type != cch::tui::KeyEventType::Release) {
            if (keybindings_->matches(*key, "tui.select.confirm")) {
                auto value = label_input_.value();
                const auto first = value.find_first_not_of(" \t\r\n");
                std::optional<std::string> label;
                if (first != std::string::npos) {
                    const auto last = value.find_last_not_of(" \t\r\n");
                    label = value.substr(first, last - first + 1);
                }
                const auto entry_id = label_state_.entry_id;
                update_node_label(entry_id, label);
                hide_label_input();
                if (on_label_change_) {
                    on_label_change_(entry_id, std::move(label));
                }
                return;
            }
            if (keybindings_->matches(*key, "tui.select.cancel")) {
                hide_label_input();
                return;
            }
        }
        label_input_.handle_input(input);
        if (on_invalidate_) on_invalidate_();
        return;
    }

    const auto* key = std::get_if<cch::tui::KeyEvent>(&input);
    if (key == nullptr || key->type == cch::tui::KeyEventType::Release) {
        return;
    }
    const auto matches = [&](std::string_view action_id) {
        return keybindings_->matches(*key, action_id);
    };

    if (matches("tui.select.up")) {
        if (!filtered_nodes_.empty()) {
            selected_index_ =
                selected_index_ == 0 ? filtered_nodes_.size() - 1 : selected_index_ - 1;
        }
    } else if (matches("tui.select.down")) {
        if (!filtered_nodes_.empty()) {
            selected_index_ =
                selected_index_ == filtered_nodes_.size() - 1 ? 0 : selected_index_ + 1;
        }
    } else if (matches("app.tree.foldOrUp")) {
        const auto current = selected_entry_id();
        if (current && is_foldable(*current) && !folded_nodes_.contains(*current)) {
            folded_nodes_.insert(*current);
            apply_filter();
        } else {
            selected_index_ = find_branch_segment_start("up");
        }
    } else if (matches("app.tree.unfoldOrDown")) {
        const auto current = selected_entry_id();
        if (current && folded_nodes_.contains(*current)) {
            folded_nodes_.erase(*current);
            apply_filter();
        } else {
            selected_index_ = find_branch_segment_start("down");
        }
    } else if (matches("tui.editor.cursorLeft") || matches("tui.select.pageUp")) {
        selected_index_ = selected_index_ > max_visible_lines_
            ? selected_index_ - max_visible_lines_
            : 0;
    } else if (matches("tui.editor.cursorRight") || matches("tui.select.pageDown")) {
        if (!filtered_nodes_.empty()) {
            selected_index_ = std::min(
                filtered_nodes_.size() - 1,
                selected_index_ + max_visible_lines_);
        }
    } else if (matches("tui.select.confirm")) {
        const auto selected = selected_entry_id();
        if (selected && on_select_) {
            on_select_(*selected);
        }
        return;
    } else if (matches("app.message.copy")) {
        copy_selected();
        return;
    } else if (matches("tui.select.cancel")) {
        if (!search_query_.empty()) {
            search_query_.clear();
            folded_nodes_.clear();
            apply_filter();
        } else if (on_cancel_) {
            on_cancel_();
        }
        return;
    } else if (matches("app.tree.filter.default")) {
        filter_mode_ = TreeFilterMode::Default;
        folded_nodes_.clear();
        apply_filter();
    } else if (matches("app.tree.filter.noTools")) {
        filter_mode_ = filter_mode_ == TreeFilterMode::NoTools
            ? TreeFilterMode::Default
            : TreeFilterMode::NoTools;
        folded_nodes_.clear();
        apply_filter();
    } else if (matches("app.tree.filter.userOnly")) {
        filter_mode_ = filter_mode_ == TreeFilterMode::UserOnly
            ? TreeFilterMode::Default
            : TreeFilterMode::UserOnly;
        folded_nodes_.clear();
        apply_filter();
    } else if (matches("app.tree.filter.labeledOnly")) {
        filter_mode_ = filter_mode_ == TreeFilterMode::LabeledOnly
            ? TreeFilterMode::Default
            : TreeFilterMode::LabeledOnly;
        folded_nodes_.clear();
        apply_filter();
    } else if (matches("app.tree.filter.all")) {
        filter_mode_ = filter_mode_ == TreeFilterMode::All
            ? TreeFilterMode::Default
            : TreeFilterMode::All;
        folded_nodes_.clear();
        apply_filter();
    } else if (matches("app.tree.filter.cycleBackward")) {
        cycle_filter_mode(-1);
    } else if (matches("app.tree.filter.cycleForward")) {
        cycle_filter_mode(1);
    } else if (matches("tui.editor.deleteCharBackward")) {
        if (!search_query_.empty()) {
            search_query_.pop_back();
            folded_nodes_.clear();
            apply_filter();
        }
    } else if (matches("app.tree.editLabel")) {
        const auto selected = selected_entry_id();
        if (selected) {
            std::optional<std::string> current_label;
            if (!filtered_nodes_.empty()) {
                current_label = filtered_nodes_[selected_index_].node->label;
            }
            show_label_input(*selected, current_label);
        }
    } else if (matches("app.tree.toggleLabelTimestamp")) {
        show_label_timestamps_ = !show_label_timestamps_;
    } else if (is_printable_key(*key)) {
        search_query_ += printable_key_text(*key);
        folded_nodes_.clear();
        apply_filter();
    }
    if (on_invalidate_) on_invalidate_();
}

void TreeSelectorComponent::set_focused(bool focused) {
    focused_ = focused;
    if (label_state_.active) {
        label_input_.set_focused(focused);
    }
}

bool TreeSelectorComponent::focused() const {
    return focused_;
}

std::optional<cch::tui::CursorPosition> TreeSelectorComponent::cursor_location() const {
    if (label_state_.active) {
        return label_input_.cursor_location();
    }
    return std::nullopt;
}

// ── Rendering ────────────────────────────────────────────────────────────────

namespace {

/// Render tree rows into a horizontally clipped viewport: the gutter stays
/// fixed, and the row bodies shift left only when the selected row's anchor
/// (the start of its entry text after tree indentation/markers) would
/// otherwise be too far right to see useful content (pi
/// `renderHorizontalViewport`).
[[nodiscard]] util::Expected<std::vector<std::string>> render_horizontal_viewport(
    const std::vector<TreeSelectorComponent::HorizontalViewportRow>& rows,
    std::size_t width) {
    const std::size_t viewport_width =
        width > kTreeGutterWidth ? width - kTreeGutterWidth : 0;
    std::size_t max_body_width = 0;
    for (const auto& row : rows) {
        max_body_width = std::max(max_body_width, row.body_width);
    }
    const std::size_t max_horizontal_scroll =
        max_body_width > viewport_width ? max_body_width - viewport_width : 0;

    const auto* selected = [&]() -> const TreeSelectorComponent::HorizontalViewportRow* {
        for (const auto& row : rows) {
            if (row.is_selected) return &row;
        }
        return nullptr;
    }();

    // Only pan horizontally when needed to keep enough selected-row content
    // visible after its anchor.
    std::size_t horizontal_scroll = 0;
    if (selected != nullptr && max_horizontal_scroll > 0) {
        const std::size_t min_visible_anchor_content_width = std::min(
            kMaxVisibleAnchorContentWidth,
            std::max(kMinVisibleAnchorContentWidth, viewport_width / 3));
        if (selected->anchor_col >
            viewport_width - min_visible_anchor_content_width) {
            const std::size_t anchor_context_width = std::min(
                kMaxAnchorContextWidth,
                std::max(kMinAnchorContextWidth, viewport_width / 4));
            horizontal_scroll = std::min(
                max_horizontal_scroll,
                selected->anchor_col - anchor_context_width);
        }
    }

    std::vector<std::string> lines;
    lines.reserve(rows.size());
    for (const auto& row : rows) {
        std::string line;
        if (horizontal_scroll > 0) {
            auto sliced = cch::tui::slice_by_column(
                row.body, horizontal_scroll, viewport_width, true);
            if (!sliced) return std::unexpected(sliced.error());
            line = row.gutter + *sliced + "\x1b[0m";
        } else {
            line = row.gutter + row.body;
        }
        auto truncated = cch::tui::truncate_text(line, width, "");
        if (!truncated) return std::unexpected(truncated.error());
        lines.push_back(std::move(*truncated));
    }
    return lines;
}

/// The tree help semantic rows with chunk-aware wrapping (pi `TreeHelp`).
[[nodiscard]] util::Expected<std::vector<std::string>> render_tree_help(
    std::size_t width,
    const cch::tui::KeybindingRegistry& keybindings,
    const LiveTheme& theme) {
    const auto first_key = [&](std::string_view action) -> std::optional<std::string> {
        const auto keys = keybindings.keys(action);
        if (keys.empty()) return std::nullopt;
        return keys.front();
    };
    // pi `compactRawKeys`: when every alternative shares one prefix, render
    // `prefix+suffix1/suffix2`; otherwise slash-join the alternatives.
    const auto compact_raw_keys =
        [](const std::vector<std::string>& keys) -> std::string {
        if (keys.size() == 1) return keys.front();
        std::vector<std::pair<std::string, std::string>> parts;
        parts.reserve(keys.size());
        std::string prefix;
        bool first = true;
        bool shared = true;
        for (const auto& key : keys) {
            const auto separator = key.rfind('+');
            std::string part_prefix;
            std::string suffix = key;
            if (separator != std::string::npos) {
                part_prefix = key.substr(0, separator + 1);
                suffix = key.substr(separator + 1);
            }
            if (first) {
                prefix = part_prefix;
                first = false;
            } else if (part_prefix != prefix) {
                shared = false;
            }
            parts.push_back({std::move(part_prefix), std::move(suffix)});
        }
        if (shared && !prefix.empty()) {
            std::string result = prefix;
            bool first_part = true;
            for (const auto& [ignored, suffix] : parts) {
                (void)ignored;
                if (!first_part) result.push_back('/');
                first_part = false;
                result += suffix;
            }
            return result;
        }
        std::string result;
        for (const auto& key : keys) {
            if (!result.empty()) result.push_back('/');
            result += key;
        }
        return result;
    };
    // pi `formatHelpKeys`: the first key of each binding, compacted, then
    // arrow/pgup replacements.
    const auto format_help_keys =
        [&](const std::vector<std::string_view>& actions) -> std::string {
        std::vector<std::string> keys;
        for (const auto action : actions) {
            if (auto key = first_key(action)) keys.push_back(std::move(*key));
        }
        if (keys.empty()) return {};
        auto text = compact_raw_keys(keys);
        const auto replace = [&](std::string_view from, std::string_view to) {
            std::string result;
            std::size_t position = 0;
            while (true) {
                const auto found = text.find(from, position);
                if (found == std::string::npos) {
                    result += text.substr(position);
                    break;
                }
                result += text.substr(position, found - position);
                result += to;
                position = found + from.size();
            }
            text = std::move(result);
        };
        replace("pageUp", "pgup");
        replace("pageDown", "pgdn");
        replace("up", "↑");
        replace("down", "↓");
        replace("left", "←");
        replace("right", "→");
        return text;
    };

    struct HelpItem {
        std::vector<std::string_view> actions;
        std::string label;
        bool label_first{false};
    };
    const std::vector<HelpItem> items{
        {{"tui.select.up", "tui.select.down"}, "move", false},
        {{"tui.editor.cursorLeft", "tui.editor.cursorRight"}, "page", false},
        {{"app.tree.foldOrUp", "app.tree.unfoldOrDown"}, "branch", false},
        {{"app.message.copy"}, "copy", false},
        {{"app.tree.editLabel"}, "label", false},
        {{"app.tree.toggleLabelTimestamp"}, "label time", false},
        {{"app.tree.filter.default",
          "app.tree.filter.noTools",
          "app.tree.filter.userOnly",
          "app.tree.filter.labeledOnly",
          "app.tree.filter.all"},
         "filters",
         true},
        {{"app.tree.filter.cycleForward", "app.tree.filter.cycleBackward"},
         "cycle",
         true},
    };

    std::vector<std::string> rendered_items;
    rendered_items.reserve(items.size());
    for (const auto& item : items) {
        const auto key_text = format_help_keys(item.actions);
        if (key_text.empty()) {
            rendered_items.push_back(item.label);
        } else {
            rendered_items.push_back(
                item.label_first
                    ? item.label + " " + key_text
                    : key_text + " " + item.label);
        }
    }

    const std::string separator = " · ";
    const std::string indent = "  ";
    const auto available_width = std::max<std::size_t>(1, width);
    std::vector<std::string> lines;
    std::string current_line;
    for (const auto& item : rendered_items) {
        std::string candidate;
        if (!current_line.empty()) {
            candidate = current_line + separator + item;
        } else {
            candidate = cch::tui::visible_width(indent + item) <= available_width
                ? indent + item
                : item;
        }
        if (!current_line.empty() || cch::tui::visible_width(candidate) <= available_width) {
            current_line = candidate;
            continue;
        }
        auto wrapped = cch::tui::wrap_text(current_line, available_width);
        if (!wrapped) return std::unexpected(wrapped.error());
        for (const auto& line : *wrapped) lines.push_back(theme.foreground(ThemeToken::Muted, line));
        current_line = cch::tui::visible_width(indent + item) <= available_width
            ? indent + item
            : item;
    }
    if (!current_line.empty()) {
        auto wrapped = cch::tui::wrap_text(current_line, available_width);
        if (!wrapped) return std::unexpected(wrapped.error());
        for (const auto& line : *wrapped) lines.push_back(theme.foreground(ThemeToken::Muted, line));
    }
    return lines;
}

} // namespace

util::Expected<cch::tui::RenderResult> TreeSelectorComponent::render(
    std::size_t width) {
    std::vector<std::string> lines;
    const auto border = [&]() {
        std::string rule;
        rule.reserve(width * 3);
        const auto count = width > 0 ? width : std::size_t{1};
        for (std::size_t index = 0; index < count; ++index) rule += "─";
        return theme_.foreground(ThemeToken::Border, rule);
    };

    lines.push_back("");
    lines.push_back(border());
    lines.push_back(std::format("\x1b[1m{}\x1b[22m", "  Session Tree"));
    auto help = render_tree_help(width, *keybindings_, theme_);
    if (!help) return std::unexpected(help.error());
    for (auto& line : *help) lines.push_back(std::move(line));
    const auto search_line = [&]() {
        if (search_query_.empty()) {
            return theme_.foreground(ThemeToken::Muted, "  Type to search:");
        }
        return theme_.foreground(ThemeToken::Muted, "  Type to search: ") +
            theme_.foreground(ThemeToken::Accent, search_query_);
    }();
    lines.push_back(search_line);
    lines.push_back(border());
    lines.push_back("");

    if (label_state_.active) {
        // pi LabelInput: "Label (empty to remove):" + the input + hints.
        const std::string indent = "  ";
        auto truncated_prompt = cch::tui::truncate_text(
            indent + theme_.foreground(ThemeToken::Muted, "Label (empty to remove):"),
            width);
        if (!truncated_prompt) return std::unexpected(truncated_prompt.error());
        lines.push_back(std::move(*truncated_prompt));
        auto input_lines = label_input_.render(
            width > indent.size() ? width - indent.size() : width);
        if (!input_lines) return std::unexpected(input_lines.error());
        for (auto& line : input_lines->lines) {
            auto truncated = cch::tui::truncate_text(indent + line, width);
            if (!truncated) return std::unexpected(truncated.error());
            lines.push_back(std::move(*truncated));
        }
        const auto hint_line =
            key_hint(theme_, *keybindings_, "tui.select.confirm", "save") +
            "  " +
            key_hint(theme_, *keybindings_, "tui.select.cancel", "cancel");
        auto truncated_hint = cch::tui::truncate_text(indent + hint_line, width);
        if (!truncated_hint) return std::unexpected(truncated_hint.error());
        lines.push_back(std::move(*truncated_hint));
    } else {
        if (filtered_nodes_.empty()) {
            auto no_entries = cch::tui::truncate_text(
                theme_.foreground(ThemeToken::Muted, "  No entries found"), width);
            if (!no_entries) return std::unexpected(no_entries.error());
            lines.push_back(std::move(*no_entries));
            auto counter = cch::tui::truncate_text(
                theme_.foreground(
                    ThemeToken::Muted,
                    std::format("  (0/0){}", status_labels())),
                width);
            if (!counter) return std::unexpected(counter.error());
            lines.push_back(std::move(*counter));
        } else {
            const std::size_t start_index = std::min(
                selected_index_ >= max_visible_lines_
                    ? selected_index_ - max_visible_lines_ / 2
                    : 0,
                filtered_nodes_.size() > max_visible_lines_
                    ? filtered_nodes_.size() - max_visible_lines_
                    : 0);
            const std::size_t end_index = std::min(
                start_index + max_visible_lines_, filtered_nodes_.size());

            std::vector<HorizontalViewportRow> rows;
            rows.reserve(end_index - start_index);
            for (std::size_t index = start_index; index < end_index; ++index) {
                const auto& flat = filtered_nodes_[index];
                const auto& entry = flat.node->entry;
                const bool is_selected = index == selected_index_;

                const std::string cursor =
                    is_selected ? theme_.foreground(ThemeToken::Accent, "› ") : "  ";

                // With multiple roots, shift display (roots at 0, not 1).
                const std::size_t display_indent =
                    multiple_roots_ ? std::max<std::size_t>(0, flat.indent - 1)
                                    : flat.indent;

                const bool connector =
                    flat.show_connector && !flat.is_virtual_root_child;
                const std::string connector_text =
                    connector ? (flat.is_last ? "└─ " : "├─ ") : "";
                const std::ptrdiff_t connector_position =
                    connector ? static_cast<std::ptrdiff_t>(display_indent) - 1 : -1;

                // Build the prefix char by char, placing gutters and the
                // connector at their positions.
                const std::size_t total_chars = display_indent * 3;
                std::string prefix_chars;
                prefix_chars.reserve(total_chars * 3);
                const bool is_folded = folded_nodes_.contains(entry.entry_id);
                for (std::size_t char_index = 0; char_index < total_chars;
                     ++char_index) {
                    const std::size_t level = char_index / 3;
                    const std::size_t pos_in_level = char_index % 3;
                    const auto gutter = std::find_if(
                        flat.gutters.begin(), flat.gutters.end(),
                        [&](const GutterInfo& info) {
                            return info.position == level;
                        });
                    if (gutter != flat.gutters.end()) {
                        // The gutter glyphs are multibyte: build with strings
                        // (single-char literals would truncate the UTF-8).
                        if (pos_in_level == 0) {
                            prefix_chars += gutter->show ? "│" : " ";
                        } else {
                            prefix_chars += " ";
                        }
                    } else if (connector &&
                               static_cast<std::ptrdiff_t>(level) ==
                                   connector_position) {
                        if (pos_in_level == 0) {
                            prefix_chars += flat.is_last ? "└" : "├";
                        } else if (pos_in_level == 1) {
                            const bool foldable = is_foldable(entry.entry_id);
                            prefix_chars +=
                                is_folded ? "⊞" : (foldable ? "⊟" : "─");
                        } else {
                            prefix_chars += " ";
                        }
                    } else {
                        prefix_chars += " ";
                    }
                }
                const std::string prefix = std::move(prefix_chars);

                // Fold marker for nodes without connectors (roots).
                const bool shows_fold_in_connector =
                    flat.show_connector && !flat.is_virtual_root_child;
                const std::string fold_marker =
                    is_folded && !shows_fold_in_connector
                    ? theme_.foreground(ThemeToken::Accent, "⊞ ")
                    : "";

                // Active-path marker, right before the entry text.
                const bool is_on_active_path =
                    active_path_ids_.contains(entry.entry_id);
                const std::string path_marker =
                    is_on_active_path ? theme_.foreground(ThemeToken::Accent, "• ") : "";

                const std::string label_text =
                    flat.node->label
                    ? theme_.foreground(
                          ThemeToken::Warning,
                          std::format("[{}] ", *flat.node->label))
                    : "";
                std::string label_timestamp;
                if (show_label_timestamps_ && flat.node->label &&
                    flat.node->label_timestamp) {
                    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch());
                    label_timestamp = theme_.foreground(
                        ThemeToken::Muted,
                        format_label_timestamp(
                            *flat.node->label_timestamp, now.count()) +
                            " ");
                }
                const std::string content =
                    get_entry_display_text(*flat.node, is_selected);
                const std::string prefix_part =
                    theme_.foreground(ThemeToken::Dim, prefix) + fold_marker + path_marker;
                const std::size_t anchor_col = cch::tui::visible_width(prefix_part);

                HorizontalViewportRow row;
                row.gutter = cursor;
                row.body = prefix_part + label_text + label_timestamp + content;
                row.anchor_col = anchor_col;
                row.body_width = cch::tui::visible_width(row.body);
                row.is_selected = is_selected;
                if (is_selected) {
                    row.gutter = theme_.background(ThemeToken::SelectedBg, row.gutter);
                    row.body = theme_.background(ThemeToken::SelectedBg, row.body);
                }
                rows.push_back(std::move(row));
            }

            auto viewport = render_horizontal_viewport(rows, width);
            if (!viewport) return std::unexpected(viewport.error());
            for (auto& line : *viewport) lines.push_back(std::move(line));
            auto counter = cch::tui::truncate_text(
                theme_.foreground(
                    ThemeToken::Muted,
                    std::format(
                        "  ({}/{}){}",
                        selected_index_ + 1,
                        filtered_nodes_.size(),
                        status_labels())),
                width);
            if (!counter) return std::unexpected(counter.error());
            lines.push_back(std::move(*counter));
        }
    }

    lines.push_back("");
    lines.push_back(border());
    return cch::tui::RenderResult{.lines = std::move(lines)};
}

} // namespace cch::coding_agent::tui
