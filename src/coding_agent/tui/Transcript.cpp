#include "Transcript.hpp"

#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Markdown.hpp>
#include <cch/tui/Text.hpp>
#include "coding_agent/BoundedText.hpp"
#include "coding_agent/tui/Theme.hpp"
#include "util/Json.hpp"

#include <algorithm>
#include <deque>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

constexpr std::size_t kCollapsedPayloadBytes = 512;
constexpr std::size_t kCollapsedLogicalLines = 3;

struct ToolItem;

struct MessageItem {
    ai::MessageVariant message;
    std::vector<ToolItem*> tools;
};

enum class ToolStatus { Pending, Success, Failure };

struct ToolItem {
    std::string call_id;
    std::string name;
    std::string arguments;
    ToolStatus status{ToolStatus::Pending};
    std::vector<ai::Content> result;
    bool embedded{false};
};

struct DiagnosticItem {
    std::string text;
};

using TranscriptItemVariant = std::variant<MessageItem, ToolItem, DiagnosticItem>;

[[nodiscard]] std::string safe_text(std::string text) {
    return bounded_redacted_presentation(std::move(text));
}

[[nodiscard]] std::string content_text(const std::vector<ai::Content>& content) {
    std::string text;
    for (const auto& block : content) {
        if (!text.empty()) text.push_back('\n');
        if (const auto* value = std::get_if<ai::TextContent>(&block)) {
            text += value->text;
        } else if (const auto* image = std::get_if<ai::ImageContent>(&block)) {
            text += std::format("[Image: {}]", image->mime_type);
        } else {
            const auto& thinking = std::get<ai::ThinkingContent>(block);
            text += thinking.redacted ? "[Redacted thinking]" : thinking.thinking;
        }
    }
    return safe_text(std::move(text));
}

[[nodiscard]] std::string preserve_markdown_line_breaks(std::string text) {
    std::string rendered;
    rendered.reserve(text.size() + 16);
    bool in_fence = false;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto newline = text.find('\n', start);
        const auto end = newline == std::string::npos ? text.size() : newline;
        const auto line = std::string_view{text}.substr(start, end - start);
        const auto first = line.find_first_not_of(' ');
        const auto marker = first == std::string_view::npos
            ? std::string_view{}
            : line.substr(first);
        const bool fence = first <= 3 &&
            (marker.starts_with("```") || marker.starts_with("~~~"));
        const bool indented_code = first >= 4 && first != std::string_view::npos;

        rendered.append(line);
        if (newline != std::string::npos) {
            const bool already_hard = line.ends_with("  ") || line.ends_with('\\');
            if (!in_fence && !fence && !indented_code && !line.empty() && !already_hard) {
                rendered += "  ";
            }
            rendered.push_back('\n');
        }
        if (fence) in_fence = !in_fence;
        if (newline == std::string::npos) break;
        start = newline + 1;
    }
    return rendered;
}

[[nodiscard]] bool needs_collapsing(std::string_view text) {
    if (text.size() > kCollapsedPayloadBytes) return true;
    return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) >=
        kCollapsedLogicalLines;
}

[[nodiscard]] std::string collapsed_text(std::string text, std::string_view hint) {
    text = safe_text(std::move(text));
    if (!needs_collapsing(text)) return text;

    std::string collapsed;
    std::size_t start = 0;
    for (std::size_t line = 0; line < kCollapsedLogicalLines && start < text.size(); ++line) {
        const auto end = text.find('\n', start);
        const auto count = end == std::string::npos ? text.size() - start : end - start;
        if (!collapsed.empty()) collapsed.push_back('\n');
        collapsed.append(text, start, count);
        if (end == std::string::npos) {
            start = text.size();
            break;
        }
        start = end + 1;
    }
    if (collapsed.size() > kCollapsedPayloadBytes) {
        collapsed = bounded_redacted_presentation(std::move(collapsed), kCollapsedPayloadBytes);
    }
    collapsed += hint.empty()
        ? "\n… (collapsed)"
        : std::format("\n… ({} to expand)", hint);
    return collapsed;
}

[[nodiscard]] util::Expected<std::vector<std::string>> render_markdown(
    const LiveTheme& theme,
    std::string text,
    std::size_t width,
    ThemeToken text_token,
    std::optional<ThemeToken> background = std::nullopt) {
    auto style = theme.markdown_style();
    style.text = theme.foreground_hook(text_token);
    cch::tui::BackgroundHook background_hook;
    if (background) background_hook = theme.background_hook(*background);
    cch::tui::Markdown markdown(
        safe_text(std::move(text)),
        1,
        0,
        std::move(style),
        {},
        std::move(background_hook));
    if (auto rendered = markdown.render(width); !rendered) {
        return std::unexpected(rendered.error());
    } else {
        return std::move(rendered->lines);
    }
}

[[nodiscard]] util::Expected<std::vector<std::string>> render_plain(
    const LiveTheme& theme,
    std::string text,
    std::size_t width,
    ThemeToken text_token,
    std::optional<ThemeToken> background = std::nullopt) {
    cch::tui::BackgroundHook background_hook;
    if (background) background_hook = theme.background_hook(*background);
    cch::tui::Text component(
        theme.foreground(text_token, safe_text(std::move(text))),
        1,
        0,
        std::move(background_hook));
    if (auto rendered = component.render(width); !rendered) {
        return std::unexpected(rendered.error());
    } else {
        return std::move(rendered->lines);
    }
}

void append_lines(std::vector<std::string>& destination, std::vector<std::string> lines) {
    destination.insert(
        destination.end(),
        std::make_move_iterator(lines.begin()),
        std::make_move_iterator(lines.end()));
}

[[nodiscard]] std::string provider_outcome(const ai::AssistantMessage& message) {
    const bool has_tool_calls = std::ranges::any_of(
        message.content,
        [](const ai::AssistantContent& block) {
            return std::holds_alternative<ai::ToolCallContent>(block);
        });
    if (has_tool_calls) return {};
    if (message.stop_reason == ai::AssistantStopReason::Error) {
        return std::format(
            "Provider error: {}",
            message.error_message.value_or("Unknown error"));
    }
    if (message.stop_reason == ai::AssistantStopReason::Aborted) {
        const auto detail = message.error_message && *message.error_message != "Request was aborted"
            ? *message.error_message
            : "Operation aborted";
        return std::format("Provider aborted: {}", detail);
    }
    if (message.stop_reason == ai::AssistantStopReason::Length) {
        return "Provider stopped: output token limit reached";
    }
    return {};
}

[[nodiscard]] std::string tool_failure_detail(const ai::AssistantMessage& message) {
    if (message.stop_reason == ai::AssistantStopReason::Aborted) {
        if (message.error_message && *message.error_message != "Request was aborted") {
            return *message.error_message;
        }
        return "Operation aborted";
    }
    return message.error_message.value_or("Provider ended before tool execution");
}

[[nodiscard]] std::string serialized_arguments(const util::JsonValue& arguments) {
    if (auto serialized = util::write_json(arguments); serialized) {
        return safe_text(std::move(*serialized));
    }
    return "{}";
}

} // namespace

struct Transcript::Impl {
    Impl(
        const LiveTheme& configured_theme,
        const cch::tui::KeybindingRegistry& configured_keybindings)
        : theme(configured_theme), keybindings(configured_keybindings) {}

    [[nodiscard]] std::string key_hint(std::string_view action_id) const {
        auto text = keybindings.key_text(action_id);
        return text.empty() ? "Unbound" : text;
    }

    void clear() {
        items.clear();
        tool_items.clear();
        active_assistant_item.reset();
    }

    [[nodiscard]] ToolItem& ensure_tool(
        std::string call_id,
        std::string name,
        std::string arguments,
        bool begin_call = false) {
        if (const auto found = tool_items.find(call_id); found != tool_items.end()) {
            auto& item = std::get<ToolItem>(items[found->second]);
            if (!begin_call || item.status == ToolStatus::Pending) {
                if (!name.empty()) item.name = std::move(name);
                if (!arguments.empty()) item.arguments = std::move(arguments);
                return item;
            }
        }

        items.emplace_back(ToolItem{
            .call_id = std::move(call_id),
            .name = std::move(name),
            .arguments = std::move(arguments),
            .status = ToolStatus::Pending,
            .result = {},
            .embedded = false,
        });
        const auto index = items.size() - 1;
        auto& item = std::get<ToolItem>(items[index]);
        tool_items.insert_or_assign(item.call_id, index);
        return item;
    }

    void synchronize_tools(MessageItem& item, const ai::AssistantMessage& assistant) {
        std::size_t position = 0;
        for (const auto& block : assistant.content) {
            const auto* call = std::get_if<ai::ToolCallContent>(&block);
            if (call == nullptr || call->id.empty()) continue;
            ToolItem* tool = nullptr;
            if (position < item.tools.size()) {
                tool = item.tools[position];
                tool->name = call->name;
                tool->arguments = call->raw_arguments;
            } else {
                tool = &ensure_tool(call->id, call->name, call->raw_arguments, true);
                item.tools.push_back(tool);
            }
            tool->embedded = true;
            ++position;
        }
    }

    void settle_provider_tools(const ai::AssistantMessage& assistant) {
        if (assistant.stop_reason != ai::AssistantStopReason::Error &&
            assistant.stop_reason != ai::AssistantStopReason::Aborted) {
            return;
        }
        for (const auto& block : assistant.content) {
            const auto* call = std::get_if<ai::ToolCallContent>(&block);
            if (call == nullptr || call->id.empty()) continue;
            auto& tool = ensure_tool(call->id, call->name, call->raw_arguments);
            if (tool.status != ToolStatus::Pending) continue;
            tool.status = ToolStatus::Failure;
            tool.result = {ai::text_content(tool_failure_detail(assistant))};
        }
    }

    void settle_tool(const ai::ToolResultMessage& result) {
        auto& tool = ensure_tool(result.tool_call_id, result.tool_name, {});
        tool.status = result.is_error ? ToolStatus::Failure : ToolStatus::Success;
        tool.result = result.content;
    }

    void add_message(ai::MessageVariant message, bool active_assistant = false) {
        if (const auto* result = std::get_if<ai::ToolResultMessage>(&message)) {
            settle_tool(*result);
            return;
        }
        if (const auto* custom = std::get_if<ai::CustomMessage>(&message);
            custom != nullptr && !custom->display) {
            return;
        }

        items.emplace_back(MessageItem{
            .message = std::move(message),
            .tools = {},
        });
        const auto index = items.size() - 1;
        auto& item = std::get<MessageItem>(items[index]);
        const auto* assistant = std::get_if<ai::AssistantMessage>(&item.message);
        if (assistant != nullptr) {
            synchronize_tools(item, *assistant);
            settle_provider_tools(*assistant);
            if (active_assistant) active_assistant_item = index;
        }
    }

    void replace_assistant(const ai::MessageVariant& message, bool finalize) {
        const auto* assistant = std::get_if<ai::AssistantMessage>(&message);
        if (assistant == nullptr) return;
        if (!active_assistant_item || *active_assistant_item >= items.size()) {
            add_message(message, !finalize);
            if (finalize) active_assistant_item.reset();
            return;
        }

        auto* item = std::get_if<MessageItem>(&items[*active_assistant_item]);
        if (item == nullptr || !std::holds_alternative<ai::AssistantMessage>(item->message)) {
            add_message(message, !finalize);
        } else {
            item->message = message;
            synchronize_tools(*item, *assistant);
            settle_provider_tools(*assistant);
        }
        if (finalize) active_assistant_item.reset();
    }

    [[nodiscard]] util::Expected<std::vector<std::string>> render_message(
        const MessageItem& message_item,
        std::size_t width) const {
        const auto& message = message_item.message;
        if (const auto* system = std::get_if<ai::SystemMessage>(&message)) {
            return render_markdown(theme, "**System:** " + system->content, width, ThemeToken::Dim);
        }
        if (const auto* user = std::get_if<ai::UserMessage>(&message)) {
            return render_markdown(
                theme,
                "**You:** " + content_text(user->content),
                width,
                ThemeToken::UserMessageText,
                ThemeToken::UserMessageBg);
        }
        if (const auto* assistant = std::get_if<ai::AssistantMessage>(&message)) {
            std::vector<std::string> lines;
            std::size_t tool_position = 0;
            for (const auto& block : assistant->content) {
                if (const auto* text = std::get_if<ai::TextContent>(&block);
                    text != nullptr && !text->text.empty()) {
                    if (auto rendered = render_markdown(
                            theme,
                            "**Assistant:** " + preserve_markdown_line_breaks(text->text),
                            width,
                            ThemeToken::Text);
                        !rendered) {
                        return std::unexpected(rendered.error());
                    } else {
                        append_lines(lines, std::move(*rendered));
                    }
                } else if (const auto* thinking = std::get_if<ai::ThinkingContent>(&block);
                    thinking != nullptr && (thinking->redacted || !thinking->thinking.empty())) {
                    const auto thinking_key = key_hint("app.thinking.toggle");
                    const auto body = thinking->redacted
                        ? "[Redacted thinking]"
                        : thinking_expanded
                            ? safe_text(thinking->thinking)
                            : std::format(
                                "(collapsed; {} to expand)",
                                thinking_key);
                    if (auto rendered = render_markdown(
                            theme,
                            "**Thinking:** " + preserve_markdown_line_breaks(body),
                            width,
                            ThemeToken::ThinkingText);
                        !rendered) {
                        return std::unexpected(rendered.error());
                    } else {
                        append_lines(lines, std::move(*rendered));
                    }
                } else if (std::holds_alternative<ai::ToolCallContent>(block) &&
                    tool_position < message_item.tools.size()) {
                    if (auto rendered = render_tool(
                            *message_item.tools[tool_position++],
                            width);
                        !rendered) {
                        return std::unexpected(rendered.error());
                    } else {
                        append_lines(lines, std::move(*rendered));
                    }
                }
            }
            const auto outcome = provider_outcome(*assistant);
            if (!outcome.empty()) {
                if (auto rendered = render_plain(theme, outcome, width, ThemeToken::Error);
                    !rendered) {
                    return std::unexpected(rendered.error());
                } else {
                    append_lines(lines, std::move(*rendered));
                }
            }
            return lines;
        }
        if (const auto* bash = std::get_if<ai::BashExecutionMessage>(&message)) {
            auto output = tools_expanded
                ? safe_text(bash->output)
                : collapsed_text(bash->output, key_hint("app.tools.expand"));
            return render_plain(
                theme,
                std::format(
                    "Bash {}: {}\n{}",
                    bash->cancelled || (bash->exit_code && *bash->exit_code != 0)
                        ? "failed"
                        : "success",
                    bash->command,
                    output),
                width,
                ThemeToken::ToolOutput,
                bash->cancelled || (bash->exit_code && *bash->exit_code != 0)
                    ? ThemeToken::ToolErrorBg
                    : ThemeToken::ToolSuccessBg);
        }
        if (const auto* custom = std::get_if<ai::CustomMessage>(&message)) {
            return render_markdown(
                theme,
                std::format(
                    "**Custom {}:** {}",
                    custom->custom_type,
                    content_text(custom->content)),
                width,
                ThemeToken::CustomMessageText,
                ThemeToken::CustomMessageBg);
        }
        if (const auto* branch = std::get_if<ai::BranchSummaryMessage>(&message)) {
            auto summary = tools_expanded
                ? safe_text(branch->summary)
                : collapsed_text(branch->summary, key_hint("app.tools.expand"));
            return render_markdown(
                theme,
                "**Branch summary:** " + summary,
                width,
                ThemeToken::CustomMessageText,
                ThemeToken::CustomMessageBg);
        }
        const auto& compaction = std::get<ai::CompactionSummaryMessage>(message);
        auto summary = tools_expanded
            ? safe_text(compaction.summary)
            : collapsed_text(compaction.summary, key_hint("app.tools.expand"));
        return render_markdown(
            theme,
            std::format(
                "**Compaction summary:** {} tokens\n{}",
                compaction.tokens_before,
                summary),
            width,
            ThemeToken::CustomMessageText,
            ThemeToken::CustomMessageBg);
    }

    [[nodiscard]] util::Expected<std::vector<std::string>> render_tool(
        const ToolItem& item,
        std::size_t width) const {
        const auto status = item.status == ToolStatus::Pending
            ? "pending"
            : item.status == ToolStatus::Success ? "success" : "failed";
        const auto background = item.status == ToolStatus::Pending
            ? ThemeToken::ToolPendingBg
            : item.status == ToolStatus::Success
                ? ThemeToken::ToolSuccessBg
                : ThemeToken::ToolErrorBg;
        std::string text = std::format("Tool {}: {}#{}", status, item.name, item.call_id);
        if (!item.arguments.empty()) {
            const auto arguments = tools_expanded
                ? safe_text(item.arguments)
                : collapsed_text(item.arguments, key_hint("app.tools.expand"));
            text += "\nTool arguments: " + arguments;
        }
        if (!item.result.empty()) {
            auto result = content_text(item.result);
            if (!tools_expanded) {
                result = collapsed_text(
                    std::move(result),
                    key_hint("app.tools.expand"));
            }
            text += "\nTool result: " + result;
        }
        return render_plain(theme, std::move(text), width, ThemeToken::ToolOutput, background);
    }

    [[nodiscard]] util::Expected<std::vector<std::string>> render_item(
        const TranscriptItemVariant& item,
        std::size_t width) const {
        if (const auto* message = std::get_if<MessageItem>(&item)) {
            return render_message(*message, width);
        }
        if (const auto* tool = std::get_if<ToolItem>(&item)) {
            if (tool->embedded) return std::vector<std::string>{};
            return render_tool(*tool, width);
        }
        return render_plain(
            theme,
            "Error: " + std::get<DiagnosticItem>(item).text,
            width,
            ThemeToken::Error);
    }

    const LiveTheme& theme; // must outlive this presentation reducer.
    const cch::tui::KeybindingRegistry& keybindings; // must outlive this presentation reducer.
    std::deque<TranscriptItemVariant> items;
    std::unordered_map<std::string, std::size_t> tool_items;
    std::optional<std::size_t> active_assistant_item;
    bool tools_expanded{false};
    bool thinking_expanded{true};
};

Transcript::Transcript(
    const LiveTheme& theme,
    const cch::tui::KeybindingRegistry& keybindings)
    : impl_(std::make_unique<Impl>(theme, keybindings)) {}
Transcript::Transcript(Transcript&&) noexcept = default;
Transcript& Transcript::operator=(Transcript&&) noexcept = default;
Transcript::~Transcript() = default;

void Transcript::initialize(const AgentSessionSnapshot& snapshot) {
    impl_->clear();
    for (const auto& message : snapshot.agent_state.messages) {
        impl_->add_message(message);
    }
    if (snapshot.agent_state.streaming_message) {
        impl_->add_message(ai::MessageVariant{*snapshot.agent_state.streaming_message}, true);
    }
}

void Transcript::apply_event(const agent::AgentLifecycleEvent& event) {
    if (const auto* start = std::get_if<agent::MessageStartEvent>(&event)) {
        if (std::holds_alternative<ai::AssistantMessage>(start->message)) {
            impl_->add_message(start->message, true);
        } else {
            impl_->add_message(start->message);
        }
        return;
    }
    if (const auto* update = std::get_if<agent::MessageUpdateEvent>(&event)) {
        impl_->replace_assistant(update->message, false);
        return;
    }
    if (const auto* end = std::get_if<agent::MessageEndEvent>(&event)) {
        if (std::holds_alternative<ai::AssistantMessage>(end->message)) {
            impl_->replace_assistant(end->message, true);
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
        tool.status = ToolStatus::Pending;
        return;
    }
    if (const auto* update = std::get_if<agent::ToolExecutionUpdateEvent>(&event)) {
        auto& tool = impl_->ensure_tool(
            update->tool_call_id,
            update->tool_name,
            serialized_arguments(update->args));
        if (tool.status != ToolStatus::Pending) return;
        tool.result = update->partial_result.content;
        return;
    }
    if (const auto* end = std::get_if<agent::ToolExecutionEndEvent>(&event)) {
        auto& tool = impl_->ensure_tool(end->tool_call_id, end->tool_name, {});
        tool.status = end->is_error || end->result.is_error
            ? ToolStatus::Failure
            : ToolStatus::Success;
        tool.result = end->result.content;
    }
}

void Transcript::append_diagnostic(std::string text) {
    impl_->items.emplace_back(DiagnosticItem{safe_text(std::move(text))});
}

void Transcript::toggle_tool_output() {
    impl_->tools_expanded = !impl_->tools_expanded;
}

void Transcript::toggle_thinking() {
    impl_->thinking_expanded = !impl_->thinking_expanded;
}

util::Expected<std::vector<std::string>> Transcript::render(std::size_t width) const {
    std::vector<std::string> lines;
    for (const auto& item : impl_->items) {
        if (auto rendered = impl_->render_item(item, width); !rendered) {
            return std::unexpected(rendered.error());
        } else {
            append_lines(lines, std::move(*rendered));
        }
    }
    return lines;
}

} // namespace cch::coding_agent::tui
