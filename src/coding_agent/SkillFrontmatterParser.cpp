#include "SkillFrontmatterParser.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace cch::coding_agent {

namespace {

/// Normalize Windows-style \r\n to Unix \n.
[[nodiscard]] std::string normalizeLineEndings(std::string_view content) {
    std::string result;
    result.reserve(content.size());
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\r') {
            if (i + 1 < content.size() && content[i + 1] == '\n') {
                result.push_back('\n');
                ++i; // skip the \n
            } else {
                result.push_back('\n');
            }
        } else {
            result.push_back(content[i]);
        }
    }
    return result;
}

/// Trim leading whitespace from a string_view.
[[nodiscard]] std::string_view ltrim(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) {
        sv.remove_prefix(1);
    }
    return sv;
}

/// Trim trailing whitespace from a string_view.
[[nodiscard]] std::string_view rtrim(std::string_view sv) {
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t')) {
        sv.remove_suffix(1);
    }
    return sv;
}

/// Trim leading and trailing whitespace.
[[nodiscard]] std::string_view trim(std::string_view sv) {
    return ltrim(rtrim(sv));
}

/// Strip matching quotes from both ends of a string_view.
/// Returns the original view if quotes don't match.
[[nodiscard]] std::string_view stripQuotes(std::string_view sv) {
    if (sv.size() >= 2) {
        char first = sv.front();
        char last = sv.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            sv.remove_prefix(1);
            sv.remove_suffix(1);
        }
    }
    return sv;
}

} // namespace

support::Expected<SkillFrontmatter> parseFrontmatter(std::string_view content) {
    std::string normalized = normalizeLineEndings(content);

    // Must start with "---\n" to have frontmatter.
    if (!normalized.starts_with("---\n")) {
        // Trim trailing whitespace.
        while (!normalized.empty() && (normalized.back() == '\n' ||
                                        normalized.back() == ' ' ||
                                        normalized.back() == '\t')) {
            normalized.pop_back();
        }
        return SkillFrontmatter{
            .fields = {},
            .body = std::move(normalized),
        };
    }

    // Find closing delimiter. Search for "\n---" starting from position 4
    // (after the opening "---\n"). The opening delimiter occupies indices 0-3.
    std::size_t closePos = std::string::npos;
    for (std::size_t i = 4; i + 4 <= normalized.size(); ++i) {
        if (normalized[i] == '\n' && normalized[i + 1] == '-' &&
            normalized[i + 2] == '-' && normalized[i + 3] == '-') {
            closePos = i;
            break;
        }
    }

    // Also handle the edge case where the YAML block is empty:
    // "---\n---\nBody" — the \n at index 3 is the start of the closing delimiter.
    if (closePos == std::string::npos && normalized.size() >= 7 &&
        normalized[3] == '\n' && normalized[4] == '-' &&
        normalized[5] == '-' && normalized[6] == '-') {
        closePos = 3;
    }

    if (closePos == std::string::npos) {
        // No closing delimiter — treat entire content as body.
        while (!normalized.empty() && (normalized.back() == '\n' ||
                                        normalized.back() == ' ' ||
                                        normalized.back() == '\t')) {
            normalized.pop_back();
        }
        return SkillFrontmatter{
            .fields = {},
            .body = std::move(normalized),
        };
    }

    // Extract YAML block (between "---\n" at start and "\n---" at closePos).
    // When closePos == 3, the YAML block is empty (e.g., "---\n---\nBody").
    std::string_view yamlBlock;
    if (closePos > 4) {
        yamlBlock = std::string_view(normalized.data() + 4, closePos - 4);
    } else {
        yamlBlock = {}; // empty YAML block
    }

    // Extract body (after "\n---" — skip the delimiter and optional trailing \n).
    std::size_t bodyStart = closePos + 4; // past "\n---"
    if (bodyStart < normalized.size() && normalized[bodyStart] == '\n') {
        ++bodyStart;
    }
    std::string body(normalized.data() + bodyStart, normalized.size() - bodyStart);
    // Trim leading and trailing whitespace from body.
    while (!body.empty() && (body.front() == '\n' || body.front() == ' ' || body.front() == '\t')) {
        body.erase(0, 1);
    }
    while (!body.empty() && (body.back() == '\n' || body.back() == ' ' || body.back() == '\t')) {
        body.pop_back();
    }

    // Parse YAML block line by line as flat key: value pairs.
    std::map<std::string, std::string> fields;
    std::string_view remaining = trim(yamlBlock);

    while (!remaining.empty()) {
        // Find end of current line.
        std::size_t lineEnd = remaining.find('\n');
        std::string_view line;
        if (lineEnd == std::string_view::npos) {
            line = remaining;
            remaining = {};
        } else {
            line = remaining.substr(0, lineEnd);
            remaining = remaining.substr(lineEnd + 1);
        }

        line = trim(line);
        if (line.empty()) continue;

        // Find the first colon.
        std::size_t colonPos = line.find(':');
        if (colonPos == std::string_view::npos) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Validation,
                "YAML frontmatter parse error",
                std::string("line has no colon: '") + std::string(line) + "'"));
        }

        std::string_view key = trim(line.substr(0, colonPos));
        std::string_view value = trim(line.substr(colonPos + 1));

        // Strip quotes from value.
        value = stripQuotes(value);

        fields.emplace(std::string(key), std::string(value));
    }

    return SkillFrontmatter{
        .fields = std::move(fields),
        .body = std::move(body),
    };
}

} // namespace cch::coding_agent
