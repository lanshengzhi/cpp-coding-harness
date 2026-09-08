#include "ChatContainer.hpp"
#include "coding_agent/tui/RenderResultUtils.hpp"

#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Markdown.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Utils.hpp>
#include "coding_agent/BoundedText.hpp"
#include "coding_agent/tui/AssistantMessageComponent.hpp"
#include "coding_agent/tui/Theme.hpp"
#include "coding_agent/tui/BashExecutionComponent.hpp"
#include "coding_agent/tui/ToolExecutionComponent.hpp"
#include "coding_agent/tui/UserMessageComponent.hpp"
#include "support/Json.hpp"

#include <cch/support/Error.hpp>
#include <algorithm>
#include <cstddef>
#include <deque>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

[[nodiscard]] std::string safe_text(std::string text) { return bounded_redacted_presentation(std::move(text)); }

/// A MessageStart/MessageUpdate (or snapshot stream) begins/continues a stream
/// even when a synthetic caller leaves the passive stop reason at its default
/// value: inside a stream the assistant message is still Pending.
[[nodiscard]] ai::AssistantMessage as_pending_stream(ai::AssistantMessage message) {
    message.stop_reason = ai::AssistantStopReason::Pending;
    return message;
}
[[nodiscard]] support::Expected<cch::tui::RenderResult> render_plain(
        const LiveTheme& theme, std::string text, std::size_t width, ThemeToken token, bool redact = true) {
    if (redact) text = safe_text(std::move(text));
    cch::tui::Text component(theme.foreground(token, std::move(text)), 0, 0);
    auto rendered = component.render(width);
    if (!rendered) return std::unexpected(rendered.error());
    return rendered;
}

/// Serializes tool-call arguments for presentation (pi JSON.stringify).
[[nodiscard]] std::string serialized_arguments(const support::JsonValue& arguments) {
    if (auto serialized = support::write_json(arguments); serialized) {
        return safe_text(std::move(*serialized));
    }
    return "{}";
}

struct CommittedLineCache {
    std::vector<std::string> lines{};
    std::vector<cch::tui::InlineImageRenderRegion> images{};
    std::size_t cached_width{0};
    bool valid{false};

    void invalidate() {
        valid = false;
        lines.clear();
        images.clear();
        cached_width = 0;
    }
};

void append_cached_lines(cch::tui::RenderResult& destination, const CommittedLineCache& cache) {
    const auto row_offset = destination.lines.size();
    destination.lines.insert(destination.lines.end(), cache.lines.begin(), cache.lines.end());
    for (const auto& image : cache.images) {
        auto copy = image;
        copy.region.row += row_offset;
        destination.images.push_back(std::move(copy));
    }
}

/// The pi `custom-message.ts`/`compaction-summary-message.ts`/
/// `branch-summary-message.ts` box shape: a padded `customMessageBg` box with
/// a bold `[label]` header, a collapsed/expanded body in `customMessageText`,
/// and inline result images.
class BoxedMessageComponent : public cch::tui::Component {
public:
    BoxedMessageComponent(const LiveTheme& theme,
            std::shared_ptr<const SharedKeybindings> keybindings,
            std::string label,
            std::vector<ai::Content> content,
            bool expanded)
        : theme_(theme), keybindings_(std::move(keybindings)), label_(std::move(label)), content_(std::move(content)),
          expanded_(expanded) {
        for (const auto& block : content_) {
            const auto* image = std::get_if<ai::ImageContent>(&block);
            if (image == nullptr) continue;
            auto component = std::make_unique<cch::tui::Image>(
                    cch::tui::ImageContent{
                            .encoded_data = image->data,
                            .mime_type = image->mime_type,
                            .filename = std::nullopt,
                    },
                    cch::tui::ImageOptions{
                            .constraints =
                                    {
                                            .max_width = 60,
                                            .max_height = std::nullopt,
                                    },
                            .fallback_style = theme_.foreground_hook(ThemeToken::CustomMessageText),
                    });
            image_slots_.push_back(std::make_unique<ImageSlot>(ImageSlot{
                    .component = std::move(component),
                    .data = image->data,
                    .mime_type = image->mime_type,
            }));
        }
    }

    BoxedMessageComponent(BoxedMessageComponent&&) noexcept = default;
    // Implicitly deleted anyway (const LiveTheme& member); spell it out.
    BoxedMessageComponent& operator=(BoxedMessageComponent&&) noexcept = delete;
    ~BoxedMessageComponent() override = default;

    BoxedMessageComponent(const BoxedMessageComponent&) = delete;
    BoxedMessageComponent& operator=(const BoxedMessageComponent&) = delete;

    void set_expanded(bool expanded) { expanded_ = expanded; }

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        cch::tui::Box box(1, 1, theme_.background_hook(ThemeToken::CustomMessageBg));
        const auto label =
                theme_.foreground_hook(ThemeToken::CustomMessageLabel)(std::format("\x1b[1m[{}]\x1b[22m", label_));
        auto label_text = std::make_unique<cch::tui::Text>(label, 0, 0);
        (void)box.add_child(std::move(label_text));
        (void)box.add_child(std::make_unique<cch::tui::Spacer>(1));

        const auto expand_key = keybindings_->registry().key_text("app.tools.expand");
        const auto hint = expand_key.empty() ? "Unbound" : expand_key;
        if (!expanded_) {
            auto collapsed =
                    std::make_unique<cch::tui::Text>(theme_.foreground(ThemeToken::CustomMessageText,
                                                             std::format("{} ({} to expand)", collapsed_body(), hint)),
                            0,
                            0);
            (void)box.add_child(std::move(collapsed));
        } else {
            auto style = theme_.markdown_style();
            style.text = theme_.foreground_hook(ThemeToken::CustomMessageText);
            auto markdown = std::make_unique<cch::tui::Markdown>(expanded_body(), 0, 0, std::move(style));
            (void)box.add_child(std::move(markdown));
        }

        auto rendered = box.render(width);
        if (!rendered) return std::unexpected(rendered.error());
        for (const auto& slot : image_slots_) {
            auto image_rendered = slot->component->render(width);
            if (!image_rendered) return std::unexpected(image_rendered.error());
            append_render_result(*rendered, std::move(*image_rendered));
        }
        return rendered;
    }

    void invalidate() override {}

protected:
    [[nodiscard]] virtual std::string collapsed_body() const {
        std::string text;
        for (const auto& block : content_) {
            if (const auto* value = std::get_if<ai::TextContent>(&block)) {
                if (!text.empty()) text.push_back('\n');
                text += value->text;
            }
        }
        return safe_text(std::move(text));
    }

    [[nodiscard]] virtual std::string expanded_body() const { return collapsed_body(); }

    struct ImageSlot {
        std::unique_ptr<cch::tui::Image> component;
        std::string data;
        std::string mime_type;
    };

    const LiveTheme& theme_;                               // must outlive this component.
    std::shared_ptr<const SharedKeybindings> keybindings_; // must outlive this component.
    std::string label_;
    std::vector<ai::Content> content_;
    bool expanded_{false};
    // In content order so multi-image messages render in source order.
    std::vector<std::unique_ptr<ImageSlot>> image_slots_;
};

/// pi `custom-message.ts` default rendering: `[<type>]` label plus the text
/// content; images render inline.
class CustomMessageComponent final : public BoxedMessageComponent {
public:
    CustomMessageComponent(const LiveTheme& theme,
            std::shared_ptr<const SharedKeybindings> keybindings,
            std::string custom_type,
            std::vector<ai::Content> content,
            bool expanded)
        : BoxedMessageComponent(theme, keybindings, std::move(custom_type), std::move(content), expanded) {}
};

/// Formats an integer like pi's `toLocaleString()` (en-US thousands
/// grouping).
[[nodiscard]] std::string locale_grouped_number(std::int64_t value) {
    auto digits = std::to_string(value);
    std::string grouped;
    const auto first_group = digits.size() % 3 == 0 ? 3 : digits.size() % 3;
    for (std::size_t index = 0; index < digits.size(); ++index) {
        if (index != 0 && (index == first_group || (index > first_group && (index - first_group) % 3 == 0))) {
            grouped.push_back(',');
        }
        grouped.push_back(digits[index]);
    }
    return grouped;
}

/// pi `compaction-summary-message.ts`: `[compaction]` label, collapsed
/// `Compacted from N tokens (<key> to expand)` and expanded
/// `**Compacted from N tokens**` + summary.
class CompactionSummaryComponent final : public BoxedMessageComponent {
public:
    CompactionSummaryComponent(const LiveTheme& theme,
            std::shared_ptr<const SharedKeybindings> keybindings,
            std::string summary,
            std::int64_t tokens_before,
            bool expanded)
        : BoxedMessageComponent(theme, keybindings, "compaction", {}, expanded),
          summary_(safe_text(std::move(summary))), tokens_before_(tokens_before) {}

protected:
    [[nodiscard]] std::string collapsed_body() const override {
        return std::format("Compacted from {} tokens", locale_grouped_number(tokens_before_));
    }

    [[nodiscard]] std::string expanded_body() const override {
        return std::format("**Compacted from {} tokens**\n\n{}", locale_grouped_number(tokens_before_), summary_);
    }

private:
    std::string summary_;
    std::int64_t tokens_before_{0};
};

/// pi `branch-summary-message.ts`: `[branch]` label, collapsed
/// `Branch summary (<key> to expand)` and expanded `**Branch Summary**` +
/// summary.
class BranchSummaryComponent final : public BoxedMessageComponent {
public:
    BranchSummaryComponent(const LiveTheme& theme,
            std::shared_ptr<const SharedKeybindings> keybindings,
            std::string summary,
            bool expanded)
        : BoxedMessageComponent(theme, keybindings, "branch", {}, expanded), summary_(safe_text(std::move(summary))) {}

protected:
    [[nodiscard]] std::string collapsed_body() const override { return "Branch summary"; }

    [[nodiscard]] std::string expanded_body() const override {
        return std::format("**Branch Summary**\n\n{}", summary_);
    }

private:
    std::string summary_;
};

} // namespace

struct ChatContainer::Impl {
    enum class ToolStatus { Pending, Success, Failure };

    struct ToolItem {
        std::string call_id;
        std::string name;
        std::string arguments;
        ToolStatus status{ToolStatus::Pending};
        // True between a ToolExecutionStart and its settle. A repeated call
        // id re-enters execution while the snapshot still carries the previous
        // invocation's committed result; replaying that stale result during a
        // transcript rebuild must not clobber the in-flight state (pi
        // correlation by call id renders one component per id).
        bool in_flight{false};
        ai::ToolResultMessage result;
        std::unique_ptr<ToolExecutionComponent> component;
    };

    struct MessageItem {
        ai::MessageVariant message;
        // One of the pi-shaped message components; nullopt for frontend
        // notices and diagnostics.
        std::unique_ptr<cch::tui::Component> component;
        // Assistant tool components in call order (rendered after the
        // assistant message, pi renderSessionItems).
        std::vector<ToolItem*> tools;
        bool committed{false};
        CommittedLineCache cache;
    };

    struct FrontendItem {
        std::string text;
    };

    /// pi's untrusted-project boot warning (`renderProjectTrustWarningIfNeeded`):
    /// a Spacer row above a plain warning-token text (no `Warning:` prefix).
    struct TrustWarningItem {
        std::string text;
    };

    struct DiagnosticItem {
        std::string text;
        bool raw{false};
        /// Renders as a pi boot warning (`Warning: <text>`, warning token)
        /// instead of the error diagnostic shape.
        bool warning{false};
    };

    /// pi `showStatus`: one dim status line. A new status replaces the
    /// previous one while it is still the newest chat item.
    struct StatusItem {
        std::string text;
    };

    using ItemVariant = std::variant<MessageItem, FrontendItem, TrustWarningItem, DiagnosticItem, StatusItem>;

    Impl(const LiveTheme& theme, std::shared_ptr<const SharedKeybindings> keybindings)
        : theme(theme), keybindings(std::move(keybindings)) {}

    void clear() {
        items.clear();
        owned_tools.clear();
        active_assistant_item.reset();
        last_rendered_width = 0;
        cache_hit_count = 0;
        cold_render_count = 0;
        committed_message_count = 0;
        transcript_needs_reconcile = false;
    }

    void invalidate_all_caches() {
        for (auto& item : items) {
            if (auto* message = std::get_if<MessageItem>(&item)) {
                message->cache.invalidate();
            }
        }
    }

    void invalidate_and_update_tool_owner(ToolItem* tool) {
        for (auto& entry : items) {
            auto* message = std::get_if<MessageItem>(&entry);
            if (message == nullptr) continue;
            auto found = std::find(message->tools.begin(), message->tools.end(), tool);
            if (found != message->tools.end()) {
                message->cache.invalidate();
                update_item_commitment(*message);
            }
        }
    }

    void update_item_commitment(MessageItem& item) {
        if (active_assistant_item.has_value()) {
            if (&item == &std::get<MessageItem>(items[*active_assistant_item])) {
                item.committed = false;
                return;
            }
        }
        const bool all_tools_settled = std::all_of(item.tools.begin(), item.tools.end(), [](const ToolItem* tool) {
            return tool != nullptr && tool->status != ToolStatus::Pending;
        });
        if (all_tools_settled) {
            item.committed = true;
        }
    }

    /// Refresh the latest committed bash entry when its snapshot counterpart
    /// mutated in place (cancel flag, output, exit code). The append cursor
    /// cannot observe same-size mutations, so without this the view keeps
    /// the pre-cancel rendering forever (#597). Only the latest bash pair
    /// is compared: cancellation always settles the newest execution, and
    /// items interleave non-message entries so positional pairing beyond
    /// the tail is unreliable. Pairing keys on the command text.
    /// debt: misses in-place mutations of an older bash entry while a newer
    /// one runs; upgrade when the projection exposes per-message versions or
    /// a patch stream (issue #597 snapshot/patch stream).
    void sync_committed_bash(const std::vector<ai::MessageVariant>& messages) {
        const ai::BashExecutionMessage* updated = nullptr;
        for (auto iterator = messages.rbegin(); iterator != messages.rend(); ++iterator) {
            if (const auto* bash = std::get_if<ai::BashExecutionMessage>(&*iterator); bash != nullptr) {
                updated = bash;
                break;
            }
        }
        if (updated == nullptr) return;
        for (auto iterator = items.rbegin(); iterator != items.rend(); ++iterator) {
            auto* item = std::get_if<MessageItem>(&*iterator);
            if (item == nullptr) continue;
            auto* current = std::get_if<ai::BashExecutionMessage>(&item->message);
            if (current == nullptr) continue;
            if (current->command != updated->command) return;
            if (current->output == updated->output && current->exit_code == updated->exit_code &&
                    current->cancelled == updated->cancelled && current->truncated == updated->truncated &&
                    current->full_output_path == updated->full_output_path &&
                    current->exclude_from_context == updated->exclude_from_context) {
                return;
            }
            item->message = ai::MessageVariant{*updated};
            rebuild_message(*item);
            item->cache.invalidate();
            update_item_commitment(*item);
            return;
        }
    }

    void add_message(ai::MessageVariant message, bool from_snapshot = false) {
        // Tool results settle their owning tool component; they never render
        // as standalone chat entries (pi addMessageToChat "toolResult").
        if (const auto* result = std::get_if<ai::ToolResultMessage>(&message)) {
            settle_tool(*result, from_snapshot);
            return;
        }
        items.emplace_back(MessageItem{
                .message = std::move(message),
                .component = {},
                .tools = {},
                .committed = false,
                .cache = {},
        });
        auto& item = std::get<MessageItem>(items.back());
        rebuild_message(item);
        if (const auto* assistant = std::get_if<ai::AssistantMessage>(&item.message)) {
            synchronize_tools(item, *assistant);
            // Snapshot replay must restore the settled provider-outcome
            // invariant: a committed assistant with unmatched tool calls and a
            // non-ToolUse stop reason renders its provider failure on the tool
            // block (pi assistant-message.ts), not as a pending execution.
            settle_provider_tools(*assistant);
        }
        update_item_commitment(item);
    }

    void replace_assistant(const ai::AssistantMessage& message) {
        if (!active_assistant_item) {
            add_message(ai::MessageVariant{message});
            return;
        }
        auto& item = std::get<MessageItem>(items[*active_assistant_item]);
        item.message = ai::MessageVariant{message};
        item.cache.invalidate();
        // A settled assistant may replace an append-only streaming partial
        // with same-length text (for example a provider's final rewrite).
        // Rebuild that terminal component instead of asking the incremental
        // consumer to infer a replacement from a byte offset.
        if (message.stop_reason != ai::AssistantStopReason::Pending) {
            rebuild_message(item);
        } else if (auto* const assistant_comp = dynamic_cast<AssistantMessageComponent*>(item.component.get());
                assistant_comp != nullptr) {
            assistant_comp->update_content(message);
        } else {
            rebuild_message(item);
        }
        synchronize_tools(item, message);
        settle_provider_tools(message);
        update_item_commitment(item);
    }

    void update_latest_assistant(const ai::AssistantMessage& message) {
        for (auto iterator = items.rbegin(); iterator != items.rend(); ++iterator) {
            auto* item = std::get_if<MessageItem>(&*iterator);
            if (item == nullptr || !std::holds_alternative<ai::AssistantMessage>(item->message)) continue;
            item->message = ai::MessageVariant{message};
            item->cache.invalidate();
            if (auto* assistant_comp = dynamic_cast<AssistantMessageComponent*>(item->component.get())) {
                assistant_comp->update_content(message);
                // A passive snapshot can arrive after a pending assistant was
                // cached as an empty item. Rebuild that exceptional stale
                // presentation once; normal streaming updates stay on the
                // incremental component path above.
                if (item->cache.valid && item->cache.lines.empty() && !message.content.empty()) {
                    rebuild_message(*item);
                }
            } else {
                rebuild_message(*item);
            }
            synchronize_tools(*item, message);
            settle_provider_tools(message);
            update_item_commitment(*item);
            return;
        }
    }

    void rebuild_message(MessageItem& item) {
        const auto& message = item.message;
        if (const auto* user = std::get_if<ai::UserMessage>(&message)) {
            item.component = std::make_unique<UserMessageComponent>(theme, user->content, output_pad);
            return;
        }
        if (const auto* assistant = std::get_if<ai::AssistantMessage>(&message)) {
            auto component =
                    std::make_unique<AssistantMessageComponent>(theme, hide_thinking_block, "Thinking...", output_pad);
            component->update_content(*assistant);
            item.component = std::move(component);
            return;
        }
        if (const auto* bash = std::get_if<ai::BashExecutionMessage>(&message)) {
            auto component = std::make_unique<BashExecutionComponent>(
                    theme, keybindings, bounded_presentation(bash->command), bash->exclude_from_context);
            component->append_output(bounded_presentation(bash->output));
            component->set_complete(bash->exit_code, bash->cancelled, bash->truncated, bash->full_output_path);
            component->set_expanded(tools_expanded);
            item.component = std::move(component);
            return;
        }
        if (const auto* custom = std::get_if<ai::CustomMessage>(&message)) {
            if (custom->display) {
                item.component = std::make_unique<CustomMessageComponent>(
                        theme, keybindings, custom->custom_type, custom->content, tools_expanded);
            } else {
                item.component.reset();
            }
            return;
        }
        if (const auto* branch = std::get_if<ai::BranchSummaryMessage>(&message)) {
            item.component =
                    std::make_unique<BranchSummaryComponent>(theme, keybindings, branch->summary, tools_expanded);
            return;
        }
        if (const auto* compaction = std::get_if<ai::CompactionSummaryMessage>(&message)) {
            item.component = std::make_unique<CompactionSummaryComponent>(
                    theme, keybindings, compaction->summary, compaction->tokens_before, tools_expanded);
            return;
        }
        // System messages are not rendered in pi's chat.
        item.component.reset();
    }

    void synchronize_tools(MessageItem& item, const ai::AssistantMessage& assistant) {
        std::vector<ToolItem*> claimed;
        claimed.reserve(assistant.content.size());
        for (const auto& block : assistant.content) {
            const auto* call = std::get_if<ai::ToolCallContent>(&block);
            if (call == nullptr) continue;
            auto& tool = ensure_tool(call->id, call->name, call->raw_arguments);
            if (tool.name != call->name || tool.arguments != call->raw_arguments) {
                tool.name = call->name;
                tool.arguments = call->raw_arguments;
                tool.component->update_args(call->raw_arguments);
            }
            claim_tool(&tool);
            claimed.push_back(&tool);
        }
        // Drop tools that no longer belong to this assistant message.
        for (auto iterator = item.tools.begin(); iterator != item.tools.end();) {
            if (std::find(claimed.begin(), claimed.end(), *iterator) == claimed.end()) {
                iterator = item.tools.erase(iterator);
            } else {
                ++iterator;
            }
        }
        for (auto* tool : claimed) {
            if (std::find(item.tools.begin(), item.tools.end(), tool) == item.tools.end()) {
                item.tools.push_back(tool);
            }
        }
    }

    /// Removes a tool from every other message item so it renders once, in
    /// its owning assistant's order.
    void claim_tool(ToolItem* tool) {
        for (auto iterator = items.begin(); iterator != items.end();) {
            auto* message = std::get_if<MessageItem>(&*iterator);
            if (message == nullptr) {
                ++iterator;
                continue;
            }
            auto found = std::find(message->tools.begin(), message->tools.end(), tool);
            if (found != message->tools.end()) {
                message->tools.erase(found);
                message->cache.invalidate();
                if (std::holds_alternative<ai::ToolResultMessage>(message->message) && message->component == nullptr &&
                        message->tools.empty()) {
                    iterator = items.erase(iterator);
                    continue;
                }
                update_item_commitment(*message);
            }
            ++iterator;
        }
    }

    void settle_provider_tools(const ai::AssistantMessage& assistant) {
        if (assistant.stop_reason == ai::AssistantStopReason::ToolUse ||
                assistant.stop_reason == ai::AssistantStopReason::Pending) {
            return;
        }
        for (const auto& block : assistant.content) {
            const auto* call = std::get_if<ai::ToolCallContent>(&block);
            if (call == nullptr) continue;
            auto& tool = ensure_tool(call->id, call->name, call->raw_arguments);
            if (tool.status != ToolStatus::Pending) continue;
            tool.status = ToolStatus::Failure;
            tool.result = ai::tool_result_message(call->id, call->name, tool_failure_detail(assistant), true);
            tool.component->update_result(tool.result);
            invalidate_and_update_tool_owner(&tool);
        }
    }

    /// Applies a tool result to its owning component. `from_snapshot` marks
    /// transcript replay: a committed result for a call id that is currently
    /// executing again belongs to the previous invocation and is skipped so it
    /// cannot overwrite the in-flight partial state (#1752 repeated call id).
    void settle_tool(const ai::ToolResultMessage& result, bool from_snapshot = false) {
        auto& tool = ensure_tool(result.tool_call_id, result.tool_name, {});
        if (from_snapshot && tool.in_flight) return;
        tool.in_flight = false;
        tool.status = result.is_error ? ToolStatus::Failure : ToolStatus::Success;
        tool.result = result;
        tool.component->update_result(result);
        invalidate_and_update_tool_owner(&tool);
    }

    [[nodiscard]] ai::ToolResultMessage result_from_projection(const ToolExecutionSnapshot& execution) const {
        ai::ToolResultMessage result;
        result.tool_call_id = execution.tool_call_id;
        result.tool_name = execution.tool_name;
        result.is_error = execution.status == ToolExecutionStatus::Failed;
        if (!execution.output_tail.empty()) {
            result.content.emplace_back(ai::text_content(execution.output_tail));
        }
        if (execution.error && execution.output_tail.empty()) {
            result.content.emplace_back(ai::text_content(*execution.error));
        }
        if (execution.artifact_reference) {
            result.details = support::JsonValue::object_t{{"artifact_reference", *execution.artifact_reference}};
        }
        return result;
    }

    /// Rebuild tool execution presentation entirely from the projection read
    /// model. This is the recovery path for attach and mailbox overflow; it
    /// must not depend on an earlier Agent lifecycle event being remembered by
    /// this frontend.
    void reconcile_tool_executions(const std::vector<ToolExecutionSnapshot>& executions) {
        for (const auto& execution : executions) {
            auto& tool = ensure_tool(execution.tool_call_id,
                    execution.tool_name,
                    execution.arguments_json,
                    execution.status == ToolExecutionStatus::Running);
            const auto result = result_from_projection(execution);
            if (execution.status == ToolExecutionStatus::Running) {
                tool.in_flight = true;
                tool.status = ToolStatus::Pending;
                tool.result = result;
                tool.component->update_result(result, true);
            } else {
                tool.in_flight = false;
                tool.status =
                        execution.status == ToolExecutionStatus::Failed ? ToolStatus::Failure : ToolStatus::Success;
                // Snapshot history may carry image blocks or other structured
                // result content that the bounded projection tail cannot
                // represent. Keep that authoritative transcript result when
                // it is already present; use the read-model value only while
                // no committed result has arrived yet.
                if (tool.result.tool_call_id.empty()) {
                    tool.result = result;
                    tool.component->update_result(result);
                }
            }
            invalidate_and_update_tool_owner(&tool);
        }
    }

    [[nodiscard]] ToolItem& ensure_tool(const std::string& call_id,
            const std::string& name,
            const std::string& arguments,
            bool begin_call = false) {
        if (const auto found = owned_tools.find(call_id); found != owned_tools.end()) {
            auto& tool = *found->second;
            if (!begin_call || tool.status == ToolStatus::Pending) {
                if (tool.name != name) {
                    tool.name = name;
                    tool.component->update_args(arguments);
                }
            }
            return tool;
        }
        // A tool without an owning assistant message renders standalone.
        items.emplace_back(MessageItem{
                .message = ai::MessageVariant{ai::ToolResultMessage{}},
                .component = {},
                .tools = {},
                .committed = false,
                .cache = {},
        });
        auto& item = std::get<MessageItem>(items.back());
        auto tool = std::make_unique<ToolItem>(ToolItem{
                .call_id = call_id,
                .name = name,
                .arguments = arguments,
                .result = {},
                .component = std::make_unique<ToolExecutionComponent>(theme, keybindings, name, call_id, arguments),
        });
        tool->component->set_expanded(tools_expanded);
        auto* tool_pointer = tool.get();
        item.tools.push_back(tool_pointer);
        owned_tools.insert_or_assign(call_id, std::move(tool));
        update_item_commitment(item);
        return *tool_pointer;
    }

    [[nodiscard]] std::string tool_failure_detail(const ai::AssistantMessage& assistant) {
        if (assistant.stop_reason == ai::AssistantStopReason::Aborted) {
            if (assistant.error_message && *assistant.error_message != "Request was aborted") {
                return *assistant.error_message;
            }
            return "Operation aborted";
        }
        return assistant.error_message.value_or("Provider ended before tool execution");
    }

    void apply_expanded_to_components() {
        for (auto& item : items) {
            auto* message = std::get_if<MessageItem>(&item);
            if (message == nullptr) continue;
            if (const auto* bash = std::get_if<ai::BashExecutionMessage>(&message->message)) {
                (void)bash;
                if (message->component) {
                    static_cast<BashExecutionComponent*>(message->component.get())->set_expanded(tools_expanded);
                }
            } else if (auto* custom = std::get_if<ai::CustomMessage>(&message->message)) {
                (void)custom;
                if (message->component) {
                    static_cast<BoxedMessageComponent*>(message->component.get())->set_expanded(tools_expanded);
                }
            } else if (std::holds_alternative<ai::BranchSummaryMessage>(message->message) ||
                       std::holds_alternative<ai::CompactionSummaryMessage>(message->message)) {
                if (message->component) {
                    static_cast<BoxedMessageComponent*>(message->component.get())->set_expanded(tools_expanded);
                }
            }
            for (auto* tool : message->tools) {
                tool->component->set_expanded(tools_expanded);
            }
        }
    }

    void apply_thinking_to_components() {
        for (auto& item : items) {
            auto* message = std::get_if<MessageItem>(&item);
            if (message == nullptr) continue;
            if (std::holds_alternative<ai::AssistantMessage>(message->message) && message->component) {
                static_cast<AssistantMessageComponent*>(message->component.get())
                        ->set_hide_thinking_block(hide_thinking_block);
            }
        }
    }

    void apply_output_pad_to_components() {
        for (auto& item : items) {
            auto* message = std::get_if<MessageItem>(&item);
            if (message == nullptr || message->component == nullptr) continue;
            if (std::holds_alternative<ai::UserMessage>(message->message) ||
                    std::holds_alternative<ai::AssistantMessage>(message->message)) {
                rebuild_message(*message);
            }
        }
    }

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render_item(
            ItemVariant& item, std::size_t width, std::size_t item_index) {
        (void)item_index;
        if (auto* message = std::get_if<MessageItem>(&item)) {
            if (message->committed && message->cache.valid && message->cache.cached_width == width) {
                ++cache_hit_count;
                cch::tui::RenderResult cached_result;
                cached_result.lines = message->cache.lines;
                cached_result.images = message->cache.images;
                return cached_result;
            }
            ++cold_render_count;
            cch::tui::RenderResult result;
            if (message->component) {
                auto rendered = message->component->render(width);
                if (!rendered) return std::unexpected(rendered.error());
                append_render_result(result, std::move(*rendered));
            }
            for (const auto* tool : message->tools) {
                auto rendered = tool->component->render(width);
                if (!rendered) return std::unexpected(rendered.error());
                append_render_result(result, std::move(*rendered));
            }
            if (message->committed) {
                message->cache.lines = result.lines;
                message->cache.images = result.images;
                message->cache.cached_width = width;
                message->cache.valid = true;
            }
            return result;
        }
        if (const auto* frontend = std::get_if<FrontendItem>(&item)) {
            return render_plain(theme, frontend->text, width, ThemeToken::Text);
        }
        if (const auto* trust_warning = std::get_if<TrustWarningItem>(&item)) {
            // pi `renderProjectTrustWarningIfNeeded`: Spacer(1) above the
            // warning-token text, only when the chat already has children.
            cch::tui::RenderResult result;
            if (item_index > 0) {
                result.lines.emplace_back();
            }
            auto rendered = render_plain(theme, trust_warning->text, width, ThemeToken::Warning);
            if (!rendered) return std::unexpected(rendered.error());
            for (auto& line : rendered->lines)
                result.lines.push_back(std::move(line));
            return result;
        }
        if (const auto* status = std::get_if<StatusItem>(&item)) {
            // pi showStatus: a Spacer row above the dim status text.
            cch::tui::RenderResult result;
            result.lines.emplace_back();
            auto rendered = render_plain(theme, status->text, width, ThemeToken::Dim);
            if (!rendered) return std::unexpected(rendered.error());
            for (auto& line : rendered->lines)
                result.lines.push_back(std::move(line));
            return result;
        }
        const auto& diagnostic = std::get<DiagnosticItem>(item);
        if (diagnostic.warning) {
            return render_plain(theme, "Warning: " + diagnostic.text, width, ThemeToken::Warning, !diagnostic.raw);
        }
        return render_plain(theme, "Error: " + diagnostic.text, width, ThemeToken::Error, !diagnostic.raw);
    }

    const LiveTheme& theme; // must outlive this presentation reducer.
    /// The shared keybinding slot (ADR 0035); the strong reference keeps
    /// the registry alive for every render.
    std::shared_ptr<const SharedKeybindings> keybindings;
    std::deque<ItemVariant> items;
    std::unordered_map<std::string, std::unique_ptr<ToolItem>> owned_tools;
    std::optional<std::size_t> active_assistant_item;
    bool tools_expanded{false};
    bool hide_thinking_block{false};
    std::size_t output_pad{1};
    std::size_t last_rendered_width{0};
    std::uint64_t cache_hit_count{0};
    std::uint64_t cold_render_count{0};
    std::size_t committed_message_count{0};
    bool transcript_needs_reconcile{false};
};

ChatContainer::ChatContainer(const LiveTheme& theme, std::shared_ptr<const SharedKeybindings> keybindings)
    : impl_(std::make_unique<Impl>(theme, std::move(keybindings))) {}
ChatContainer::ChatContainer(ChatContainer&&) noexcept = default;
ChatContainer& ChatContainer::operator=(ChatContainer&&) noexcept = default;
ChatContainer::~ChatContainer() = default;

void ChatContainer::initialize(const AgentSessionSnapshot& snapshot) {
    // The transcript is message-owned: a rebuild replaces MessageItems from
    // the snapshot. Host-appended notices (diagnostics, status, frontend)
    // are view-local and never appear in a snapshot, so dropping them on a
    // rebuild would erase e.g. the persistence-failure diagnostic right after
    // the failed prompt appended it (#597). Preserve them and re-append after
    // the rebuilt transcript.
    std::vector<Impl::ItemVariant> host_notices;
    for (auto& item : impl_->items) {
        if (!std::holds_alternative<Impl::MessageItem>(item)) {
            host_notices.push_back(std::move(item));
        }
    }
    // In-flight tool executions outlive transcript rebuilds: preserve Pending
    // entries across the clear below so a transient rebuild cannot lose live
    // output before the projection read model is reconciled. Settled entries
    // rebuild from the snapshot instead, so stale outcomes cannot survive a
    // rewind (#597).
    std::unordered_map<std::string, std::unique_ptr<Impl::ToolItem>> pending_tools;
    for (auto& entry : impl_->owned_tools) {
        if (entry.second != nullptr && entry.second->status == Impl::ToolStatus::Pending) {
            pending_tools.emplace(entry.first, std::move(entry.second));
        }
    }
    impl_->clear();
    for (auto& entry : pending_tools) {
        impl_->owned_tools.emplace(entry.first, std::move(entry.second));
    }
    for (const auto& message : snapshot.agent_state.messages) {
        impl_->add_message(message, true);
    }
    impl_->committed_message_count = snapshot.agent_state.messages.size();
    for (auto& item : impl_->items) {
        if (auto* msg = std::get_if<Impl::MessageItem>(&item)) {
            impl_->update_item_commitment(*msg);
        }
    }
    if (snapshot.agent_state.streaming_message) {
        const auto assistant_index = impl_->items.size();
        impl_->add_message(ai::MessageVariant{as_pending_stream(*snapshot.agent_state.streaming_message)});
        impl_->active_assistant_item = assistant_index;
        impl_->transcript_needs_reconcile = true;
        if (assistant_index < impl_->items.size()) {
            if (auto* msg = std::get_if<Impl::MessageItem>(&impl_->items[assistant_index])) {
                msg->committed = false;
                msg->cache.invalidate();
            }
        }
    }
    for (auto& notice : host_notices) {
        impl_->items.push_back(std::move(notice));
    }
    impl_->reconcile_tool_executions(snapshot.tool_executions);
}
void ChatContainer::reconcile_snapshot(const AgentSessionSnapshot& snapshot) {
    const auto& messages = snapshot.agent_state.messages;
    if (!snapshot.agent_state.streaming_message && impl_->transcript_needs_reconcile) {
        // A projection can briefly expose the newly submitted user message
        // before the assistant's committed MessageEnd. Rebuild from the
        // snapshot once that committed assistant arrives; otherwise fall
        // through and append committed history below. Never return here
        // merely because the back is not an assistant: with no live
        // streaming item there is nothing to preserve, and returning would
        // skip trailing committed entries (e.g. a cancelled bash message
        // at Session Close) forever (#597).
        if (!messages.empty() && std::get_if<ai::AssistantMessage>(&messages.back()) != nullptr) {
            initialize(snapshot);
            return;
        }
    }
    if (messages.size() < impl_->committed_message_count) {
        initialize(snapshot);
        return;
    }

    std::size_t next_message = impl_->committed_message_count;
    if (impl_->active_assistant_item && !snapshot.agent_state.streaming_message && next_message < messages.size()) {
        const auto* assistant = std::get_if<ai::AssistantMessage>(&messages[next_message]);
        if (assistant == nullptr) {
            initialize(snapshot);
            return;
        }
        impl_->replace_assistant(*assistant);
        const auto assistant_index = *impl_->active_assistant_item;
        impl_->active_assistant_item.reset();
        if (assistant_index < impl_->items.size()) {
            if (auto* item = std::get_if<Impl::MessageItem>(&impl_->items[assistant_index])) {
                impl_->update_item_commitment(*item);
            }
        }
        ++next_message;
    } else if (impl_->active_assistant_item && !snapshot.agent_state.streaming_message) {
        // The lifecycle event is authoritative while the projection catches
        // up. Do not erase a visible live assistant just because this frame
        // sampled the short interval between MessageEnd delivery and the
        // committed history update.
        return;
    }

    for (; next_message < messages.size(); ++next_message) {
        impl_->add_message(messages[next_message]);
    }
    impl_->committed_message_count = messages.size();
    impl_->sync_committed_bash(messages);
    if (!messages.empty()) {
        if (const auto* assistant = std::get_if<ai::AssistantMessage>(&messages.back())) {
            impl_->update_latest_assistant(*assistant);
        }
    }

    if (snapshot.agent_state.streaming_message) {
        const auto streaming_message = as_pending_stream(*snapshot.agent_state.streaming_message);
        if (impl_->active_assistant_item) {
            impl_->replace_assistant(streaming_message);
        } else {
            const auto assistant_index = impl_->items.size();
            impl_->add_message(ai::MessageVariant{streaming_message});
            impl_->active_assistant_item = assistant_index;
            if (assistant_index < impl_->items.size()) {
                if (auto* message = std::get_if<Impl::MessageItem>(&impl_->items[assistant_index])) {
                    message->committed = false;
                    message->cache.invalidate();
                }
            }
        }
    }
    impl_->reconcile_tool_executions(snapshot.tool_executions);
}

void ChatContainer::apply_event(const agent::AgentLifecycleEvent& event) {
    if (const auto* start = std::get_if<agent::MessageStartEvent>(&event)) {
        if (const auto* assistant = std::get_if<ai::AssistantMessage>(&start->message)) {
            // Capture the item index before add_message: synchronize_tools
            // may append standalone tool items after it, so the streaming
            // target must not be the last deque entry.
            const auto assistant_index = impl_->items.size();
            impl_->add_message(ai::MessageVariant{as_pending_stream(*assistant)});
            impl_->active_assistant_item = assistant_index;
            if (assistant_index < impl_->items.size()) {
                if (auto* msg = std::get_if<Impl::MessageItem>(&impl_->items[assistant_index])) {
                    msg->committed = false;
                    msg->cache.invalidate();
                }
            }
        } else {
            impl_->add_message(start->message);
        }
        return;
    }
    if (const auto* update = std::get_if<agent::MessageUpdateEvent>(&event)) {
        if (const auto* assistant = std::get_if<ai::AssistantMessage>(&update->message)) {
            impl_->replace_assistant(as_pending_stream(*assistant));
        }
        return;
    }
    if (const auto* end = std::get_if<agent::MessageEndEvent>(&event)) {
        if (std::holds_alternative<ai::AssistantMessage>(end->message)) {
            const auto& assistant = std::get<ai::AssistantMessage>(end->message);
            impl_->replace_assistant(assistant);
            if (impl_->active_assistant_item) {
                const auto index = *impl_->active_assistant_item;
                impl_->active_assistant_item.reset();
                if (index < impl_->items.size()) {
                    if (auto* item = std::get_if<Impl::MessageItem>(&impl_->items[index])) {
                        impl_->update_item_commitment(*item);
                    }
                }
            }
        } else if (const auto* result = std::get_if<ai::ToolResultMessage>(&end->message)) {
            impl_->settle_tool(*result);
        }
        ++impl_->committed_message_count;
        impl_->transcript_needs_reconcile = true;
        return;
    }
    if (const auto* start = std::get_if<agent::ToolExecutionStartEvent>(&event)) {
        auto& tool = impl_->ensure_tool(start->tool_call_id, start->tool_name, serialized_arguments(start->args), true);
        tool.status = Impl::ToolStatus::Pending;
        tool.in_flight = true;
        impl_->invalidate_and_update_tool_owner(&tool);
        return;
    }
    if (const auto* update = std::get_if<agent::ToolExecutionUpdateEvent>(&event)) {
        auto& tool = impl_->ensure_tool(update->tool_call_id, update->tool_name, serialized_arguments(update->args));
        if (tool.status != Impl::ToolStatus::Pending) return;
        tool.component->update_result(
                ai::ToolResultMessage{
                        .tool_call_id = tool.call_id,
                        .tool_name = tool.name,
                        .content = update->partial_result.content,
                        .details = update->partial_result.details,
                        .is_error = update->partial_result.is_error,
                },
                true);
        impl_->invalidate_and_update_tool_owner(&tool);
        return;
    }
    if (const auto* end = std::get_if<agent::ToolExecutionEndEvent>(&event)) {
        auto& tool = impl_->ensure_tool(end->tool_call_id, end->tool_name, {});
        tool.in_flight = false;
        tool.status = end->is_error || end->result.is_error ? Impl::ToolStatus::Failure : Impl::ToolStatus::Success;
        tool.result = ai::ToolResultMessage{
                .tool_call_id = tool.call_id,
                .tool_name = tool.name,
                .content = end->result.content,
                .details = end->result.details,
                .is_error = end->result.is_error,
        };
        tool.component->update_result(tool.result);
        impl_->invalidate_and_update_tool_owner(&tool);
    }
}
void ChatContainer::append_committed_message(ai::MessageVariant message) {
    impl_->add_message(std::move(message));
    ++impl_->committed_message_count;
}

void ChatContainer::clear() { impl_->clear(); }

void ChatContainer::append_frontend_message(std::string text) {
    if (!text.empty()) {
        impl_->items.emplace_back(Impl::FrontendItem{safe_text(std::move(text))});
    }
}

void ChatContainer::append_diagnostic(std::string text) {
    impl_->items.emplace_back(Impl::DiagnosticItem{safe_text(std::move(text))});
}

void ChatContainer::append_warning(std::string text) {
    impl_->items.emplace_back(Impl::DiagnosticItem{
            .text = safe_text(std::move(text)),
            .raw = false,
            .warning = true,
    });
}

void ChatContainer::append_trust_warning(std::string text) {
    impl_->items.emplace_back(Impl::TrustWarningItem{
            .text = safe_text(std::move(text)),
    });
}

void ChatContainer::append_status_message(std::string text) {
    // pi showStatus: replace the tail status while it is the newest item.
    if (!impl_->items.empty()) {
        if (auto* tail = std::get_if<Impl::StatusItem>(&impl_->items.back())) {
            tail->text = safe_text(std::move(text));
            return;
        }
    }
    impl_->items.emplace_back(Impl::StatusItem{.text = safe_text(std::move(text))});
}

void ChatContainer::append_user_bash_diagnostic(std::string text) {
    impl_->items.emplace_back(Impl::DiagnosticItem{
            .text = bounded_presentation(text),
            .raw = true,
    });
}

void ChatContainer::toggle_tool_output() {
    impl_->tools_expanded = !impl_->tools_expanded;
    impl_->apply_expanded_to_components();
    impl_->invalidate_all_caches();
}

void ChatContainer::set_hide_thinking_block(bool hide) {
    impl_->hide_thinking_block = hide;
    impl_->apply_thinking_to_components();
    impl_->invalidate_all_caches();
}

void ChatContainer::set_output_pad(std::size_t output_pad) {
    if (impl_->output_pad == output_pad) return;
    impl_->output_pad = output_pad;
    impl_->apply_output_pad_to_components();
    impl_->invalidate_all_caches();
}

bool ChatContainer::tools_expanded() const { return impl_->tools_expanded; }

support::Expected<cch::tui::RenderResult> ChatContainer::render(std::size_t width) {
    if (width != impl_->last_rendered_width) {
        impl_->invalidate_all_caches();
        impl_->last_rendered_width = width;
    }

    cch::tui::RenderResult result;
    for (std::size_t index = 0; index < impl_->items.size(); ++index) {
        auto& item = impl_->items[index];
        if (auto* message = std::get_if<Impl::MessageItem>(&item)) {
            const auto* assistant = std::get_if<ai::AssistantMessage>(&message->message);
            const bool stale_empty_assistant_cache =
                    assistant != nullptr && !assistant->content.empty() && message->cache.lines.empty();
            if (message->committed && message->cache.valid && message->cache.cached_width == width &&
                    !stale_empty_assistant_cache) {
                ++impl_->cache_hit_count;
                append_cached_lines(result, message->cache);
                continue;
            }
            if (stale_empty_assistant_cache) message->cache.invalidate();
        }
        auto rendered = impl_->render_item(item, width, index);
        if (!rendered) return std::unexpected(rendered.error());
        append_render_result(result, std::move(*rendered));
    }
    return result;
}

void ChatContainer::invalidate() {
    for (auto& item : impl_->items) {
        if (auto* message = std::get_if<Impl::MessageItem>(&item)) {
            message->cache.invalidate();
            if (message->component) {
                message->component->invalidate();
            }
        }
    }
}

std::size_t ChatContainer::item_count() const { return impl_->items.size(); }

bool ChatContainer::is_item_committed(std::size_t index) const {
    if (index >= impl_->items.size()) return false;
    const auto* message = std::get_if<Impl::MessageItem>(&impl_->items[index]);
    return message != nullptr && message->committed;
}

bool ChatContainer::is_item_cache_valid(std::size_t index) const {
    if (index >= impl_->items.size()) return false;
    const auto* message = std::get_if<Impl::MessageItem>(&impl_->items[index]);
    return message != nullptr && message->cache.valid;
}

std::size_t ChatContainer::cached_line_count(std::size_t index) const {
    if (index >= impl_->items.size()) return 0;
    const auto* message = std::get_if<Impl::MessageItem>(&impl_->items[index]);
    if (message == nullptr || !message->cache.valid) return 0;
    return message->cache.lines.size();
}

std::size_t ChatContainer::committed_item_count() const {
    std::size_t count = 0;
    for (const auto& item : impl_->items) {
        if (const auto* message = std::get_if<Impl::MessageItem>(&item); message != nullptr && message->committed) {
            ++count;
        }
    }
    return count;
}

std::uint64_t ChatContainer::cache_hit_count() const { return impl_->cache_hit_count; }

std::uint64_t ChatContainer::cold_render_count() const { return impl_->cold_render_count; }

} // namespace cch::coding_agent::tui
