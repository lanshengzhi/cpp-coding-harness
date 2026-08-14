#include "UserMessageComponent.hpp"

#include <cch/tui/Utils.hpp>
#include "coding_agent/BoundedText.hpp"
#include "coding_agent/tui/Theme.hpp"

#include <cch/util/Error.hpp>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace cch::coding_agent::tui {
namespace {

// pi `user-message.ts` OSC 133 prompt zones.
constexpr std::string_view kOsc133ZoneStart = "\x1b]133;A\x07";
constexpr std::string_view kOsc133ZoneEnd = "\x1b]133;B\x07";
constexpr std::string_view kOsc133ZoneFinal = "\x1b]133;C\x07";

[[nodiscard]] std::string safe_text(std::string text) {
    return bounded_redacted_presentation(std::move(text));
}

/// Text blocks join with newlines (pi `getUserMessageText` joins with "").
[[nodiscard]] std::string user_text(
    const std::variant<std::string, std::vector<ai::Content>>& content) {
    if (const auto* text = std::get_if<std::string>(&content)) {
        return *text;
    }
    std::string text;
    for (const auto& block : std::get<std::vector<ai::Content>>(content)) {
        if (const auto* value = std::get_if<ai::TextContent>(&block)) {
            if (!text.empty()) text.push_back('\n');
            text += value->text;
        }
    }
    return text;
}

} // namespace

UserMessageComponent::UserMessageComponent(
    const LiveTheme& theme,
    std::variant<std::string, std::vector<ai::Content>> content,
    std::size_t output_pad)
    : theme_(theme),
      box_(output_pad, 1, theme.background_hook(ThemeToken::UserMessageBg)) {
    auto style = theme.markdown_style();
    style.text = theme.foreground_hook(ThemeToken::UserMessageText);
    markdown_ = std::make_unique<cch::tui::Markdown>(
        safe_text(user_text(content)),
        0,
        0,
        std::move(style));
    (void)box_.add_child(std::move(markdown_));

    if (const auto* blocks = std::get_if<std::vector<ai::Content>>(&content)) {
        for (const auto& block : *blocks) {
            const auto* image = std::get_if<ai::ImageContent>(&block);
            if (image == nullptr) continue;
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
                        .fallback_style = theme.foreground_hook(ThemeToken::UserMessageText),
                    }),
                .data = image->data,
                .mime_type = image->mime_type,
            });
            image_slots_.push_back(std::move(slot));
        }
    }
}

UserMessageComponent::~UserMessageComponent() = default;

util::Expected<cch::tui::RenderResult> UserMessageComponent::render(std::size_t width) {
    auto rendered = box_.render(width);
    if (!rendered) return std::unexpected(rendered.error());
    if (!rendered->lines.empty()) {
        // pi user-message.ts: the zones wrap the rendered block.
        rendered->lines.front() = std::string{kOsc133ZoneStart} + rendered->lines.front();
        rendered->lines.back() =
            std::string{kOsc133ZoneEnd} + std::string{kOsc133ZoneFinal} + rendered->lines.back();
    }
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

void UserMessageComponent::invalidate() {
    box_.invalidate();
}

} // namespace cch::coding_agent::tui
