#pragma once

#include "util/Json.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::ai::api {

/// Repairs malformed JSON string literals the way pi's `repairJson` does
/// (`utils/json-parse.ts`): escapes raw control characters inside strings and
/// doubles backslashes before invalid escape characters.
[[nodiscard]] inline std::string repair_json_strings(std::string_view json) {
    std::string repaired;
    repaired.reserve(json.size());
    bool in_string = false;
    for (std::size_t index = 0; index < json.size(); ++index) {
        const auto character = static_cast<unsigned char>(json[index]);
        if (!in_string) {
            repaired.push_back(static_cast<char>(character));
            if (character == '"') {
                in_string = true;
            }
            continue;
        }
        if (character == '"') {
            repaired.push_back('"');
            in_string = false;
            continue;
        }
        if (character == '\\') {
            if (index + 1 >= json.size()) {
                repaired += "\\\\";
                continue;
            }
            const auto next = json[index + 1];
            const bool simple_escape = next == '"' || next == '\\' || next == '/' ||
                                       next == 'b' || next == 'f' || next == 'n' ||
                                       next == 'r' || next == 't';
            if (simple_escape) {
                repaired.push_back('\\');
                repaired.push_back(next);
                ++index;
                continue;
            }
            if (next == 'u' && index + 5 < json.size() &&
                std::ranges::all_of(
                    json.substr(index + 2, 4),
                    [](char digit) {
                        return (digit >= '0' && digit <= '9') ||
                               (digit >= 'a' && digit <= 'f') ||
                               (digit >= 'A' && digit <= 'F');
                    })) {
                repaired.append(json.substr(index, 6));
                index += 5;
                continue;
            }
            repaired += "\\\\";
            continue;
        }
        switch (character) {
        case '\b':
            repaired += "\\b";
            break;
        case '\f':
            repaired += "\\f";
            break;
        case '\n':
            repaired += "\\n";
            break;
        case '\r':
            repaired += "\\r";
            break;
        case '\t':
            repaired += "\\t";
            break;
        default:
            if (character < 0x20U) {
                constexpr char kHex[] = "0123456789abcdef";
                repaired += "\\u00";
                repaired.push_back(kHex[(character >> 4U) & 0x0fU]);
                repaired.push_back(kHex[character & 0x0fU]);
            } else {
                repaired.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    return repaired;
}

namespace detail {

/// Streaming-tolerant JSON parser mirroring pi's `partial-json` dependency
/// (used by `utils/json-parse.ts` `parseStreamingJson`) with the default
/// `Allow.ALL` configuration. It parses the best possible prefix of a JSON
/// document: unterminated strings are closed, incomplete objects/arrays return
/// the already-parsed members, and a trailing key without a value is dropped
/// rather than completed to `null`. Returns nullopt where `partial-json` would
/// throw (empty input or input with no recoverable prefix). The JSON Infinity/
/// NaN literal extensions that `partial-json` accepts are intentionally not
/// supported: they are not valid JSON tool arguments and would not serialize.
struct PartialJsonContext {
    std::string_view json;
    std::size_t index{};
};

inline void skip_blank(PartialJsonContext& context) {
    while (context.index < context.json.size() &&
           (context.json[context.index] == ' ' || context.json[context.index] == '\n' ||
            context.json[context.index] == '\r' || context.json[context.index] == '\t')) {
        ++context.index;
    }
}

[[nodiscard]] inline bool literal_prefix(
    PartialJsonContext& context,
    std::string_view literal) {
    const auto remaining = context.json.substr(context.index);
    if (remaining.size() >= literal.size()) {
        return remaining.starts_with(literal);
    }
    return literal.starts_with(remaining);
}

[[nodiscard]] inline std::optional<util::JsonValue> parse_any(PartialJsonContext& context);

[[nodiscard]] inline std::optional<std::string> parse_str(PartialJsonContext& context) {
    const auto start = context.index;
    bool escape = false;
    ++context.index; // skip the initial quote
    while (context.index < context.json.size() &&
           (context.json[context.index] != '"' ||
            (escape && context.json[context.index - 1] == '\\'))) {
        escape = context.json[context.index] == '\\' ? !escape : false;
        ++context.index;
    }
    if (context.index < context.json.size() && context.json[context.index] == '"') {
        const auto end = context.index + 1 - (escape ? 1 : 0);
        context.index += 1;
        const auto substring = context.json.substr(start, end - start);
        if (auto parsed = util::read_json<util::JsonValue>(substring)) {
            if (const auto* text = parsed->get_if<std::string>()) {
                return *text;
            }
        }
        return std::nullopt; // malformed string literal
    }
    // Unterminated string: `partial-json` closes it; on an invalid trailing
    // escape it retries from the last backslash.
    const auto end = context.index - (escape ? 1 : 0);
    auto candidate = std::string{context.json.substr(start, end - start)} + '"';
    if (auto parsed = util::read_json<util::JsonValue>(candidate)) {
        if (const auto* text = parsed->get_if<std::string>()) {
            return *text;
        }
    }
    const auto last_backslash = candidate.rfind('\\');
    if (last_backslash != std::string::npos) {
        const auto retry = candidate.substr(0, last_backslash) + '"';
        if (auto parsed = util::read_json<util::JsonValue>(retry)) {
            if (const auto* text = parsed->get_if<std::string>()) {
                return *text;
            }
        }
    }
    return std::nullopt; // unterminated string literal
}

[[nodiscard]] inline std::optional<util::JsonValue> parse_obj(PartialJsonContext& context) {
    ++context.index; // skip the opening brace
    util::JsonValue::object_t object;
    while (true) {
        skip_blank(context);
        if (context.index >= context.json.size()) {
            return util::JsonValue{std::move(object)}; // Allow.OBJ
        }
        if (context.json[context.index] == '}') {
            break;
        }
        auto key = parse_str(context);
        if (!key) {
            return util::JsonValue{std::move(object)}; // Allow.OBJ
        }
        skip_blank(context);
        if (context.index >= context.json.size()) {
            return util::JsonValue{std::move(object)}; // Allow.OBJ
        }
        ++context.index; // skip the colon
        auto value = parse_any(context);
        if (!value) {
            return util::JsonValue{std::move(object)}; // Allow.OBJ
        }
        object.emplace(std::move(*key), std::move(*value));
        skip_blank(context);
        if (context.index < context.json.size() && context.json[context.index] == ',') {
            ++context.index;
        }
    }
    ++context.index; // skip the closing brace
    return util::JsonValue{std::move(object)};
}

[[nodiscard]] inline std::optional<util::JsonValue> parse_arr(PartialJsonContext& context) {
    ++context.index; // skip the opening bracket
    util::JsonValue::array_t array;
    while (true) {
        if (context.index >= context.json.size()) {
            return util::JsonValue{std::move(array)}; // Allow.ARR
        }
        if (context.json[context.index] == ']') {
            break;
        }
        auto value = parse_any(context);
        if (!value) {
            return util::JsonValue{std::move(array)}; // Allow.ARR
        }
        array.push_back(std::move(*value));
        skip_blank(context);
        if (context.index < context.json.size() && context.json[context.index] == ',') {
            ++context.index;
        }
    }
    ++context.index; // skip the closing bracket
    return util::JsonValue{std::move(array)};
}

[[nodiscard]] inline std::optional<double> parse_number_token(std::string_view token) {
    if (auto parsed = util::read_json<util::JsonValue>(token)) {
        if (const auto* number = parsed->get_if<double>()) {
            return *number;
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<double> parse_num(PartialJsonContext& context) {
    if (context.index == 0) {
        // Top-level scalar: `partial-json` parses the whole remaining string.
        if (context.json == "-") {
            return std::nullopt;
        }
        if (auto parsed = parse_number_token(context.json)) {
            return parsed;
        }
        const auto exponent = context.json.rfind('e');
        if (exponent != std::string_view::npos) {
            if (auto parsed = parse_number_token(context.json.substr(0, exponent))) {
                return parsed;
            }
        }
        return std::nullopt;
    }
    const auto start = context.index;
    if (context.json[context.index] == '-') {
        ++context.index;
    }
    while (context.index < context.json.size() &&
           context.json[context.index] != ',' &&
           context.json[context.index] != ']' &&
           context.json[context.index] != '}') {
        ++context.index;
    }
    const auto candidate = context.json.substr(start, context.index - start);
    if (auto parsed = parse_number_token(candidate)) {
        return parsed;
    }
    if (candidate == "-") {
        return std::nullopt;
    }
    const auto exponent = candidate.rfind('e');
    if (exponent != std::string_view::npos) {
        if (auto parsed = parse_number_token(candidate.substr(0, exponent))) {
            return parsed;
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<util::JsonValue> parse_any(PartialJsonContext& context) {
    skip_blank(context);
    if (context.index >= context.json.size()) {
        return std::nullopt; // "Unexpected end of input"
    }
    const auto current = context.json[context.index];
    if (current == '"') {
        auto text = parse_str(context);
        if (!text) {
            return std::nullopt;
        }
        return util::JsonValue{std::move(*text)};
    }
    if (current == '{') {
        return parse_obj(context);
    }
    if (current == '[') {
        return parse_arr(context);
    }
    if (literal_prefix(context, "null")) {
        context.index += 4;
        return util::JsonValue{nullptr};
    }
    if (literal_prefix(context, "true")) {
        context.index += 4;
        return util::JsonValue{true};
    }
    if (literal_prefix(context, "false")) {
        context.index += 5;
        return util::JsonValue{false};
    }
    if (auto number = parse_num(context)) {
        return util::JsonValue{*number};
    }
    return std::nullopt;
}

} // namespace detail

/// Port of `partial-json` `parse` with `Allow.ALL`: best-effort prefix parse
/// of incomplete JSON. Returns nullopt where the library would throw.
[[nodiscard]] inline std::optional<util::JsonValue> parse_partial_json(std::string_view input) {
    std::size_t begin = 0;
    std::size_t end = input.size();
    while (begin < end && (input[begin] == ' ' || input[begin] == '\n' ||
                           input[begin] == '\r' || input[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (input[end - 1] == ' ' || input[end - 1] == '\n' ||
                           input[end - 1] == '\r' || input[end - 1] == '\t')) {
        --end;
    }
    if (begin == end) {
        return std::nullopt; // "is empty"
    }
    detail::PartialJsonContext context{input.substr(begin, end - begin), 0};
    return detail::parse_any(context);
}

/// Port of pi's `parseStreamingJson` (`utils/json-parse.ts`): exact JSON
/// parse, then repaired-JSON parse, then the `partial-json` tolerant parse on
/// both the raw and repaired input, falling back to an empty object. This is
/// the parsing semantics pi applies to streaming tool-call arguments.
[[nodiscard]] inline util::JsonValue parse_streaming_json(std::string_view raw) {
    if (auto parsed = util::read_json<util::JsonValue>(raw)) {
        return std::move(*parsed);
    }
    auto repaired = repair_json_strings(raw);
    if (auto parsed = util::read_json<util::JsonValue>(repaired)) {
        return std::move(*parsed);
    }
    if (auto parsed = parse_partial_json(raw)) {
        return std::move(*parsed);
    }
    if (auto parsed = parse_partial_json(repaired)) {
        return std::move(*parsed);
    }
    return util::JsonValue::object_t{};
}

} // namespace cch::ai::api
