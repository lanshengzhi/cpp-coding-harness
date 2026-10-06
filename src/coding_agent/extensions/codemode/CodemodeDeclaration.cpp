#include "coding_agent/extensions/codemode/CodemodeDeclaration.hpp"

#include "support/Json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace cch::coding_agent::extensions {

namespace {

constexpr std::string_view kOptionsPrefix = "// @options:";
constexpr std::string_view kSupportedFields = "`max_output_tokens` and `timeout_ms`";
/// Largest delay a JS `setTimeout` supports, which bounds `timeout_ms` (pi `MAX_TIMEOUT_MS`).
constexpr std::int64_t kMaxTimeoutMs = 2'147'483'647;
/// `Number.MAX_SAFE_INTEGER` (pi `Number.isSafeInteger`).
constexpr double kMaxSafeInteger = 9'007'199'254'740'991.0;

constexpr std::string_view kNameField = "name";
constexpr std::string_view kDescriptionField = "description";
constexpr std::string_view kInputSchemaField = "inputSchema";
constexpr std::string_view kSourceField = "source";

[[nodiscard]] support::Error declaration_error(std::string message, std::string detail = {}) {
    return support::make_error(support::ErrorCode::Validation, std::move(message), std::move(detail));
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

/// pi `Number.isSafeInteger`: a finite, integral value within the exactly
/// representable integer range. JSON numbers arrive as `double`.
[[nodiscard]] bool is_safe_integer(double value) {
    return std::isfinite(value) && std::floor(value) == value && std::abs(value) <= kMaxSafeInteger;
}

[[nodiscard]] support::Expected<std::string> read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(declaration_error(std::format("codemode file '{}' could not be read", path.string())));
    }
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] support::Expected<CodemodeSourceOptions> parse_source_options(std::string_view directive) {
    if (directive.empty()) {
        return std::unexpected(declaration_error(
                std::format("`@options` must be a JSON object with supported fields {}", kSupportedFields)));
    }
    auto parsed = support::read_json(directive);
    if (!parsed) {
        return std::unexpected(
                declaration_error(std::format("`@options` must be valid JSON with supported fields {}: {}",
                        kSupportedFields,
                        parsed.error().message)));
    }
    if (!parsed->holds<support::JsonValue::object_t>()) {
        return std::unexpected(declaration_error(
                std::format("`@options` must be a JSON object with supported fields {}", kSupportedFields)));
    }
    const auto& fields = parsed->get_object();
    for (const auto& [key, value] : fields) {
        (void)value;
        if (key != "max_output_tokens" && key != "timeout_ms") {
            return std::unexpected(
                    declaration_error(std::format("`@options` only supports {}; got `{}`", kSupportedFields, key)));
        }
    }
    CodemodeSourceOptions options;
    if (const auto it = fields.find("max_output_tokens"); it != fields.end()) {
        const auto* number = it->second.get_if<double>();
        if (number == nullptr || !is_safe_integer(*number) || *number < 0) {
            return std::unexpected(
                    declaration_error("`@options` field `max_output_tokens` must be a non-negative safe integer"));
        }
        options.max_output_tokens = static_cast<std::int64_t>(*number);
    }
    if (const auto it = fields.find("timeout_ms"); it != fields.end()) {
        const auto* number = it->second.get_if<double>();
        if (number == nullptr || !is_safe_integer(*number) || *number <= 0 || *number > kMaxTimeoutMs) {
            return std::unexpected(declaration_error(
                    std::format("`@options` field `timeout_ms` must be a positive integer up to {}", kMaxTimeoutMs)));
        }
        options.timeout_ms = static_cast<std::int64_t>(*number);
    }
    return options;
}

[[nodiscard]] const support::JsonValue* find_field(const support::JsonValue::object_t& object, std::string_view key) {
    const auto it = object.find(std::string{key});
    return it == object.end() ? nullptr : &it->second;
}

} // namespace

std::filesystem::path codemode_declaration_directory(const std::filesystem::path& workspace) {
    return workspace / ".pi" / "codemode";
}

support::Expected<CodemodeSource> parse_codemode_source(std::string_view input) {
    if (trim(input).empty()) {
        return std::unexpected(declaration_error(
                "Expected JavaScript source text (non-empty). Provide JS only, optionally with a first "
                "line `// @options: {\"max_output_tokens\": 1000}`."));
    }
    const auto newline = input.find('\n');
    std::string_view first_line = newline == std::string_view::npos ? input : input.substr(0, newline);
    if (!first_line.empty() && first_line.back() == '\r') {
        first_line.remove_suffix(1);
    }
    const auto trimmed = trim_start(first_line);
    if (!trimmed.starts_with(kOptionsPrefix)) {
        return CodemodeSource{.code = std::string{input}, .options = {}};
    }
    // The options line is replaced by an empty line so script line numbers are
    // unchanged (pi `parseCodemodeSource`).
    const std::string code = newline == std::string_view::npos ? std::string{} : std::string{input.substr(newline)};
    if (trim(code).empty()) {
        return std::unexpected(
                declaration_error("The @options line must be followed by JavaScript source on subsequent lines"));
    }
    auto options = parse_source_options(trim(trimmed.substr(kOptionsPrefix.size())));
    if (!options) {
        return std::unexpected(std::move(options.error()));
    }
    return CodemodeSource{.code = code, .options = *options};
}

support::Expected<CodemodeDeclaration> parse_codemode_declaration(
        const std::filesystem::path& declaration_path, std::string_view json_text) {
    const std::string where = declaration_path.string();
    auto parsed = support::read_json(json_text);
    if (!parsed) {
        return std::unexpected(declaration_error(
                std::format("codemode declaration '{}' is not valid JSON", where), parsed.error().message));
    }
    if (!parsed->holds<support::JsonValue::object_t>()) {
        return std::unexpected(
                declaration_error(std::format("codemode declaration '{}' must be a JSON object", where)));
    }
    const auto& object = parsed->get_object();
    for (const auto& [key, value] : object) {
        (void)value;
        if (key != kNameField && key != kDescriptionField && key != kInputSchemaField && key != kSourceField) {
            return std::unexpected(
                    declaration_error(std::format("codemode declaration '{}' has unknown field `{}`", where, key),
                            "allowed fields are `name`, `description`, `inputSchema`, and `source`"));
        }
    }

    CodemodeDeclaration declaration;
    const auto* name = find_field(object, kNameField);
    const auto* name_text = name == nullptr ? nullptr : name->get_if<std::string>();
    if (name_text == nullptr || trim(*name_text).empty()) {
        return std::unexpected(
                declaration_error(std::format("codemode declaration '{}' is missing a non-empty `name`", where)));
    }
    declaration.name = *name_text;

    const auto* description = find_field(object, kDescriptionField);
    const auto* description_text = description == nullptr ? nullptr : description->get_if<std::string>();
    if (description_text == nullptr) {
        return std::unexpected(
                declaration_error(std::format("codemode declaration '{}' is missing a string `description`", where)));
    }
    declaration.description = *description_text;

    if (const auto* input_schema = find_field(object, kInputSchemaField); input_schema != nullptr) {
        if (!input_schema->holds<support::JsonValue::object_t>()) {
            return std::unexpected(declaration_error(
                    std::format("codemode declaration '{}' field `inputSchema` must be a JSON object", where)));
        }
        declaration.input_schema = *input_schema;
    }

    const auto* source = find_field(object, kSourceField);
    const auto* source_text = source == nullptr ? nullptr : source->get_if<std::string>();
    if (source_text == nullptr || trim(*source_text).empty()) {
        return std::unexpected(
                declaration_error(std::format("codemode declaration '{}' is missing a non-empty `source`", where)));
    }

    declaration.source_path = (declaration_path.parent_path() / *source_text).lexically_normal();
    auto source_code = read_text_file(declaration.source_path);
    if (!source_code) {
        return std::unexpected(declaration_error(std::format("codemode declaration '{}' source '{}' could not be read",
                                                         where,
                                                         declaration.source_path.string()),
                source_code.error().message));
    }
    auto parsed_source = parse_codemode_source(*source_code);
    if (!parsed_source) {
        return std::unexpected(declaration_error(
                std::format(
                        "codemode declaration '{}' source '{}' is invalid", where, declaration.source_path.string()),
                parsed_source.error().message));
    }
    declaration.source = std::move(*parsed_source);
    return declaration;
}

support::Expected<CodemodeDeclaration> load_codemode_declaration(const std::filesystem::path& declaration_path) {
    auto text = read_text_file(declaration_path);
    if (!text) {
        return std::unexpected(std::move(text.error()));
    }
    return parse_codemode_declaration(declaration_path, *text);
}

support::Expected<std::vector<CodemodeDeclaration>> load_codemode_declarations(
        const std::filesystem::path& declaration_directory) {
    std::error_code iterator_error;
    std::filesystem::directory_iterator iterator{declaration_directory, iterator_error};
    if (iterator_error) {
        std::error_code type_error;
        if (!std::filesystem::is_directory(declaration_directory, type_error)) {
            // A project without codemode sources declares nothing; that is not
            // an error. Any other failure (present but unreadable) is.
            std::error_code exists_error;
            if (!std::filesystem::exists(declaration_directory, exists_error)) {
                return std::vector<CodemodeDeclaration>{};
            }
        }
        return std::unexpected(declaration_error(
                std::format("codemode declaration directory '{}' could not be read", declaration_directory.string())));
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry : iterator) {
        std::error_code type_error;
        if (!entry.is_regular_file(type_error)) {
            continue;
        }
        if (entry.path().extension() != ".json") {
            continue;
        }
        files.push_back(entry.path());
    }
    std::ranges::sort(files, {}, [](const std::filesystem::path& path) { return path.filename().string(); });

    std::vector<CodemodeDeclaration> declarations;
    declarations.reserve(files.size());
    for (const auto& file : files) {
        auto declaration = load_codemode_declaration(file);
        if (!declaration) {
            return std::unexpected(std::move(declaration.error()));
        }
        declarations.push_back(std::move(*declaration));
    }
    return declarations;
}

} // namespace cch::coding_agent::extensions
