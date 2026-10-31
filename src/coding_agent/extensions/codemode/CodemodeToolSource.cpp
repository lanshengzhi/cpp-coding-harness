#include "coding_agent/extensions/codemode/CodemodeToolSource.hpp"

#include "coding_agent/extensions/codemode/CodemodeCatalog.hpp"
#include "coding_agent/extensions/codemode/CodemodeCatalogState.hpp"
#include "coding_agent/extensions/codemode/CodemodeDiscovery.hpp"
#include "coding_agent/extensions/codemode/CodemodeTool.hpp"

#include <cch/codemode/Declarations.hpp>
#include <cch/codemode/Sandbox.hpp>
#include <cch/codemode/Source.hpp>

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
#include <unordered_set>
#include <utility>
#include <vector>

namespace cch::coding_agent::extensions {

namespace {

/// Lazy holder for the codemode guest (pi `loadCodemodeExecutor`): the sandbox
/// is created on the first execute and reused for the source's lifetime, so a
/// session that never calls codemode never loads the guest module.
struct GuestHolder {
    std::filesystem::path path;
    std::shared_ptr<codemode::CodemodeSandbox> sandbox;

    /// pi `createDiscoveryGlobals`: the three discovery globals every codemode
    /// sandbox binds. They are construction-time names; each run's backing tool
    /// set is resolved from the live callable loadout at call time.
    [[nodiscard]] static std::vector<codemode::CodemodeGlobal> discovery_globals() {
        std::vector<codemode::CodemodeGlobal> globals;
        for (const auto& name : codemode_discovery_globals()) {
            globals.push_back(codemode::CodemodeGlobal{.name = name, .spread = true});
        }
        return globals;
    }

    [[nodiscard]] support::Expected<codemode::CodemodeSandbox*> ensure() {
        if (!sandbox) {
            auto created = codemode::CodemodeSandbox::create(path, discovery_globals());
            if (!created) {
                return std::unexpected(std::move(created.error()));
            }
            sandbox = std::shared_ptr<codemode::CodemodeSandbox>{std::move(*created)};
        }
        return sandbox.get();
    }
};

/// A script's `@options.timeout_ms` is its deadline; without one pi's script
/// sandbox runs with no timeout (`timeoutMs ?? Number.POSITIVE_INFINITY`), not
/// the library's 300 s default. `CodemodeLimits.timeout == 0` disables it.
[[nodiscard]] codemode::CodemodeLimits limits_for(const codemode::CodemodeSourceOptions& options) {
    codemode::CodemodeLimits limits;
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
        const codemode::CodemodeRunResult& run, std::chrono::steady_clock::duration elapsed) {
    ExtensionToolResult result;
    result.is_error = run.error.has_value();
    std::string header = result.is_error ? "Script failed" : "Script completed";
    header += "\nWall time " + wall_time_seconds(elapsed) + " seconds\nOutput:\n";
    result.content.push_back(ai::text_content(std::move(header)));
    for (const auto& item : run.output) {
        if (item.kind == codemode::CodemodeOutputItem::Kind::Image) {
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

/// pi `getCodemodeCallableTools`: every given tool except codemode itself
/// (its `exposure` is `model-only`, so scripts must not start other scripts).
/// With the session's catalog, the set is the exposure-filtered callable
/// loadout (`codemode`/`deferred` plus active `direct`); without one, every
/// registered tool is callable.
[[nodiscard]] std::vector<LoadoutTool> callable_loadout(
        const std::vector<ai::Tool>& tools, const CodemodeCatalogState* catalog) {
    std::vector<LoadoutTool> loadout;
    loadout.reserve(tools.size());
    if (catalog == nullptr) {
        for (const auto& tool : tools) {
            loadout.push_back(LoadoutTool{.definition = tool});
        }
        return callable_tools(loadout);
    }
    std::set<std::string, std::less<>> callable_names;
    for (const auto& entry : catalog->callable())
        callable_names.insert(entry.definition.name);
    for (const auto& tool : tools) {
        if (!callable_names.contains(tool.name)) continue;
        const LoadoutTool* entry = catalog->find(tool.name);
        if (entry == nullptr) continue;
        LoadoutTool listed = *entry;
        // The live definition (not the registration snapshot) is what the
        // script must call: a replacement schema from a re-registered MCP tool
        // reaches the sandbox exactly as it reaches the model.
        listed.definition = tool;
        loadout.push_back(std::move(listed));
    }
    return loadout;
}

/// The `toolsJson` entries the prelude binds `tools.<jsName>` and
/// `tools["<raw name>"]` to.
[[nodiscard]] std::vector<codemode::CodemodeToolDescriptor> to_descriptors(const std::vector<LoadoutTool>& tools) {
    std::vector<codemode::CodemodeToolDescriptor> descriptors;
    descriptors.reserve(tools.size());
    for (const auto& tool : tools) {
        const CodemodeDeclaration declaration = to_codemode_declaration(tool.definition, tool.guidelines);
        codemode::CodemodeToolDescriptor descriptor;
        descriptor.name = tool.definition.name;
        descriptor.js_name = codemode::to_codemode_identifier(tool.definition.name);
        descriptor.description = codemode::render_tool_sample(ai::Tool{
                .name = declaration.name,
                .description = declaration.description,
                .parameters = declaration.input_schema,
                .output_schema = declaration.output_schema,
        });
        descriptors.push_back(std::move(descriptor));
    }
    return descriptors;
}

[[nodiscard]] bool is_discovery_global(std::string_view name) {
    return name == "searchTools" || name == "describeTool" || name == "describeNamespace";
}

/// pi `toScriptValue` (extensions/codemode/execute.ts): a call resolves to
/// its structured content when the tool declares an output schema and the
/// result carries structured content — error results included, so an MCP
/// CallToolResult keeps its `isError` flag — otherwise to its text content;
/// a failure rejects with the text, or pi's `Tool "<name>" failed` fallback.
[[nodiscard]] support::Expected<std::string> script_value_from(
        const agent::AsyncToolExecutionResult& outcome, bool declares_output_schema, std::string_view name) {
    if (declares_output_schema && outcome.details.has_value()) {
        if (auto written = support::write_json(*outcome.details)) {
            return support::Expected<std::string>{std::move(*written)};
        }
    }
    if (!outcome.is_error) {
        auto written = support::write_json(support::JsonValue{ai::text_from_content(outcome.content)});
        if (written) return support::Expected<std::string>{std::move(*written)};
        return std::unexpected(std::move(written.error()));
    }
    std::string text = ai::text_from_content(outcome.content);
    if (text.empty()) {
        text = std::format("Tool \"{}\" failed", name);
    }
    return std::unexpected(support::make_error(support::ErrorCode::Validation, std::move(text)));
}

} // namespace

CodemodeToolSource::CodemodeToolSource(
        std::filesystem::path guest_wasm_path, std::shared_ptr<const CodemodeCatalogState> catalog)
    : guest_wasm_path_(std::move(guest_wasm_path)), catalog_(std::move(catalog)) {}

support::Expected<std::vector<ExtensionTool>> CodemodeToolSource::load_tools() {
    auto holder = std::make_shared<GuestHolder>();
    holder->path = guest_wasm_path_;

    ExtensionTool tool;
    tool.definition = codemode_tool_definition();
    // pi `defaultActive: false` (extensions/codemode/index.ts): the model must
    // not see `codemode` unless activation names it — pi's `--tools` selection,
    // an explicit `setActiveTools`, or the MCP `codemode` exposure.
    tool.default_active = false;
    // pi `exposure: "model-only"`: scripts must not start other scripts.
    tool.exposure = ToolExposure::ModelOnly;
    tool.prompt_snippet = std::string{kCodemodePromptSnippet};
    tool.prompt_guidelines = codemode_prompt_guidelines();
    tool.concurrency = agent::ToolConcurrency::Exclusive;
    tool.context_execute = [holder, catalog = catalog_](support::JsonValue arguments,
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
        auto parsed = codemode::parse_codemode_source(*source_text);
        if (!parsed) {
            return failed("Script error:\n" + parsed.error().message);
        }

        std::vector<LoadoutTool> callable;
        if (context.nested_calls != nullptr) {
            callable = callable_loadout(context.nested_calls->tools(), catalog.get());
        }
        // pi `toScriptValue`'s output-schema guard: only tools declaring an
        // output schema resolve a script call to their structured content.
        auto schema_names = std::make_shared<std::unordered_set<std::string>>();
        for (const auto& tool : callable) {
            if (tool.definition.output_schema.has_value()) {
                schema_names->insert(tool.definition.name);
            }
        }
        auto discovery = std::make_shared<CodemodeDiscovery>(callable);
        auto descriptors = std::make_shared<std::vector<codemode::CodemodeToolDescriptor>>(to_descriptors(callable));
        const std::string caller_id = context.call_id;

        return support::detail::make_async_result(
                [holder,
                        parsed = std::move(*parsed),
                        discovery,
                        descriptors,
                        schema_names,
                        caller_id,
                        nested_calls = context.nested_calls,
                        stop_token]() mutable -> boost::asio::awaitable<support::Expected<ExtensionToolResult>> {
                    auto guest = holder->ensure();
                    if (!guest) {
                        co_return std::unexpected(support::make_error(
                                support::ErrorCode::Validation, "Script failed\n" + guest.error().message));
                    }
                    const auto started = std::chrono::steady_clock::now();

                    auto handler = [discovery, nested_calls, schema_names, caller_id](std::string_view name,
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
                                        schema_names,
                                        caller_id,
                                        name = std::string{name},
                                        args = std::move(args),
                                        signal]() mutable -> boost::asio::awaitable<support::Expected<std::string>> {
                                    auto outcome = co_await support::detail::await_async_result(
                                            nested_calls->execute(caller_id, name, std::move(args), signal));
                                    if (!outcome) {
                                        co_return support::Expected<std::string>{std::unexpected(outcome.error())};
                                    }
                                    co_return script_value_from(*outcome, schema_names->contains(name), name);
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
