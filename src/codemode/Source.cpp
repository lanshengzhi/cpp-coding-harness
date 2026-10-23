#include <cch/codemode/Source.hpp>

#include "support/Json.hpp"

#include <cmath>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace cch::codemode {

namespace {

constexpr std::string_view kSupportedFields = "`max_output_tokens` and `timeout_ms`";
/// Largest delay a JS `setTimeout` supports, which bounds `timeout_ms` (pi `MAX_TIMEOUT_MS`).
constexpr std::int64_t kMaxTimeoutMs = 2'147'483'647;
/// `Number.MAX_SAFE_INTEGER` (pi `Number.isSafeInteger`).
constexpr double kMaxSafeInteger = 9'007'199'254'740'991.0;

[[nodiscard]] support::Error source_error(std::string message) {
    return support::make_error(support::ErrorCode::Validation, std::move(message));
}

[[nodiscard]] std::string_view trim_start(std::string_view text) {
    constexpr std::string_view kWhitespace = " \t\n\r\v\f";
    const auto first = text.find_first_not_of(kWhitespace);
    return first == std::string_view::npos ? std::string_view{} : text.substr(first);
}

[[nodiscard]] std::string_view trim_end(std::string_view text) {
    constexpr std::string_view kWhitespace = " \t\n\r\v\f";
    const auto last = text.find_last_not_of(kWhitespace);
    return last == std::string_view::npos ? std::string_view{} : text.substr(0, last + 1);
}

[[nodiscard]] std::string_view trim(std::string_view text) { return trim_end(trim_start(text)); }

/// pi's OPTIONS_LINE per line: leading spaces/tabs, then the options prefix.
[[nodiscard]] bool is_options_line(std::string_view line) {
    const auto first = line.find_first_not_of(" \t");
    return first != std::string_view::npos && line.substr(first).starts_with(kCodemodeOptionsPrefix);
}

/// pi `Number.isSafeInteger`: a finite, integral value within the exactly
/// representable integer range. JSON numbers arrive as `double`.
[[nodiscard]] bool is_safe_integer(double value) {
    return std::isfinite(value) && std::floor(value) == value && std::abs(value) <= kMaxSafeInteger;
}

[[nodiscard]] support::Expected<CodemodeSourceOptions> parse_source_options(std::string_view directive) {
    if (directive.empty()) {
        return std::unexpected(
                source_error(std::format("@options must be a JSON object with supported fields {}", kSupportedFields)));
    }
    auto parsed = support::read_json(directive);
    if (!parsed) {
        return std::unexpected(source_error(std::format(
                "@options must be valid JSON with supported fields {}: {}", kSupportedFields, parsed.error().message)));
    }
    if (!parsed->holds<support::JsonValue::object_t>()) {
        return std::unexpected(
                source_error(std::format("@options must be a JSON object with supported fields {}", kSupportedFields)));
    }
    const auto& fields = parsed->get_object();
    for (const auto& [key, value] : fields) {
        (void)value;
        if (key != "max_output_tokens" && key != "timeout_ms") {
            return std::unexpected(
                    source_error(std::format("@options only supports {}; got `{}`", kSupportedFields, key)));
        }
    }
    CodemodeSourceOptions options;
    if (const auto it = fields.find("max_output_tokens"); it != fields.end()) {
        const auto* number = it->second.get_if<double>();
        if (number == nullptr || !is_safe_integer(*number) || *number < 0) {
            return std::unexpected(
                    source_error("@options field `max_output_tokens` must be a non-negative safe integer"));
        }
        options.max_output_tokens = static_cast<std::int64_t>(*number);
    }
    if (const auto it = fields.find("timeout_ms"); it != fields.end()) {
        const auto* number = it->second.get_if<double>();
        if (number == nullptr || !is_safe_integer(*number) || *number <= 0 || *number > kMaxTimeoutMs) {
            return std::unexpected(source_error(
                    std::format("@options field `timeout_ms` must be a positive integer up to {}", kMaxTimeoutMs)));
        }
        options.timeout_ms = static_cast<std::int64_t>(*number);
    }
    return options;
}

} // namespace

support::Expected<CodemodeSource> parse_codemode_source(std::string_view input) {
    if (trim(input).empty()) {
        return std::unexpected(
                source_error("Expected JavaScript source text (non-empty). Provide JS only, optionally with a first "
                             "line `// @options: {\"max_output_tokens\": 1000}`."));
    }
    const auto newline = input.find('\n');
    // Only the first line may carry options. A `// @options:` line anywhere
    // later is a typed placement error naming its 1-based line (frozen pi
    // rejects it rather than treating it as ordinary source).
    if (newline != std::string_view::npos) {
        std::size_t line_number = 1;
        std::string_view rest = input.substr(newline + 1);
        while (!rest.empty()) {
            ++line_number;
            const auto end = rest.find('\n');
            std::string_view line = end == std::string_view::npos ? rest : rest.substr(0, end);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (is_options_line(line)) {
                return std::unexpected(source_error(
                        std::format("The {} line must be the first line of the input; found one on line {}",
                                kCodemodeOptionsPrefix,
                                line_number)));
            }
            if (end == std::string_view::npos) break;
            rest = rest.substr(end + 1);
        }
    }
    std::string_view first_line = newline == std::string_view::npos ? input : input.substr(0, newline);
    if (!first_line.empty() && first_line.back() == '\r') {
        first_line.remove_suffix(1);
    }
    const auto trimmed = trim_start(first_line);
    if (!trimmed.starts_with(kCodemodeOptionsPrefix)) {
        return CodemodeSource{.code = std::string{input}, .options = {}};
    }
    // The options line is replaced by an empty line so script line numbers are
    // unchanged (pi `parseCodemodeSource`).
    const std::string code = newline == std::string_view::npos ? std::string{} : std::string{input.substr(newline)};
    if (trim(code).empty()) {
        return std::unexpected(
                source_error("The @options line must be followed by JavaScript source on subsequent lines"));
    }
    auto options = parse_source_options(trim(trimmed.substr(kCodemodeOptionsPrefix.size())));
    if (!options) {
        return std::unexpected(std::move(options.error()));
    }
    return CodemodeSource{.code = code, .options = *options};
}

} // namespace cch::codemode
