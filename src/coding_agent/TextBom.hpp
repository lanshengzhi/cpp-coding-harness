#pragma once

#include <string_view>

namespace cch::coding_agent {

/// Remove a leading UTF-8 byte order mark from decoded text (pi
/// `utils/text.ts` `stripBom`): resource files authored elsewhere may carry
/// the U+FEFF prefix, and every resource read strips it before the text is
/// used (pi `core/resource-loader.ts` `resolvePromptInput`/
/// `loadContextFileFromDir`, `utils/frontmatter.ts` `parseFrontmatter`).
[[nodiscard]] inline std::string_view strip_bom(std::string_view content) {
    constexpr std::string_view kBom = "\xEF\xBB\xBF";
    return content.starts_with(kBom) ? content.substr(kBom.size()) : content;
}

} // namespace cch::coding_agent
