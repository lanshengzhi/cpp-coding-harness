#include "ToolExecutionComponent.hpp"

#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Utils.hpp>
#include "coding_agent/BoundedText.hpp"
#include "coding_agent/tui/Theme.hpp"
#include "coding_agent/tui/tool_renderers/RenderUtils.hpp"
#include "support/Json.hpp"

#include <cch/support/Error.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

[[nodiscard]] std::string safe_text(std::string text) {
    return bounded_redacted_presentation(std::move(text));
}

[[nodiscard]] ai::TimestampMs now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
}

[[nodiscard]] std::string result_text(const ai::ToolResultMessage& result) {
    std::string text;
    for (const auto& block : result.content) {
        if (const auto* value = std::get_if<ai::TextContent>(&block); value != nullptr) {
            if (!text.empty()) text.push_back('\n');
            text += value->text;
        }
    }
    // pi `getTextOutput` strips `\r` before the result is split into lines. The
    // redacting bound is this component's existing presentation budget
    // (ADR 0028); it is unchanged by the renderer seam.
    return normalize_display_text(safe_text(std::move(text)));
}

} // namespace

struct ToolExecutionComponent::ImageSlot {
    std::unique_ptr<cch::tui::Image> component;
    std::string data;
    std::string mime_type;
};

ToolExecutionComponent::ToolExecutionComponent(const LiveTheme& theme,
        std::shared_ptr<const SharedKeybindings> keybindings,
        std::string tool_name,
        std::string tool_call_id,
        std::string arguments_json,
        std::string cwd,
        ToolRendererRegistry registry)
    : theme_(theme), keybindings_(std::move(keybindings)), tool_name_(std::move(tool_name)),
      tool_call_id_(std::move(tool_call_id)), arguments_json_(safe_text(std::move(arguments_json))),
      cwd_(std::move(cwd)), registry_(std::move(registry)),
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
    if (!is_partial && !ended_at_ms_.has_value()) ended_at_ms_ = now_ms();
    result_ = std::move(result);
    is_partial_ = is_partial;
    rebuild();
}

void ToolExecutionComponent::set_expanded(bool expanded) {
    expanded_ = expanded;
    rebuild();
}

void ToolExecutionComponent::mark_execution_started() {
    if (started_at_ms_.has_value()) return;
    started_at_ms_ = now_ms();
    rebuild();
}

void ToolExecutionComponent::set_args_complete() {
    if (args_complete_) return;
    args_complete_ = true;
    rebuild();
}

support::JsonValue ToolExecutionComponent::parsed_arguments() const {
    if (auto parsed = support::read_json(arguments_json_); parsed) {
        return std::move(*parsed);
    }
    return support::JsonValue::object_t{};
}

void ToolExecutionComponent::append_rendered(const ToolRenderedText& rendered) {
    // pi gives each renderer one component holding the title and the body, so
    // the blank lines a renderer writes into its own text survive: joining the
    // pieces here is what keeps `title` + `"\n\n"` + body at one blank row.
    std::string text = rendered.title;
    for (const auto& block : rendered.blocks) {
        if (!block.empty()) text += block;
    }
    if (text.empty()) return;
    (void)box_.add_child(std::make_unique<cch::tui::Text>(std::move(text), 0, 0));
}

void ToolExecutionComponent::rebuild() {
    box_.clear();
    box_.set_background_hook(
        is_partial_
            ? theme_.background_hook(ThemeToken::ToolPendingBg)
            : (result_ && result_->is_error)
                ? theme_.background_hook(ThemeToken::ToolErrorBg)
                : theme_.background_hook(ThemeToken::ToolSuccessBg));

    // The expand key is resolved once here so every renderer reads the same
    // text (pi resolves it once per hint through `keyHint`).
    const auto expand_key = keybindings_->registry().key_text("app.tools.expand");
    expand_key_ = expand_key.empty() ? "Unbound" : expand_key;
    expand_hint_ = theme_.foreground(ThemeToken::Dim, expand_key_) + theme_.foreground(ThemeToken::Muted, " to expand");

    const auto args = parsed_arguments();
    const ToolRenderContext context{
            .args = args,
            .tool_name = tool_name_,
            .tool_call_id = tool_call_id_,
            .cwd = cwd_,
            .theme = theme_,
            .expand_key = expand_key_,
            .expand_hint = expand_hint_,
            .args_complete = args_complete_,
            .execution_started = started_at_ms_.has_value(),
            .is_partial = is_partial_,
            .is_error = result_ && result_->is_error,
            .expanded = expanded_,
            .started_at_ms = started_at_ms_,
            .ended_at_ms = ended_at_ms_,
    };

    auto& renderer = registry_.lookup(tool_name_);
    // A renderer pair with no call half draws pi's bare bold tool name.
    const auto call = renderer.render_call
                              ? renderer.render_call(context)
                              : ToolRenderedText{
                                        .title = bold_foreground(theme_, ThemeToken::ToolTitle, tool_name_),
                                };
    append_rendered(call);

    if (!result_) {
        rebuild_image_slots();
        return;
    }

    auto& result_renderer = renderer.render_result ? renderer.render_result : registry_.fallback().render_result;
    const ToolRenderedResult rendered_result{
            .output = result_text(*result_),
            .details = result_->details,
    };
    append_rendered(result_renderer(rendered_result, context));
    rebuild_image_slots();
}

void ToolExecutionComponent::rebuild_image_slots() {
    // Result images render inline (pi tool-execution.ts imageComponents).
    // Slots outlive box rebuilds; the components are rendered by this
    // component rather than nested in the box.
    if (!result_) return;
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
                                    .constraints =
                                            {
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

support::Expected<cch::tui::RenderResult> ToolExecutionComponent::render(std::size_t width) {
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
