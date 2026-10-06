#include "coding_agent/extensions/ExtensionToolRegistry.hpp"

#include <cch/support/AsyncResult.hpp>

#include <expected>
#include <string>
#include <utility>
#include <vector>

namespace cch::coding_agent::extensions {

namespace {

/// Convert one extension-provided Tool into the Agent's Tool value. The
/// definition, prompt metadata, and concurrency policy carry over unchanged;
/// the extension execute operation runs when the Agent executor invokes the
/// tool and its terminal outcome is mapped onto the Agent Tool result.
[[nodiscard]] agent::Tool to_agent_tool(ExtensionTool tool) {
    agent::Tool agent_tool;
    agent_tool.definition = std::move(tool.definition);
    agent_tool.concurrency = tool.concurrency;
    agent_tool.prompt_snippet = std::move(tool.prompt_snippet);
    agent_tool.prompt_guidelines = std::move(tool.prompt_guidelines);
    agent_tool.execute = agent::ToolExecute{
            [execute = std::move(tool.execute)](
                    agent::ToolInvocation invocation,
                    std::stop_token stop_token,
                    agent::ToolUpdateSink /*update_sink*/) mutable -> agent::ToolExecuteResult {
                return agent::ToolExecuteResult{agent::ToolExecuteResult::producer_type{
                        [execute = std::move(execute),
                                arguments = std::move(invocation.arguments),
                                stop_token](agent::ToolExecuteResult::completion_type completion) mutable noexcept {
                            std::move(execute)(std::move(arguments), stop_token)
                                    .start([completion = std::move(completion)](
                                                   support::Expected<ExtensionToolResult> outcome) mutable noexcept {
                                        if (!outcome) {
                                            completion(std::unexpected(std::move(outcome.error())));
                                            return;
                                        }
                                        completion(agent::AsyncToolExecutionResult{
                                                .content = std::move(outcome->content),
                                                .details = std::move(outcome->details),
                                                .is_error = outcome->is_error,
                                        });
                                    });
                        }}};
            }};
    return agent_tool;
}

} // namespace

support::ExpectedVoid ExtensionToolRegistry::add(ExtensionTool tool) {
    if (!tool.execute) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                "cannot register an extension tool without an execute operation"));
    }
    if (tool.definition.name.empty()) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "cannot register an extension tool without a name"));
    }
    const std::string name = tool.definition.name;
    if (tools_.contains(name)) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                "duplicate extension tool name: '" + name + "'",
                "each extension tool must have a unique name"));
    }
    tools_.emplace(name, std::move(tool));
    return {};
}

ExtensionTool* ExtensionToolRegistry::find(const std::string& name) {
    auto it = tools_.find(name);
    return it == tools_.end() ? nullptr : &it->second;
}

const ExtensionTool* ExtensionToolRegistry::find(const std::string& name) const {
    auto it = tools_.find(name);
    return it == tools_.end() ? nullptr : &it->second;
}

std::vector<ExtensionTool> ExtensionToolRegistry::take_tools() {
    std::vector<ExtensionTool> tools;
    tools.reserve(tools_.size());
    for (auto& [name, tool] : tools_) {
        tools.push_back(std::move(tool));
    }
    tools_.clear();
    return tools;
}

support::ExpectedVoid load_extension_tools(
        ExtensionToolRegistry& registry, std::span<ExtensionToolSource* const> sources) {
    for (auto* source : sources) {
        auto loaded = source->load_tools();
        if (!loaded) {
            return std::unexpected(std::move(loaded.error()));
        }
        for (auto& tool : *loaded) {
            if (auto added = registry.add(std::move(tool)); !added) {
                return std::unexpected(std::move(added.error()));
            }
        }
    }
    return {};
}

support::ExpectedVoid register_extension_tools(
        agent::ToolRegistry& registry, ExtensionToolRegistry extension_tools) {
    for (auto& tool : extension_tools.take_tools()) {
        const std::string name = tool.definition.name;
        if (registry.find(name) != nullptr) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation,
                    "extension tool name '" + name + "' collides with an existing tool",
                    "each tool name must be unique across built-in, custom, and extension tools"));
        }
        if (auto added = registry.add(to_agent_tool(std::move(tool))); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return {};
}

} // namespace cch::coding_agent::extensions
