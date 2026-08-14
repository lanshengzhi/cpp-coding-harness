#include "ChatContainer.hpp"

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
#include "util/Json.hpp"

#include <cch/util/Error.hpp>
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

[[nodiscard]] std::string safe_text(std::string text) {
    return bounded_redacted_presentation(std::move(text));
}

[[nodiscard]] util::Expected<cch::tui::RenderResult> render_plain(
    const LiveTheme& theme,
    std::string text,
    std::size_t width,
    ThemeToken token,
    bool redact = true) {
    if (redact) text = safe_text(std::move(text));
    cch::tui::Text component(theme.foreground(token, std::move(text)), 0, 0);
    auto rendered = component.render(width);
    if (!rendered) return std::unexpected(rendered.error());
    return rendered;
}

/// Serializes tool-call arguments for presentation (pi JSON.stringify).
[[nodiscard]] std::string serialized_arguments(const util::JsonValue& arguments) {
    if (auto serialized = util::write_json(arguments); serialized) {
        return safe_text(std::move(*serialized));
    }
    return "{}";
}

void append_render_result(
    cch::tui::RenderResult& destination,
    cch::tui::RenderResult rendered) {
    const auto row_offset = destination.lines.size();
    destination.lines.insert(
        destination.lines.end(),
        std::make_move_iterator(rendered.lines.begin()),
        std::make_move_iterator(rendered.lines.end()));
    for (auto& image : rendered.images) {
        image.region.row += row_offset;
        destination.images.push_back(std::move(image));
    }
}

void append_lines(std::vector<std::string>& destination, std::vector<std::string> lines) {
    destination.insert(
        destination.end(),
        std::make_move_iterator(lines.begin()),
        std::make_move_iterator(lines.end()));
}

/// The pi `custom-message.ts`/`compaction-summary-message.ts`/
/// `branch-summary-message.ts` box shape: a padded `customMessageBg` box with
/// a bold `[label]` header, a collapsed/expanded body in `customMessageText`,
/// and inline result images.
class BoxedMessageComponent : public cch::tui::Component {
public:
    BoxedMessageComponent(
        const LiveTheme& theme,
        std::shared_ptr<const SharedKeybindings> keybindings,
        std::string label,
        std::vector<ai::Content> content,
        bool expanded)
        : theme_(theme),
          keybindings_(std::move(keybindings)),
          label_(std::move(label)),
          content_(std::move(content)),
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
                    .constraints = {
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
    BoxedMessageComponent& operator=(BoxedMessageComponent&&) noexcept = default;
    ~BoxedMessageComponent() override = default;

    BoxedMessageComponent(const BoxedMessageComponent&) = delete;
    BoxedMessageComponent& operator=(const BoxedMessageComponent&) = delete;

    void set_expanded(bool expanded) {
        expanded_ = expanded;
    }

    [[nodiscard]] util::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        cch::tui::Box box(
            1,
            1,
            theme_.background_hook(ThemeToken::CustomMessageBg));
        const auto label = theme_.foreground_hook(ThemeToken::CustomMessageLabel)(
            std::format("\x1b[1m[{}]\x1b[22m", label_));
        auto label_text = std::make_unique<cch::tui::Text>(label, 0, 0);
        (void)box.add_child(std::move(label_text));
        (void)box.add_child(std::make_unique<cch::tui::Spacer>(1));

        const auto expand_key = keybindings_->registry().key_text("app.tools.expand");
        const auto hint = expand_key.empty() ? "Unbound" : expand_key;
        if (!expanded_) {
            auto collapsed = std::make_unique<cch::tui::Text>(
                theme_.foreground(
                    ThemeToken::CustomMessageText,
                    std::format("{} ({} to expand)", collapsed_body(), hint)),
                0,
                0);
            (void)box.add_child(std::move(collapsed));
        } else {
            auto style = theme_.markdown_style();
            style.text = theme_.foreground_hook(ThemeToken::CustomMessageText);
            auto markdown = std::make_unique<cch::tui::Markdown>(
                expanded_body(),
                0,
                0,
                std::move(style));
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

    [[nodiscard]] virtual std::string expanded_body() const {
        return collapsed_body();
    }

    struct ImageSlot {
        std::unique_ptr<cch::tui::Image> component;
        std::string data;
        std::string mime_type;
    };

    const LiveTheme& theme_; // must outlive this component.
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
    CustomMessageComponent(
        const LiveTheme& theme,
        std::shared_ptr<const SharedKeybindings> keybindings,
        std::string custom_type,
        std::vector<ai::Content> content,
        bool expanded)
        : BoxedMessageComponent(
              theme,
              keybindings,
              std::move(custom_type),
              std::move(content),
              expanded) {}
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
    CompactionSummaryComponent(
        const LiveTheme& theme,
        std::shared_ptr<const SharedKeybindings> keybindings,
        std::string summary,
        std::int64_t tokens_before,
        bool expanded)
        : BoxedMessageComponent(theme, keybindings, "compaction", {}, expanded),
          summary_(safe_text(std::move(summary))),
          tokens_before_(tokens_before) {}

protected:
    [[nodiscard]] std::string collapsed_body() const override {
        return std::format("Compacted from {} tokens", locale_grouped_number(tokens_before_));
    }

    [[nodiscard]] std::string expanded_body() const override {
        return std::format(
            "**Compacted from {} tokens**\n\n{}",
            locale_grouped_number(tokens_before_),
            summary_);
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
    BranchSummaryComponent(
        const LiveTheme& theme,
        std::shared_ptr<const SharedKeybindings> keybindings,
        std::string summary,
        bool expanded)
        : BoxedMessageComponent(theme, keybindings, "branch", {}, expanded),
          summary_(safe_text(std::move(summary))) {}

protected:
    [[nodiscard]] std::string collapsed_body() const override {
        return "Branch summary";
    }

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

    using ItemVariant = std::variant<
        MessageItem,
        FrontendItem,
        TrustWarningItem,
        DiagnosticItem,
        StatusItem>;

    Impl(
        const LiveTheme& theme,
        std::shared_ptr<const SharedKeybindings> keybindings)
        : theme(theme),
          keybindings(std::move(keybindings)) {}

    void clear() {
        items.clear();
        owned_tools.clear();
        active_assistant_item.reset();
    }

    void add_message(ai::MessageVariant message) {
        // Tool results settle their owning tool component; they never render
        // as standalone chat entries (pi addMessageToChat "toolResult").
        if (const auto* result = std::get_if<ai::ToolResultMessage>(&message)) {
            settle_tool(*result);
            return;
        }
        items.emplace_back(MessageItem{
            .message = std::move(message),
        });
        auto& item = std::get<MessageItem>(items.back());
        rebuild_message(item);
        if (const auto* assistant = std::get_if<ai::AssistantMessage>(&item.message)) {
            synchronize_tools(item, *assistant);
        }
    }

    void replace_assistant(const ai::AssistantMessage& message) {
        if (!active_assistant_item) {
            add_message(ai::MessageVariant{message});
            return;
        }
        auto& item = std::get<MessageItem>(items[*active_assistant_item]);
        item.message = ai::MessageVariant{message};
        rebuild_message(item);
        synchronize_tools(item, message);
        settle_provider_tools(message);
    }

    void rebuild_message(MessageItem& item) {
        const auto& message = item.message;
        if (const auto* user = std::get_if<ai::UserMessage>(&message)) {
            item.component = std::make_unique<UserMessageComponent>(
                theme, user->content, output_pad);
            return;
        }
        if (const auto* assistant = std::get_if<ai::AssistantMessage>(&message)) {
            auto component = std::make_unique<AssistantMessageComponent>(
                theme,
                hide_thinking_block,
                "Thinking...",
                output_pad);
            component->update_content(*assistant);
            item.component = std::move(component);
            return;
        }
        if (const auto* bash = std::get_if<ai::BashExecutionMessage>(&message)) {
            auto component = std::make_unique<BashExecutionComponent>(
                theme,
                keybindings,
                bounded_presentation(bash->command),
                bash->exclude_from_context);
            component->append_output(bounded_presentation(bash->output));
            component->set_complete(
                bash->exit_code,
                bash->cancelled,
                bash->truncated,
                bash->full_output_path);
            component->set_expanded(tools_expanded);
            item.component = std::move(component);
            return;
        }
        if (const auto* custom = std::get_if<ai::CustomMessage>(&message)) {
            if (custom->display) {
                item.component = std::make_unique<CustomMessageComponent>(
                    theme,
                    keybindings,
                    custom->custom_type,
                    custom->content,
                    tools_expanded);
            } else {
                item.component.reset();
            }
            return;
        }
        if (const auto* branch = std::get_if<ai::BranchSummaryMessage>(&message)) {
            item.component = std::make_unique<BranchSummaryComponent>(
                theme,
                keybindings,
                branch->summary,
                tools_expanded);
            return;
        }
        if (const auto* compaction = std::get_if<ai::CompactionSummaryMessage>(&message)) {
            item.component = std::make_unique<CompactionSummaryComponent>(
                theme,
                keybindings,
                compaction->summary,
                compaction->tokens_before,
                tools_expanded);
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
        for (auto& entry : items) {
            auto* message = std::get_if<MessageItem>(&entry);
            if (message == nullptr) continue;
            auto found = std::find(message->tools.begin(), message->tools.end(), tool);
            if (found != message->tools.end()) {
                message->tools.erase(found);
            }
        }
    }

    void settle_provider_tools(const ai::AssistantMessage& assistant) {
        for (const auto& block : assistant.content) {
            const auto* call = std::get_if<ai::ToolCallContent>(&block);
            if (call == nullptr) continue;
            auto& tool = ensure_tool(call->id, call->name, call->raw_arguments);
            if (tool.status != ToolStatus::Pending) continue;
            tool.status = ToolStatus::Failure;
            tool.result = ai::tool_result_message(
                call->id,
                call->name,
                tool_failure_detail(assistant),
                true);
            tool.component->update_result(tool.result);
        }
    }

    void settle_tool(const ai::ToolResultMessage& result) {
        auto& tool = ensure_tool(result.tool_call_id, result.tool_name, {});
        tool.status = result.is_error ? ToolStatus::Failure : ToolStatus::Success;
        tool.result = result;
        tool.component->update_result(result);
    }

    [[nodiscard]] ToolItem& ensure_tool(
        const std::string& call_id,
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
        });
        auto& item = std::get<MessageItem>(items.back());
        auto tool = std::make_unique<ToolItem>(ToolItem{
            .call_id = call_id,
            .name = name,
            .arguments = arguments,
            .component = std::make_unique<ToolExecutionComponent>(
                theme,
                keybindings,
                name,
                call_id,
                arguments),
        });
        tool->component->set_expanded(tools_expanded);
        auto* tool_pointer = tool.get();
        item.tools.push_back(tool_pointer);
        owned_tools.insert_or_assign(call_id, std::move(tool));
        return *tool_pointer;
    }

    [[nodiscard]] std::string tool_failure_detail(const ai::AssistantMessage& assistant) {
        if (assistant.stop_reason == ai::AssistantStopReason::Aborted) {
            if (assistant.error_message &&
                *assistant.error_message != "Request was aborted") {
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
                    static_cast<BashExecutionComponent*>(message->component.get())
                        ->set_expanded(tools_expanded);
                }
            } else if (auto* custom = std::get_if<ai::CustomMessage>(&message->message)) {
                (void)custom;
                if (message->component) {
                    static_cast<BoxedMessageComponent*>(message->component.get())
                        ->set_expanded(tools_expanded);
                }
            } else if (std::holds_alternative<ai::BranchSummaryMessage>(message->message) ||
                std::holds_alternative<ai::CompactionSummaryMessage>(message->message)) {
                if (message->component) {
                    static_cast<BoxedMessageComponent*>(message->component.get())
                        ->set_expanded(tools_expanded);
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
            if (std::holds_alternative<ai::AssistantMessage>(message->message) &&
                message->component) {
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

    [[nodiscard]] util::Expected<cch::tui::RenderResult> render_item(
        const ItemVariant& item,
        std::size_t width,
        std::size_t item_index) {
        (void)item_index;
        if (const auto* message = std::get_if<MessageItem>(&item)) {
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
            auto rendered = render_plain(
                theme, trust_warning->text, width, ThemeToken::Warning);
            if (!rendered) return std::unexpected(rendered.error());
            for (auto& line : rendered->lines) result.lines.push_back(std::move(line));
            return result;
        }
        if (const auto* status = std::get_if<StatusItem>(&item)) {
            // pi showStatus: a Spacer row above the dim status text.
            cch::tui::RenderResult result;
            result.lines.emplace_back();
            auto rendered = render_plain(theme, status->text, width, ThemeToken::Dim);
            if (!rendered) return std::unexpected(rendered.error());
            for (auto& line : rendered->lines) result.lines.push_back(std::move(line));
            return result;
        }
        const auto& diagnostic = std::get<DiagnosticItem>(item);
        if (diagnostic.warning) {
            return render_plain(
                theme,
                "Warning: " + diagnostic.text,
                width,
                ThemeToken::Warning,
                !diagnostic.raw);
        }
        return render_plain(
            theme,
            "Error: " + diagnostic.text,
            width,
            ThemeToken::Error,
            !diagnostic.raw);
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
};

ChatContainer::ChatContainer(
    const LiveTheme& theme,
    std::shared_ptr<const SharedKeybindings> keybindings)
    : impl_(std::make_unique<Impl>(theme, std::move(keybindings))) {}
ChatContainer::ChatContainer(ChatContainer&&) noexcept = default;
ChatContainer& ChatContainer::operator=(ChatContainer&&) noexcept = default;
ChatContainer::~ChatContainer() = default;

void ChatContainer::initialize(const AgentSessionSnapshot& snapshot) {
    impl_->clear();
    for (const auto& message : snapshot.agent_state.messages) {
        impl_->add_message(message);
    }
    if (snapshot.agent_state.streaming_message) {
        impl_->add_message(ai::MessageVariant{*snapshot.agent_state.streaming_message});
    }
}

void ChatContainer::apply_event(const agent::AgentLifecycleEvent& event) {
    if (const auto* start = std::get_if<agent::MessageStartEvent>(&event)) {
        if (std::holds_alternative<ai::AssistantMessage>(start->message)) {
            // Capture the item index before add_message: synchronize_tools
            // may append standalone tool items after it, so the streaming
            // target must not be the last deque entry.
            const auto assistant_index = impl_->items.size();
            impl_->add_message(start->message);
            impl_->active_assistant_item = assistant_index;
        } else {
            impl_->add_message(start->message);
        }
        return;
    }
    if (const auto* update = std::get_if<agent::MessageUpdateEvent>(&event)) {
        if (const auto* assistant = std::get_if<ai::AssistantMessage>(&update->message)) {
            impl_->replace_assistant(*assistant);
        }
        return;
    }
    if (const auto* end = std::get_if<agent::MessageEndEvent>(&event)) {
        if (std::holds_alternative<ai::AssistantMessage>(end->message)) {
            impl_->replace_assistant(std::get<ai::AssistantMessage>(end->message));
            impl_->active_assistant_item.reset();
        } else if (const auto* result = std::get_if<ai::ToolResultMessage>(&end->message)) {
            impl_->settle_tool(*result);
        }
        return;
    }
    if (const auto* start = std::get_if<agent::ToolExecutionStartEvent>(&event)) {
        auto& tool = impl_->ensure_tool(
            start->tool_call_id,
            start->tool_name,
            serialized_arguments(start->args),
            true);
        tool.status = Impl::ToolStatus::Pending;
        return;
    }
    if (const auto* update = std::get_if<agent::ToolExecutionUpdateEvent>(&event)) {
        auto& tool = impl_->ensure_tool(
            update->tool_call_id,
            update->tool_name,
            serialized_arguments(update->args));
        if (tool.status != Impl::ToolStatus::Pending) return;
        tool.component->update_result(ai::ToolResultMessage{
            .tool_call_id = tool.call_id,
            .tool_name = tool.name,
            .content = update->partial_result.content,
            .details = update->partial_result.details,
            .is_error = update->partial_result.is_error,
        }, true);
        return;
    }
    if (const auto* end = std::get_if<agent::ToolExecutionEndEvent>(&event)) {
        auto& tool = impl_->ensure_tool(end->tool_call_id, end->tool_name, {});
        tool.status = end->is_error || end->result.is_error
            ? Impl::ToolStatus::Failure
            : Impl::ToolStatus::Success;
        tool.result = ai::ToolResultMessage{
            .tool_call_id = tool.call_id,
            .tool_name = tool.name,
            .content = end->result.content,
            .details = end->result.details,
            .is_error = end->result.is_error,
        };
        tool.component->update_result(tool.result);
    }
}

void ChatContainer::append_committed_message(ai::MessageVariant message) {
    impl_->add_message(std::move(message));
}

void ChatContainer::clear() {
    impl_->clear();
}

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
}

void ChatContainer::set_hide_thinking_block(bool hide) {
    impl_->hide_thinking_block = hide;
    impl_->apply_thinking_to_components();
}

void ChatContainer::set_output_pad(std::size_t output_pad) {
    if (impl_->output_pad == output_pad) return;
    impl_->output_pad = output_pad;
    impl_->apply_output_pad_to_components();
}

bool ChatContainer::tools_expanded() const {
    return impl_->tools_expanded;
}

util::Expected<cch::tui::RenderResult> ChatContainer::render(std::size_t width) {
    cch::tui::RenderResult result;
    for (std::size_t index = 0; index < impl_->items.size(); ++index) {
        auto rendered = impl_->render_item(impl_->items[index], width, index);
        if (!rendered) return std::unexpected(rendered.error());
        append_render_result(result, std::move(*rendered));
    }
    return result;
}

void ChatContainer::invalidate() {
    for (const auto& item : impl_->items) {
        if (const auto* message = std::get_if<Impl::MessageItem>(&item);
            message != nullptr && message->component) {
            message->component->invalidate();
        }
    }
}

} // namespace cch::coding_agent::tui
