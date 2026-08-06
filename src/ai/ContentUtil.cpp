#include <cch/ai/Content.hpp>

#include <string>
#include <variant>
#include <vector>

namespace cch::ai {

namespace {

/// Concatenates the text of every text block in a content-block list. Generic
/// block-walking helper kept in the implementation layer (guardrail 4).
template <typename ContentBlock>
[[nodiscard]] std::string text_from_blocks(const std::vector<ContentBlock>& content) {
    std::string text;
    for (const auto& block : content) {
        if (const auto* text_block = std::get_if<TextContent>(&block)) {
            text += text_block->text;
        }
    }
    return text;
}

} // namespace

[[nodiscard]] std::string text_from_content(const std::vector<Content>& content) {
    return text_from_blocks(content);
}

[[nodiscard]] std::string text_from_assistant_content(
    const std::vector<AssistantContent>& content) {
    return text_from_blocks(content);
}

} // namespace cch::ai
