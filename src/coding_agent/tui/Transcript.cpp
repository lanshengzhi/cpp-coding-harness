#include "Transcript.hpp"

#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/tui/Image.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Markdown.hpp>
#include <cch/tui/Text.hpp>
#include "coding_agent/BoundedText.hpp"
#include "coding_agent/tui/BashBlock.hpp"
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

struct FrontendItem {
    std::string text;
};

struct DiagnosticItem {
    std::string text;
};

using TranscriptItemVariant = std::variant<MessageItem, ToolItem, FrontendItem, DiagnosticItem>;

[[nodiscard]] std::string safe_text(std::string text) {
    return bounded_redacted_presentation(std::move(text));
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

void append_render_result(
    cch::tui::RenderResult& destination,
    cch::tui::RenderResult source) {
    const auto row_offset = destination.lines.size();
    for (auto& image : source.images) {
        image.region.row += row_offset;
        destination.images.push_back(std::move(image));
    }
    append_lines(destination.lines, std::move(source.lines));
}

[[nodiscard]] cch::tui::RenderResult lines_result(std::vector<std::string> lines) {
    return cch::tui::RenderResult{.lines = std::move(lines), .images = {}};
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
    struct ImageSlot {
        std::unique_ptr<cch::tui::Image> component;
        std::string data;
        std::string mime_type;
    };

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
        image_slots.clear();
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

    [[nodiscard]] util::Expected<cch::tui::RenderResult> render_image(
        std::string key,
        const ai::ImageContent& image,
        std::size_t width,
        ThemeToken fallback_token) const {
        auto found = image_slots.find(key);
        if (found == image_slots.end()) {
            auto component = std::make_unique<cch::tui::Image>(
                cch::tui::ImageContent{
                    .encoded_data = image.data,
                    .mime_type = image.mime_type,
                    .filename = std::nullopt,
                },
                cch::tui::ImageOptions{
                    .constraints = {
                        .max_width = 60,
                        .max_height = std::nullopt,
                    },
                    .fallback_style = theme.foreground_hook(fallback_token),
                });
            auto slot = std::make_unique<ImageSlot>(ImageSlot{
                .component = std::move(component),
                .data = image.data,
                .mime_type = image.mime_type,
            });
            found = image_slots.emplace(std::move(key), std::move(slot)).first;
        } else if (found->second->data != image.data ||
            found->second->mime_type != image.mime_type) {
            found->second->component->set_content(cch::tui::ImageContent{
                .encoded_data = image.data,
                .mime_type = image.mime_type,
                .filename = std::nullopt,
            });
            found->second->data = image.data;
            found->second->mime_type = image.mime_type;
        }
        return found->second->component->render(width);
    }

    [[nodiscard]] util::Expected<cch::tui::RenderResult> render_content(
        const std::vector<ai::Content>& content,
        std::string label,
        std::size_t width,
        ThemeToken text_token,
        std::optional<ThemeToken> background,
        const std::string& key_prefix) const {
        cch::tui::RenderResult result;
        bool label_rendered = false;
        for (std::size_t index = 0; index < content.size(); ++index) {
            const auto& block = content[index];
            if (const auto* image = std::get_if<ai::ImageContent>(&block)) {
                if (!label_rendered) {
                    auto rendered_label = render_markdown(
                        theme, label, width, text_token, background);
                    if (!rendered_label) return std::unexpected(rendered_label.error());
                    append_lines(result.lines, std::move(*rendered_label));
                    label_rendered = true;
                }
                auto rendered_image = render_image(
                    std::format("{}/image/{}", key_prefix, index),
                    *image,
                    width,
                    text_token);
                if (!rendered_image) return std::unexpected(rendered_image.error());
                append_render_result(result, std::move(*rendered_image));
                continue;
            }

            std::string text;
            if (const auto* value = std::get_if<ai::TextContent>(&block)) {
                text = value->text;
            } else {
                const auto& thinking = std::get<ai::ThinkingContent>(block);
                text = thinking.redacted ? "[Redacted thinking]" : thinking.thinking;
            }
            if (text.empty()) continue;
            if (!label_rendered) {
                text = label + " " + text;
                label_rendered = true;
            }
            auto rendered = render_markdown(
                theme,
                preserve_markdown_line_breaks(std::move(text)),
                width,
                text_token,
                background);
            if (!rendered) return std::unexpected(rendered.error());
            append_lines(result.lines, std::move(*rendered));
        }
        if (!label_rendered) {
            auto rendered = render_markdown(theme, std::move(label), width, text_token, background);
            if (!rendered) return std::unexpected(rendered.error());
            append_lines(result.lines, std::move(*rendered));
        }
        return result;
    }

    [[nodiscard]] util::Expected<cch::tui::RenderResult> render_message(
        const MessageItem& message_item,
        std::size_t width,
        const std::string& key_prefix) const {
        const auto& message = message_item.message;
        if (const auto* system = std::get_if<ai::SystemMessage>(&message)) {
            auto rendered = render_markdown(
                theme, "**System:** " + system->content, width, ThemeToken::Dim);
            if (!rendered) return std::unexpected(rendered.error());
            return lines_result(std::move(*rendered));
        }
        if (const auto* user = std::get_if<ai::UserMessage>(&message)) {
            return render_content(
                user->content,
                "**You:**",
                width,
                ThemeToken::UserMessageText,
                ThemeToken::UserMessageBg,
                key_prefix);
        }
        if (const auto* assistant = std::get_if<ai::AssistantMessage>(&message)) {
            cch::tui::RenderResult result;
            std::size_t tool_position = 0;
            for (const auto& block : assistant->content) {
                if (const auto* text = std::get_if<ai::TextContent>(&block);
                    text != nullptr && !text->text.empty()) {
                    auto rendered = render_markdown(
                        theme,
                        "**Assistant:** " + preserve_markdown_line_breaks(text->text),
                        width,
                        ThemeToken::Text);
                    if (!rendered) return std::unexpected(rendered.error());
                    append_lines(result.lines, std::move(*rendered));
                } else if (const auto* thinking = std::get_if<ai::ThinkingContent>(&block);
                    thinking != nullptr && (thinking->redacted || !thinking->thinking.empty())) {
                    const auto thinking_key = key_hint("app.thinking.toggle");
                    const auto body = thinking->redacted
                        ? "[Redacted thinking]"
                        : thinking_expanded
                            ? safe_text(thinking->thinking)
                            : std::format("(collapsed; {} to expand)", thinking_key);
                    auto rendered = render_markdown(
                        theme,
                        "**Thinking:** " + preserve_markdown_line_breaks(body),
                        width,
                        ThemeToken::ThinkingText);
                    if (!rendered) return std::unexpected(rendered.error());
                    append_lines(result.lines, std::move(*rendered));
                } else if (std::holds_alternative<ai::ToolCallContent>(block) &&
                    tool_position < message_item.tools.size()) {
                    auto rendered = render_tool(
                        *message_item.tools[tool_position++],
                        width,
                        key_prefix + "/tool");
                    if (!rendered) return std::unexpected(rendered.error());
                    append_render_result(result, std::move(*rendered));
                }
            }
            const auto outcome = provider_outcome(*assistant);
            if (!outcome.empty()) {
                auto rendered = render_plain(theme, outcome, width, ThemeToken::Error);
                if (!rendered) return std::unexpected(rendered.error());
                append_lines(result.lines, std::move(*rendered));
            }
            return result;
        }
        if (const auto* bash = std::get_if<ai::BashExecutionMessage>(&message)) {
            auto rendered = render_bash_block(
                theme,
                keybindings,
                BashBlockView{
                    .command = bash->command,
                    .output = bash->output,
                    .exclude_from_context = bash->exclude_from_context,
                    .running = false,
                    .exit_code = bash->exit_code,
                    .cancelled = bash->cancelled,
                    .truncated = bash->truncated,
                    .full_output_path = bash->full_output_path,
                },
                tools_expanded,
                width);
            if (!rendered) return std::unexpected(rendered.error());
            return lines_result(std::move(*rendered));
        }
        if (const auto* custom = std::get_if<ai::CustomMessage>(&message)) {
            return render_content(
                custom->content,
                std::format("**Custom {}:**", custom->custom_type),
                width,
                ThemeToken::CustomMessageText,
                ThemeToken::CustomMessageBg,
                key_prefix);
        }
        if (const auto* branch = std::get_if<ai::BranchSummaryMessage>(&message)) {
            auto summary = tools_expanded
                ? safe_text(branch->summary)
                : collapsed_text(branch->summary, key_hint("app.tools.expand"));
            auto rendered = render_markdown(
                theme,
                "**Branch summary:** " + summary,
                width,
                ThemeToken::CustomMessageText,
                ThemeToken::CustomMessageBg);
            if (!rendered) return std::unexpected(rendered.error());
            return lines_result(std::move(*rendered));
        }
        const auto& compaction = std::get<ai::CompactionSummaryMessage>(message);
        auto summary = tools_expanded
            ? safe_text(compaction.summary)
            : collapsed_text(compaction.summary, key_hint("app.tools.expand"));
        auto rendered = render_markdown(
            theme,
            std::format(
                "**Compaction summary:** {} tokens\n{}",
                compaction.tokens_before,
                summary),
            width,
            ThemeToken::CustomMessageText,
            ThemeToken::CustomMessageBg);
        if (!rendered) return std::unexpected(rendered.error());
        return lines_result(std::move(*rendered));
    }

    [[nodiscard]] util::Expected<cch::tui::RenderResult> render_tool(
        const ToolItem& item,
        std::size_t width,
        const std::string& key_prefix) const {
        const auto status = item.status == ToolStatus::Pending
            ? "pending"
            : item.status == ToolStatus::Success ? "success" : "failed";
        const auto background = item.status == ToolStatus::Pending
            ? ThemeToken::ToolPendingBg
            : item.status == ToolStatus::Success
                ? ThemeToken::ToolSuccessBg
                : ThemeToken::ToolErrorBg;
        std::string header = std::format("Tool {}: {}#{}", status, item.name, item.call_id);
        if (!item.arguments.empty()) {
            const auto arguments = tools_expanded
                ? safe_text(item.arguments)
                : collapsed_text(item.arguments, key_hint("app.tools.expand"));
            header += "\nTool arguments: " + arguments;
        }

        auto rendered_header = render_plain(
            theme, std::move(header), width, ThemeToken::ToolOutput, background);
        if (!rendered_header) return std::unexpected(rendered_header.error());
        cch::tui::RenderResult result = lines_result(std::move(*rendered_header));
        bool result_label_rendered = false;
        for (std::size_t index = 0; index < item.result.size(); ++index) {
            const auto& block = item.result[index];
            if (const auto* image = std::get_if<ai::ImageContent>(&block)) {
                if (!result_label_rendered) {
                    auto label = render_plain(
                        theme, "Tool result:", width, ThemeToken::ToolOutput, background);
                    if (!label) return std::unexpected(label.error());
                    append_lines(result.lines, std::move(*label));
                    result_label_rendered = true;
                }
                auto rendered_image = render_image(
                    std::format("{}/{}/image/{}", key_prefix, item.call_id, index),
                    *image,
                    width,
                    ThemeToken::ToolOutput);
                if (!rendered_image) return std::unexpected(rendered_image.error());
                append_render_result(result, std::move(*rendered_image));
                continue;
            }

            std::string text;
            if (const auto* value = std::get_if<ai::TextContent>(&block)) {
                text = value->text;
            } else {
                const auto& thinking = std::get<ai::ThinkingContent>(block);
                text = thinking.redacted ? "[Redacted thinking]" : thinking.thinking;
            }
            if (!tools_expanded) {
                text = collapsed_text(std::move(text), key_hint("app.tools.expand"));
            }
            if (!result_label_rendered) {
                text = "Tool result: " + text;
                result_label_rendered = true;
            }
            auto rendered = render_plain(
                theme, std::move(text), width, ThemeToken::ToolOutput, background);
            if (!rendered) return std::unexpected(rendered.error());
            append_lines(result.lines, std::move(*rendered));
        }
        return result;
    }

    [[nodiscard]] util::Expected<cch::tui::RenderResult> render_item(
        const TranscriptItemVariant& item,
        std::size_t width,
        std::size_t item_index) const {
        const auto key_prefix = std::format("item/{}", item_index);
        if (const auto* message = std::get_if<MessageItem>(&item)) {
            return render_message(*message, width, key_prefix);
        }
        if (const auto* tool = std::get_if<ToolItem>(&item)) {
            if (tool->embedded) return cch::tui::RenderResult{};
            return render_tool(*tool, width, key_prefix + "/tool");
        }
        if (const auto* frontend = std::get_if<FrontendItem>(&item)) {
            auto rendered = render_plain(theme, frontend->text, width, ThemeToken::Text);
            if (!rendered) return std::unexpected(rendered.error());
            return lines_result(std::move(*rendered));
        }
        auto rendered = render_plain(
            theme,
            "Error: " + std::get<DiagnosticItem>(item).text,
            width,
            ThemeToken::Error);
        if (!rendered) return std::unexpected(rendered.error());
        return lines_result(std::move(*rendered));
    }

    const LiveTheme& theme; // must outlive this presentation reducer.
    const cch::tui::KeybindingRegistry& keybindings; // must outlive this presentation reducer.
    std::deque<TranscriptItemVariant> items;
    std::unordered_map<std::string, std::size_t> tool_items;
    mutable std::unordered_map<std::string, std::unique_ptr<ImageSlot>> image_slots;
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

void Transcript::append_committed_message(ai::MessageVariant message) {
    impl_->add_message(std::move(message));
}

void Transcript::clear() {
    impl_->clear();
}

void Transcript::append_frontend_message(std::string text) {
    if (!text.empty()) {
        impl_->items.emplace_back(FrontendItem{safe_text(std::move(text))});
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

bool Transcript::tools_expanded() const {
    return impl_->tools_expanded;
}

util::Expected<cch::tui::RenderResult> Transcript::render(std::size_t width) const {
    cch::tui::RenderResult result;
    for (std::size_t index = 0; index < impl_->items.size(); ++index) {
        auto rendered = impl_->render_item(impl_->items[index], width, index);
        if (!rendered) return std::unexpected(rendered.error());
        append_render_result(result, std::move(*rendered));
    }
    return result;
}

} // namespace cch::coding_agent::tui
