#include "coding_agent/extensions/codemode/CodemodeToolSource.hpp"

#include "coding_agent/extensions/codemode/CodemodeDeclaration.hpp"
#include "coding_agent/extensions/codemode/CodemodeSandbox.hpp"

#include <cch/ai/Content.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#ifndef CCH_SOURCE_DIR
#define CCH_SOURCE_DIR ""
#endif

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

/// Map the sandbox's terminal outcome onto the extension Tool result: the
/// output items become content blocks, a script error becomes an error result,
/// and the return value is appended last so a model call sees it.
[[nodiscard]] ExtensionToolResult to_extension_result(const CodemodeRunResult& run) {
    ExtensionToolResult result;
    for (const auto& item : run.output) {
        if (item.kind == CodemodeOutputItem::Kind::Image) {
            result.content.push_back(ai::image_content(item.data, item.mime_type));
        } else {
            result.content.push_back(ai::text_content(item.data));
        }
    }
    if (run.error.has_value()) {
        result.is_error = true;
        result.content.push_back(ai::text_content(run.error->message));
        return result;
    }
    if (run.value_json.has_value()) {
        result.content.push_back(ai::text_content(*run.value_json));
    }
    if (result.content.empty()) {
        result.content.push_back(ai::text_content("(no output)"));
    }
    return result;
}

/// Convert one validated declaration into an extension Tool whose execute runs
/// the declared script inside the shared sandbox. `sandbox` is shared by every
/// tool of one source; the executions are serialized by the Tool's Exclusive
/// concurrency and the sandbox instance is per run.
[[nodiscard]] ExtensionTool to_extension_tool(
        std::shared_ptr<CodemodeSandbox> sandbox, CodemodeDeclaration declaration) {
    ExtensionTool tool;
    tool.definition.name = declaration.name;
    tool.definition.description = declaration.description;
    tool.definition.parameters = declaration.input_schema.value_or(default_parameters());
    tool.concurrency = agent::ToolConcurrency::Exclusive;
    std::string name = declaration.name;
    std::string code = declaration.source.code;
    CodemodeSourceOptions options = declaration.source.options;
    tool.execute = [sandbox = std::move(sandbox), name = std::move(name), code = std::move(code), options](
                           support::JsonValue /*arguments*/,
                           std::stop_token stop_token) -> support::AsyncResult<ExtensionToolResult> {
        CodemodeLimits limits;
        if (options.timeout_ms.has_value()) {
            limits.timeout = std::chrono::milliseconds{*options.timeout_ms};
        }
        // The declared script runs self-contained in this slice: it has no
        // session-tool surface yet (see the fixture README / #874 notes).
        CodemodeRunResult run = sandbox->run(code, {}, limits, stop_token);
        if (run.error.has_value() && run.error->kind == CodemodeError::Kind::Sandbox) {
            return support::AsyncResult<ExtensionToolResult>{support::Expected<ExtensionToolResult>{
                    std::unexpected(support::make_error(support::ErrorCode::Validation,
                            std::format("codemode tool '{}': {}", name, run.error->message),
                            "the declared script could not run inside the codemode sandbox"))}};
        }
        return support::AsyncResult<ExtensionToolResult>{
                support::Expected<ExtensionToolResult>{to_extension_result(run)}};
    };
    return tool;
}

} // namespace

std::filesystem::path default_codemode_guest_wasm_path() {
    if (const char* override_path = std::getenv("PIKE_CODEMODE_WASM");
            override_path != nullptr && *override_path != '\0') {
        return std::filesystem::path{override_path};
    }
    return std::filesystem::path{CCH_SOURCE_DIR} / "fixtures" / "codemode" / "quickjs" / "quickjs.wasm";
}

CodemodeToolSource::CodemodeToolSource(std::filesystem::path workspace, std::filesystem::path guest_wasm_path)
    : workspace_(std::move(workspace)), guest_wasm_path_(std::move(guest_wasm_path)) {}

support::Expected<std::vector<ExtensionTool>> CodemodeToolSource::load_tools() {
    auto declarations = load_codemode_declarations(codemode_declaration_directory(workspace_));
    if (!declarations) {
        return std::unexpected(std::move(declarations.error()));
    }
    if (declarations->empty()) return std::vector<ExtensionTool>{};

    auto sandbox = CodemodeSandbox::create(guest_wasm_path_);
    if (!sandbox) return std::unexpected(std::move(sandbox.error()));
    auto shared_sandbox = std::shared_ptr<CodemodeSandbox>{std::move(*sandbox)};

    std::vector<ExtensionTool> tools;
    tools.reserve(declarations->size());
    for (auto& declaration : *declarations) {
        tools.push_back(to_extension_tool(shared_sandbox, std::move(declaration)));
    }
    return tools;
}

} // namespace cch::coding_agent::extensions
