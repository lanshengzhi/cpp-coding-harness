#include "ToolArgumentPreparation.hpp"

#include "ToolArgumentDetail.hpp"

#include "support/BoundedText.hpp"

#include "support/Json.hpp"

#include <string>
#include <utility>

namespace cch::agent {
namespace {

[[nodiscard]] support::Error preparation_error(
    const std::string& tool_name,
    std::string reason,
    support::ErrorCode code = support::ErrorCode::Validation) {
    return support::make_error(
        code,
        "tool argument preparation failed",
        bounded_tool_argument_diagnostic(
            "Tool Argument Contract preparation failed at root for tool \"" +
            bounded_tool_argument_component(tool_name, 256) + "\": " +
            bounded_tool_argument_component(std::move(reason), 3500)));
}

[[nodiscard]] support::Error malformed_arguments_error(
    const std::string& tool_name,
    std::string parser_detail) {
    std::string diagnostic =
        "Tool Argument Contract preparation failed at root for tool \"" +
        bounded_tool_argument_component(tool_name, 256) +
        "\": arguments are malformed JSON";
    if (!parser_detail.empty()) {
        diagnostic += " (parser detail: " + std::move(parser_detail) + ")";
    }
    return support::make_error(support::ErrorCode::JsonParse,
            "tool argument preparation failed",
            support::bounded_redacted_text(std::move(diagnostic), 4096, " [diagnostic truncated]"));
}

[[nodiscard]] support::Expected<support::JsonValue> parse_and_clone_arguments(
    const ai::Tool& tool,
    const ai::ToolCallContent& call) {
    if (!call.arguments_valid) {
        return std::unexpected(malformed_arguments_error(
            tool.name,
            call.argument_error.value_or(std::string{})));
    }
    if (!call.raw_arguments.empty()) {
        auto parsed = support::read_json(call.raw_arguments);
        if (!parsed) {
            return std::unexpected(malformed_arguments_error(
                tool.name,
                parsed.error().detail));
        }
        return *parsed;
    }
    if (call.arguments) {
        return *call.arguments;
    }
    auto parsed = support::read_json("{}");
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return *parsed;
}

} // namespace

support::Expected<support::JsonValue> prepare_tool_arguments(
    const ai::Tool& tool,
    const ai::ToolCallContent& call) {
    auto arguments = parse_and_clone_arguments(tool, call);
    if (!arguments) {
        return std::unexpected(arguments.error());
    }

    CompilationState compilation_state;
    auto schema = compile_schema(
        tool.parameters,
        "schema",
        0,
        compilation_state,
        CompilationContext{});
    if (!schema) {
        return std::unexpected(preparation_error(tool.name, schema.error().detail));
    }

    coerce_value(*arguments, *schema);
    std::vector<ValidationFailure> failures;
    validate_value(*arguments, *schema, "root", failures);
    if (!failures.empty()) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "tool arguments do not satisfy their contract",
            validation_diagnostic(tool.name, failures)));
    }
    return arguments;
}

} // namespace cch::agent
