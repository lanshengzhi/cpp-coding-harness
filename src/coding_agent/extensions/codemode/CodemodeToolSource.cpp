#include "coding_agent/extensions/codemode/CodemodeToolSource.hpp"

#include "coding_agent/extensions/codemode/CodemodeDeclaration.hpp"

#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <format>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace cch::coding_agent::extensions {

namespace {

/// The Tool `parameters` a declaration without an `inputSchema` gets: an empty
/// object schema, the shape providers and the Agent's JSON Schema validation
/// both require.
[[nodiscard]] support::JsonValue default_parameters() {
    return support::JsonValue::object_t{
            {"type", "object"},
            {"properties", support::JsonValue::object_t{}},
            {"additionalProperties", false},
    };
}

/// Convert one validated declaration into the passive extension Tool. The
/// declaration supplies the model-facing identity and schema; the execute
/// operation is deliberately not backed by a sandbox yet (#874), so it reports
/// the explicit "not yet executable" error rather than running the script or
/// returning a silent success.
[[nodiscard]] ExtensionTool to_extension_tool(CodemodeDeclaration declaration) {
    ExtensionTool tool;
    tool.definition.name = declaration.name;
    tool.definition.description = declaration.description;
    tool.definition.parameters = declaration.input_schema.value_or(default_parameters());
    tool.concurrency = agent::ToolConcurrency::Exclusive;
    std::string name = declaration.name;
    std::string source_path = declaration.source_path.string();
    tool.execute = [name = std::move(name), source_path = std::move(source_path)](support::JsonValue /*arguments*/,
                           std::stop_token /*stop_token*/) -> support::AsyncResult<ExtensionToolResult> {
        return support::AsyncResult<ExtensionToolResult>{support::Expected<ExtensionToolResult>{
                std::unexpected(support::make_error(support::ErrorCode::Validation,
                        std::format("codemode tool '{}' cannot run: codemode execution is not wired until #874", name),
                        std::format("the declaration at '{}' was loaded for discovery only; no sandbox "
                                    "runs its script yet (#874)",
                                source_path)))}};
    };
    return tool;
}

} // namespace

CodemodeToolSource::CodemodeToolSource(std::filesystem::path workspace) : workspace_(std::move(workspace)) {}

support::Expected<std::vector<ExtensionTool>> CodemodeToolSource::load_tools() {
    auto declarations = load_codemode_declarations(codemode_declaration_directory(workspace_));
    if (!declarations) {
        return std::unexpected(std::move(declarations.error()));
    }
    std::vector<ExtensionTool> tools;
    tools.reserve(declarations->size());
    for (auto& declaration : *declarations) {
        tools.push_back(to_extension_tool(std::move(declaration)));
    }
    return tools;
}

} // namespace cch::coding_agent::extensions
