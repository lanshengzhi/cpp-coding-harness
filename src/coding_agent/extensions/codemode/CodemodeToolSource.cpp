#include "coding_agent/extensions/codemode/CodemodeToolSource.hpp"

#include "coding_agent/extensions/codemode/CodemodeSandbox.hpp"
#include "coding_agent/extensions/codemode/CodemodeSource.hpp"
#include "coding_agent/extensions/codemode/CodemodeTool.hpp"

#include <cch/ai/Content.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <chrono>
#include <format>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
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
    // pi `executeCodemode`: `${ok ? "Script completed" : "Script failed"}\nWall
    // time ${wallTime} seconds\nOutput:\n`. Built as one string so the first
    // line carries no trailing space.
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

} // namespace

CodemodeToolSource::CodemodeToolSource(std::filesystem::path guest_wasm_path)
    : guest_wasm_path_(std::move(guest_wasm_path)) {}

support::Expected<std::vector<ExtensionTool>> CodemodeToolSource::load_tools() {
    auto holder = std::make_shared<GuestHolder>();
    holder->path = guest_wasm_path_;

    ExtensionTool tool;
    tool.definition = codemode_tool_definition();
    tool.prompt_snippet = std::string{kCodemodePromptSnippet};
    tool.prompt_guidelines = codemode_prompt_guidelines();
    tool.concurrency = agent::ToolConcurrency::Exclusive;
    tool.execute = [holder](support::JsonValue arguments,
                           std::stop_token stop_token) -> support::AsyncResult<ExtensionToolResult> {
        const auto* code = arguments.get_if<support::JsonValue::object_t>();
        if (code == nullptr) {
            return failed("codemode expects a JSON object with a `code` string argument.");
        }
        const auto it = code->find("code");
        const auto* source_text = it == code->end() ? nullptr : it->second.get_if<std::string>();
        if (source_text == nullptr) {
            return failed("codemode expects a `code` string argument.");
        }
        auto parsed = parse_codemode_source(*source_text);
        if (!parsed) {
            return failed("Script error:\n" + parsed.error().message);
        }
        auto guest = holder->ensure();
        if (!guest) {
            return failed("Script failed\n" + guest.error().message);
        }
        const auto started = std::chrono::steady_clock::now();
        CodemodeRunResult run = (*guest)->run(parsed->code, {}, limits_for(parsed->options), stop_token);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        return ready(to_extension_result(run, elapsed));
    };
    std::vector<ExtensionTool> contributed;
    contributed.push_back(std::move(tool));
    return contributed;
}

} // namespace cch::coding_agent::extensions
