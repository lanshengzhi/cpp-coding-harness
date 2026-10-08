#include "coding_agent/extensions/ExtensionToolRegistry.hpp"

#include <cch/support/AsyncResult.hpp>

#include <expected>
#include <string>
#include <utility>
#include <vector>

namespace cch::coding_agent::extensions {

/// Convert one extension-provided Tool into the Agent's Tool value (see the
/// header declaration).
agent::Tool convert_extension_tool(ExtensionTool tool) {
    agent::Tool agent_tool;
    agent_tool.definition = std::move(tool.definition);
    agent_tool.concurrency = tool.concurrency;
    agent_tool.prompt_snippet = std::move(tool.prompt_snippet);
    agent_tool.prompt_guidelines = std::move(tool.prompt_guidelines);
    const auto finish = [](agent::ToolExecuteResult::completion_type completion,
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
    };
    if (tool.context_execute) {
        agent_tool.execute = agent::ToolExecute{[execute = std::move(tool.context_execute), finish](
                                                        agent::ToolInvocation invocation,
                                                        std::stop_token stop_token,
                                                        agent::ToolUpdateSink update_sink) mutable
                                                        -> agent::ToolExecuteResult {
            return agent::ToolExecuteResult{agent::ToolExecuteResult::producer_type{
                    [execute = std::move(execute),
                            arguments = std::move(invocation.arguments),
                            context = ExtensionToolContext{.nested_calls = invocation.nested_calls,
                                    .call_id = std::move(invocation.call_id),
                                    .update_sink = std::move(update_sink)},
                            stop_token,
                            finish](agent::ToolExecuteResult::completion_type completion) mutable noexcept {
                        std::move(execute)(std::move(arguments), std::move(context), stop_token)
                                .start([completion = std::move(completion), finish](
                                               support::Expected<ExtensionToolResult> outcome) mutable noexcept {
                                    finish(std::move(completion), std::move(outcome));
                                });
                    }}};
        }};
    } else {
        agent_tool.execute = agent::ToolExecute{[execute = std::move(tool.execute), finish](
                                                        agent::ToolInvocation invocation,
                                                        std::stop_token stop_token,
                                                        agent::ToolUpdateSink /*update_sink*/) mutable
                                                        -> agent::ToolExecuteResult {
            return agent::ToolExecuteResult{agent::ToolExecuteResult::producer_type{
                    [execute = std::move(execute), arguments = std::move(invocation.arguments), stop_token, finish](
                            agent::ToolExecuteResult::completion_type completion) mutable noexcept {
                        std::move(execute)(std::move(arguments), stop_token)
                                .start([completion = std::move(completion), finish](
                                               support::Expected<ExtensionToolResult> outcome) mutable noexcept {
                                    finish(std::move(completion), std::move(outcome));
                                });
                    }}};
        }};
    }
    return agent_tool;
}

support::ExpectedVoid ExtensionToolRegistry::add(ExtensionTool tool) {
    if (!tool.execute && !tool.context_execute) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "cannot register an extension tool without an execute operation"));
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
    const bool default_inactive = !tool.default_active;
    tools_.emplace(name, std::move(tool));
    if (default_inactive) {
        default_inactive_.push_back(name);
    }
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

support::ExpectedVoid register_extension_tools(agent::ToolRegistry& registry, ExtensionToolRegistry extension_tools) {
    for (auto& tool : extension_tools.take_tools()) {
        const std::string name = tool.definition.name;
        if (registry.find(name) != nullptr) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation,
                    "extension tool name '" + name + "' collides with an existing tool",
                    "each tool name must be unique across built-in, custom, and extension tools"));
        }
        if (auto added = registry.add(convert_extension_tool(std::move(tool))); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return {};
}

} // namespace cch::coding_agent::extensions
