#include "ToolExecutionComponent.hpp"

#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Utils.hpp>
#include "coding_agent/BoundedText.hpp"
#include "coding_agent/tui/DiffRenderer.hpp"
#include "coding_agent/tui/Theme.hpp"
#include "util/Json.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

constexpr std::size_t kCollapsedPayloadBytes = 2048;
constexpr std::size_t kCollapsedLogicalLines = 5;

[[nodiscard]] std::string safe_text(std::string text) {
    return bounded_redacted_presentation(std::move(text));
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

[[nodiscard]] std::string bold_tool_title(const LiveTheme& theme, std::string_view text) {
    return theme.foreground_hook(ThemeToken::ToolTitle)(
        std::format("\x1b[1m{}\x1b[22m", text));
}

/// Extracts the `path`/`file_path` argument for tool titles.
[[nodiscard]] std::optional<std::string> argument_path(std::string_view arguments_json) {
    auto parsed = util::read_json<glz::generic>(arguments_json);
    if (!parsed) return std::nullopt;
    auto value = util::json_from_glaze(std::move(*parsed));
    const auto* object = value.get_if<util::JsonValue::object_t>();
    if (object == nullptr) return std::nullopt;
    const auto find_text = [&](std::string_view key) -> std::optional<std::string> {
        const auto found = object->find(std::string{key});
        if (found == object->end()) return std::nullopt;
        const auto* text = found->second.get_if<std::string>();
        if (text == nullptr) return std::nullopt;
        return *text;
    };
    if (auto path = find_text("path")) return path;
    return find_text("file_path");
}

/// Extracts the `command` argument for the bash tool title.
[[nodiscard]] std::optional<std::string> argument_command(std::string_view arguments_json) {
    auto parsed = util::read_json<glz::generic>(arguments_json);
    if (!parsed) return std::nullopt;
    auto value = util::json_from_glaze(std::move(*parsed));
    const auto* object = value.get_if<util::JsonValue::object_t>();
    if (object == nullptr) return std::nullopt;
    const auto found = object->find("command");
    if (found == object->end()) return std::nullopt;
    const auto* text = found->second.get_if<std::string>();
    if (text == nullptr) return std::nullopt;
    return *text;
}

[[nodiscard]] std::string result_error_text(const ai::ToolResultMessage& result) {
    std::string text;
    for (const auto& block : result.content) {
        if (const auto* value = std::get_if<ai::TextContent>(&block)) {
            if (!text.empty()) text.push_back('\n');
            text += value->text;
        }
    }
    return text;
}

[[nodiscard]] std::string result_text_content(const ai::ToolResultMessage& result) {
    std::string text;
    for (const auto& block : result.content) {
        if (const auto* value = std::get_if<ai::TextContent>(&block)) {
            if (!text.empty()) text.push_back('\n');
            text += value->text;
        }
    }
    return text;
}

[[nodiscard]] std::optional<std::string> result_diff(const ai::ToolResultMessage& result) {
    if (!result.details) return std::nullopt;
    const auto* object = result.details->get_if<util::JsonValue::object_t>();
    if (object == nullptr) return std::nullopt;
    const auto found = object->find("diff");
    if (found == object->end()) return std::nullopt;
    const auto* text = found->second.get_if<std::string>();
    if (text == nullptr) return std::nullopt;
    return *text;
}

} // namespace

struct ToolExecutionComponent::ImageSlot {
    std::unique_ptr<cch::tui::Image> component;
    std::string data;
    std::string mime_type;
};

ToolExecutionComponent::ToolExecutionComponent(
    const LiveTheme& theme,
    std::shared_ptr<const SharedKeybindings> keybindings,
    std::string tool_name,
    std::string tool_call_id,
    std::string arguments_json)
    : theme_(theme),
      keybindings_(std::move(keybindings)),
      tool_name_(std::move(tool_name)),
      tool_call_id_(std::move(tool_call_id)),
      arguments_json_(safe_text(std::move(arguments_json))),
      box_(1, 1, theme.background_hook(ThemeToken::ToolPendingBg)) {
    rebuild();
}

ToolExecutionComponent::~ToolExecutionComponent() = default;

void ToolExecutionComponent::update_args(std::string arguments_json) {
    arguments_json_ = safe_text(std::move(arguments_json));
    rebuild();
}

void ToolExecutionComponent::update_result(
    ai::ToolResultMessage result,
    bool is_partial) {
    result_ = std::move(result);
    is_partial_ = is_partial;
    rebuild();
}

void ToolExecutionComponent::set_expanded(bool expanded) {
    expanded_ = expanded;
    rebuild();
}

void ToolExecutionComponent::rebuild() {
    box_.clear();
    box_.set_background_hook(
        is_partial_
            ? theme_.background_hook(ThemeToken::ToolPendingBg)
            : (result_ && result_->is_error)
                ? theme_.background_hook(ThemeToken::ToolErrorBg)
                : theme_.background_hook(ThemeToken::ToolSuccessBg));

    // Title (pi tool-execution.ts: bold toolTitle).
    std::string title;
    if (tool_name_ == "edit") {
        title = "edit";
        if (auto path = argument_path(arguments_json_)) {
            title += " " + *path;
        }
    } else if (tool_name_ == "read" || tool_name_ == "write") {
        title = tool_name_;
        if (auto path = argument_path(arguments_json_)) {
            title += " " + *path;
        }
    } else if (tool_name_ == "bash") {
        title = "$";
        if (auto command = argument_command(arguments_json_)) {
            title += " " + *command;
        }
    } else {
        title = tool_name_;
    }
    (void)box_.add_child(std::make_unique<cch::tui::Text>(bold_tool_title(theme_, title), 0, 0));

    // Arguments preview.
    if (!arguments_json_.empty()) {
        const auto expand_hint = keybindings_->registry().key_text("app.tools.expand");
        const auto hint = expand_hint.empty() ? "Unbound" : expand_hint;
        const auto preview = expanded_
            ? safe_text(arguments_json_)
            : collapsed_text(arguments_json_, hint);
        (void)box_.add_child(std::make_unique<cch::tui::Text>(
            theme_.foreground(ThemeToken::ToolOutput, std::format("\n{}", preview)),
            0,
            0));
    }

    if (!result_) return;

    // Edit results render through the diff renderer (pi edit.ts renderResult).
    if (tool_name_ == "edit" && !result_->is_error) {
        if (auto diff = result_diff(*result_)) {
            (void)box_.add_child(std::make_unique<cch::tui::Text>(
                std::format("\n{}", render_diff(theme_, *diff)),
                0,
                0));
        }
    } else if (result_->is_error) {
        const auto error = result_error_text(*result_);
        if (!error.empty()) {
            (void)box_.add_child(std::make_unique<cch::tui::Text>(
                theme_.foreground(ThemeToken::Error, std::format("\n{}", safe_text(error))),
                0,
                0));
        }
    } else {
        const auto output = result_text_content(*result_);
        if (!output.empty()) {
            const auto expand_hint = keybindings_->registry().key_text("app.tools.expand");
            const auto hint = expand_hint.empty() ? "Unbound" : expand_hint;
            const auto preview = expanded_
                ? safe_text(output)
                : collapsed_text(output, hint);
            (void)box_.add_child(std::make_unique<cch::tui::Text>(
                theme_.foreground(ThemeToken::ToolOutput, std::format("\n{}", preview)),
                0,
                0));
        }
    }

    // Result images render inline (pi tool-execution.ts imageComponents).
    // Slots outlive box rebuilds; the components are rendered by this
    // component rather than nested in the box.
    if (result_) {
        std::size_t image_position = 0;
        for (const auto& block : result_->content) {
            const auto* image = std::get_if<ai::ImageContent>(&block);
            if (image == nullptr) continue;
            if (image_position < image_slots_.size()) {
                image_slots_[image_position]->component->set_content(cch::tui::ImageContent{
                    .encoded_data = image->data,
                    .mime_type = image->mime_type,
                    .filename = std::nullopt,
                });
                image_slots_[image_position]->data = image->data;
                image_slots_[image_position]->mime_type = image->mime_type;
            } else {
                auto slot = std::make_unique<ImageSlot>(ImageSlot{
                    .component = std::make_unique<cch::tui::Image>(
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
                            .fallback_style = theme_.foreground_hook(ThemeToken::ToolOutput),
                        }),
                    .data = image->data,
                    .mime_type = image->mime_type,
                });
                image_slots_.push_back(std::move(slot));
            }
            ++image_position;
        }
        if (image_position < image_slots_.size()) {
            image_slots_.resize(image_position);
        }
    }
}

util::Expected<cch::tui::RenderResult> ToolExecutionComponent::render(std::size_t width) {
    auto rendered = box_.render(width);
    if (!rendered) return std::unexpected(rendered.error());
    for (const auto& slot : image_slots_) {
        auto image_rendered = slot->component->render(width);
        if (!image_rendered) return std::unexpected(image_rendered.error());
        const auto row_offset = rendered->lines.size();
        for (auto& line : image_rendered->lines) {
            rendered->lines.push_back(std::move(line));
        }
        for (auto& image : image_rendered->images) {
            image.region.row += row_offset;
            rendered->images.push_back(std::move(image));
        }
    }
    return rendered;
}

void ToolExecutionComponent::invalidate() {
    box_.invalidate();
}

} // namespace cch::coding_agent::tui
