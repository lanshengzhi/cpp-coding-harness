#include "coding_agent/extensions/codemode/CodemodeToolSource.hpp"

#include "coding_agent/extensions/codemode/CodemodeDiscovery.hpp"
#include "coding_agent/extensions/codemode/CodemodeSandbox.hpp"
#include "coding_agent/extensions/codemode/CodemodeSource.hpp"
#include "coding_agent/extensions/codemode/CodemodeTool.hpp"

#include "support/AsyncResultBridge.hpp"
#include "support/Json.hpp"

#include <cch/agent/NestedToolCalls.hpp>
#include <cch/ai/Content.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <boost/asio/awaitable.hpp>

#include <algorithm>
#include <chrono>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::extensions {

namespace {

/// Lazy holder for the codemode guest (pi `loadCodemodeExecutor`): the sandbox
/// is created on the first execute and reused for the source's lifetime, so a
/// session that never calls codemode never loads the guest module.
struct GuestHolder {
    std::filesystem::path path;
    std::shared_ptr<CodemodeSandbox> sandbox;

    [[nodiscard]] support::Expected<CodemodeSandbox*> ensure() {
        if (!sandbox) {
            auto created = CodemodeSandbox::create(path);
            if (!created) {
                return std::unexpected(std::move(created.error()));
            }
            sandbox = std::shared_ptr<CodemodeSandbox>{std::move(*created)};
        }
        return sandbox.get();
    }
};

/// A script's `@options.timeout_ms` is its deadline; without one pi's script
/// sandbox runs with no timeout (`timeoutMs ?? Number.POSITIVE_INFINITY`), not
/// the library's 300 s default. `CodemodeLimits.timeout == 0` disables it.
[[nodiscard]] CodemodeLimits limits_for(const CodemodeSourceOptions& options) {
    CodemodeLimits limits;
    limits.timeout = options.timeout_ms.has_value() ? std::chrono::milliseconds{*options.timeout_ms}
                                                    : std::chrono::milliseconds{0};
    return limits;
}

[[nodiscard]] std::string wall_time_seconds(std::chrono::steady_clock::duration elapsed) {
    const double seconds = std::chrono::duration<double>(elapsed).count();
    return std::format("{:.1f}", seconds);
}

/// Map the sandbox's terminal outcome onto pi's `executeCodemode` content
/// channel: the `Script completed`/`Script failed` header, the script's
/// `text()`/`image()` output, the returned value appended like `text()` on
/// success, and a `Script error:` block on failure.
[[nodiscard]] ExtensionToolResult to_extension_result(
        const CodemodeRunResult& run, std::chrono::steady_clock::duration elapsed) {
    ExtensionToolResult result;
    result.is_error = run.error.has_value();
    std::string header = result.is_error ? "Script failed" : "Script completed";
    header += "\nWall time " + wall_time_seconds(elapsed) + " seconds\nOutput:\n";
    result.content.push_back(ai::text_content(std::move(header)));
    for (const auto& item : run.output) {
        if (item.kind == CodemodeOutputItem::Kind::Image) {
            result.content.push_back(ai::image_content(item.data, item.mime_type));
        } else {
            result.content.push_back(ai::text_content(item.data));
        }
    }
    if (run.error.has_value()) {
        result.content.push_back(ai::text_content("Script error:\n" + run.error->message));
    } else if (run.value_json.has_value()) {
        result.content.push_back(ai::text_content(*run.value_json));
    }
    return result;
}

[[nodiscard]] support::AsyncResult<ExtensionToolResult> ready(ExtensionToolResult result) {
    return support::AsyncResult<ExtensionToolResult>{support::Expected<ExtensionToolResult>{std::move(result)}};
}

[[nodiscard]] support::AsyncResult<ExtensionToolResult> failed(std::string message) {
    ExtensionToolResult result;
    result.is_error = true;
    result.content.push_back(ai::text_content(std::move(message)));
    return ready(std::move(result));
}

/// pi `getCodemodeCallableTools`: every registered tool except codemode itself
/// (its `exposure` is `model-only`, so scripts must not start other scripts).
[[nodiscard]] std::vector<ai::Tool> callable_tools(const std::vector<ai::Tool>& tools) {
    std::vector<ai::Tool> callable;
    callable.reserve(tools.size());
    for (const auto& tool : tools) {
        if (tool.name == kCodemodeToolName) continue;
        callable.push_back(tool);
    }
    return callable;
}

/// The `toolsJson` entries the prelude binds `tools.<jsName>` and
/// `tools["<raw name>"]` to.
[[nodiscard]] std::vector<CodemodeToolDescriptor> to_descriptors(const std::vector<ai::Tool>& tools) {
    std::vector<CodemodeToolDescriptor> descriptors;
    descriptors.reserve(tools.size());
    for (const auto& tool : tools) {
        CodemodeToolDescriptor descriptor;
        descriptor.name = tool.name;
        descriptor.js_name = to_codemode_identifier(tool.name);
        descriptor.description = render_tool_sample(tool);
        descriptors.push_back(std::move(descriptor));
    }
    return descriptors;
}

[[nodiscard]] bool is_discovery_global(std::string_view name) {
    return name == "searchTools" || name == "describeTool" || name == "describeNamespace";
}

/// pi `toScriptValue`: a tool call resolves to its structured content when it
/// carries one, otherwise to its text content; a failure rejects with the text.
[[nodiscard]] support::Expected<std::string> script_value_from(const agent::AsyncToolExecutionResult& outcome) {
    if (!outcome.is_error) {
        if (outcome.details.has_value()) {
            auto written = support::write_json(*outcome.details);
            if (written) return std::move(*written);
        }
        auto written = support::write_json(support::JsonValue{ai::text_from_content(outcome.content)});
        if (written) return std::move(*written);
        return std::unexpected(std::move(written.error()));
    }
    std::string text = ai::text_from_content(outcome.content);
    if (text.empty()) text = "Tool call failed";
    return std::unexpected(support::make_error(support::ErrorCode::Validation, std::move(text)));
}

} // namespace

CodemodeToolSource::CodemodeToolSource(std::filesystem::path guest_wasm_path)
    : guest_wasm_path_(std::move(guest_wasm_path)) {}

support::Expected<std::vector<ExtensionTool>> CodemodeToolSource::load_tools() {
    auto holder = std::make_shared<GuestHolder>();
    holder->path = guest_wasm_path_;

    ExtensionTool tool;
    tool.definition = codemode_tool_definition();
    // pi `defaultActive: false` (extensions/codemode/index.ts): the model must
    // not see `codemode` unless activation names it — pi's `--tools` selection,
    // an explicit `setActiveTools`, or the MCP `codemode` exposure.
    tool.default_active = false;
    tool.prompt_snippet = std::string{kCodemodePromptSnippet};
    tool.prompt_guidelines = codemode_prompt_guidelines();
    tool.concurrency = agent::ToolConcurrency::Exclusive;
    tool.context_execute = [holder](support::JsonValue arguments,
                                   ExtensionToolContext context,
                                   std::stop_token stop_token) -> support::AsyncResult<ExtensionToolResult> {
        const auto* fields = arguments.get_if<support::JsonValue::object_t>();
        if (fields == nullptr) {
            return failed("codemode expects a JSON object with a `code` string argument.");
        }
        const auto it = fields->find("code");
        const auto* source_text = it == fields->end() ? nullptr : it->second.get_if<std::string>();
        if (source_text == nullptr) {
            return failed("codemode expects a `code` string argument.");
        }
        auto parsed = parse_codemode_source(*source_text);
        if (!parsed) {
            return failed("Script error:\n" + parsed.error().message);
        }

        std::vector<ai::Tool> callable;
        if (context.nested_calls != nullptr) {
            callable = callable_tools(context.nested_calls->tools());
        }
        auto discovery = std::make_shared<CodemodeDiscovery>(callable);
        auto descriptors = std::make_shared<std::vector<CodemodeToolDescriptor>>(to_descriptors(callable));
        const std::string caller_id = context.call_id;

        return support::detail::make_async_result(
                [holder,
                        parsed = std::move(*parsed),
                        discovery,
                        descriptors,
                        caller_id,
                        nested_calls = context.nested_calls,
                        stop_token]() mutable -> boost::asio::awaitable<support::Expected<ExtensionToolResult>> {
                    auto guest = holder->ensure();
                    if (!guest) {
                        co_return std::unexpected(support::make_error(
                                support::ErrorCode::Validation, "Script failed\n" + guest.error().message));
                    }
                    const auto started = std::chrono::steady_clock::now();

                    auto handler = [discovery, nested_calls, caller_id](std::string_view name,
                                           std::string_view arguments_json,
                                           std::stop_token signal) -> support::AsyncResult<std::string> {
                        if (is_discovery_global(name)) {
                            auto parsed_args = support::read_json(arguments_json);
                            support::JsonValue args = parsed_args ? std::move(*parsed_args) : support::JsonValue{};
                            auto outcome = discovery->handle(name, args);
                            if (outcome) {
                                return support::AsyncResult<std::string>{
                                        support::Expected<std::string>{std::move(*outcome)}};
                            }
                            return support::AsyncResult<std::string>{
                                    support::Expected<std::string>{std::unexpected(outcome.error())}};
                        }
                        if (nested_calls == nullptr) {
                            return support::AsyncResult<std::string>{support::Expected<std::string>{
                                    std::unexpected(support::make_error(support::ErrorCode::Validation,
                                            std::format("tool '{}' is not available to the codemode sandbox", name)))}};
                        }
                        auto parsed_args = support::read_json(arguments_json);
                        support::JsonValue args = parsed_args ? std::move(*parsed_args) : support::JsonValue{};
                        return support::detail::make_async_result(
                                [nested_calls,
                                        caller_id,
                                        name = std::string{name},
                                        args = std::move(args),
                                        signal]() mutable -> boost::asio::awaitable<support::Expected<std::string>> {
                                    auto outcome = co_await support::detail::await_async_result(
                                            nested_calls->execute(caller_id, name, std::move(args), signal));
                                    if (!outcome) {
                                        co_return support::Expected<std::string>{std::unexpected(outcome.error())};
                                    }
                                    co_return script_value_from(*outcome);
                                });
                    };

                    auto run = co_await support::detail::await_async_result((*guest)->run(std::move(parsed.code),
                            std::move(*descriptors),
                            limits_for(parsed.options),
                            stop_token,
                            std::move(handler)));
                    if (!run) {
                        co_return std::unexpected(
                                support::make_error(support::ErrorCode::Validation, run.error().message));
                    }
                    const auto elapsed = std::chrono::steady_clock::now() - started;
                    co_return to_extension_result(*run, elapsed);
                });
    };
    std::vector<ExtensionTool> contributed;
    contributed.push_back(std::move(tool));
    return contributed;
}

} // namespace cch::coding_agent::extensions
