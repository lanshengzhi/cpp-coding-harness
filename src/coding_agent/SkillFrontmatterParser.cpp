#include "SkillFrontmatterParser.hpp"

#include "coding_agent/TextBom.hpp"

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

/// YAML core-schema integer/float resolution: optional sign, hex (0x),
/// octal (0o), decimal digits with an optional fraction and exponent, and
/// the .inf/.nan specials. Underscores are not part of the core schema.
[[nodiscard]] bool isYamlNumber(std::string_view value) {
    std::size_t index = 0;
    if (!value.empty() && (value[index] == '+' || value[index] == '-')) {
        ++index;
    }
    const std::string_view body = value.substr(index);
    if (body.empty()) {
        return false;
    }
    if (body == ".inf" || body == ".Inf" || body == ".INF" || body == ".nan" || body == ".NaN" || body == ".NAN") {
        return true;
    }
    const auto all_digits = [](std::string_view digits, auto is_digit) {
        return !digits.empty() && std::all_of(digits.begin(), digits.end(), [is_digit](char c) {
            return is_digit(static_cast<unsigned char>(c));
        });
    };
    if (body.size() > 2 && (body.starts_with("0x") || body.starts_with("0X"))) {
        return all_digits(body.substr(2), [](unsigned char c) { return std::isxdigit(c) != 0; });
    }
    if (body.size() > 2 && (body.starts_with("0o") || body.starts_with("0O"))) {
        return all_digits(body.substr(2), [](unsigned char c) { return c >= '0' && c <= '7'; });
    }
    bool has_digits = false;
    std::size_t cursor = 0;
    while (cursor < body.size() && std::isdigit(static_cast<unsigned char>(body[cursor])) != 0) {
        ++cursor;
        has_digits = true;
    }
    if (cursor < body.size() && body[cursor] == '.') {
        ++cursor;
        while (cursor < body.size() && std::isdigit(static_cast<unsigned char>(body[cursor])) != 0) {
            ++cursor;
            has_digits = true;
        }
    }
    if (!has_digits) {
        return false;
    }
    if (cursor < body.size() && (body[cursor] == 'e' || body[cursor] == 'E')) {
        ++cursor;
        if (cursor < body.size() && (body[cursor] == '+' || body[cursor] == '-')) {
            ++cursor;
        }
        if (cursor >= body.size()) {
            return false;
        }
        while (cursor < body.size() && std::isdigit(static_cast<unsigned char>(body[cursor])) != 0) {
            ++cursor;
        }
    }
    return cursor == body.size();
}

/// YAML core-schema scalar typing for the flat parser: a value is a string
/// when it is quoted or when it does not resolve to null, a boolean, a
/// number, or a flow collection (pi `typeof frontmatter[key] === "string"`).
[[nodiscard]] bool isStringScalar(std::string_view value) {
    if (value.empty()) {
        return false; // null
    }
    if ((value.front() == '"' && value.back() == '"' && value.size() >= 2) ||
            (value.front() == '\'' && value.back() == '\'' && value.size() >= 2)) {
        return true;
    }
    if (value.front() == '[' || value.front() == '{') {
        return false; // flow sequence or flow mapping
    }
    if (value == "~" || value == "null" || value == "Null" || value == "NULL") {
        return false; // null
    }
    if (value == "true" || value == "True" || value == "TRUE" || value == "false" || value == "False" ||
            value == "FALSE") {
        return false; // boolean
    }
    return !isYamlNumber(value);
}

} // namespace

support::Expected<SkillFrontmatter> parseFrontmatter(std::string_view content) {
    // pi `utils/frontmatter.ts`: the leading UTF-8 BOM is stripped before the
    // delimiters are located, so both the frontmatter and the body are clean.
    std::string normalized = normalizeLineEndings(strip_bom(content));

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
                .non_string_fields = {},
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
                .non_string_fields = {},
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
    std::set<std::string> non_string_fields;
    std::string_view remaining = trim(yamlBlock);
    // The most recent top-level key, used to classify continuation lines
    // (block sequences and nested mappings) as that key's non-string value.
    std::string lastKey;

    while (!remaining.empty()) {
        // Find end of current line.
        std::size_t lineEnd = remaining.find('\n');
        std::string_view rawLine;
        if (lineEnd == std::string_view::npos) {
            rawLine = remaining;
            remaining = {};
        } else {
            rawLine = remaining.substr(0, lineEnd);
            remaining = remaining.substr(lineEnd + 1);
        }

        std::string_view line = trim(rawLine);
        if (line.empty()) continue;

        // Deeper-indented lines and block-sequence items are the previous
        // key's collection value (a YAML sequence or nested mapping, never a
        // string); the flat parser keeps no text for them. A block-scalar
        // key (`key: |`) carries a non-empty string value, so its body lines
        // stay swallowed without changing the key's typing.
        const bool indented = rawLine.front() == ' ' || rawLine.front() == '\t';
        const bool sequenceItem = line.starts_with("- ");
        if (indented || sequenceItem) {
            if (!lastKey.empty() && fields[lastKey].empty()) {
                non_string_fields.insert(lastKey);
            }
            continue;
        }

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

        // Record the YAML scalar type before the quotes are stripped: a
        // quoted value is always a string, an unquoted one can resolve to
        // null, boolean, number, or a flow collection.
        if (!isStringScalar(value)) {
            non_string_fields.emplace(key);
        }

        // Strip quotes from value.
        value = stripQuotes(value);

        fields.emplace(std::string(key), std::string(value));
        lastKey = std::string(key);
    }

    return SkillFrontmatter{
            .fields = std::move(fields),
            .non_string_fields = std::move(non_string_fields),
            .body = std::move(body),
    };
}

} // namespace cch::coding_agent
