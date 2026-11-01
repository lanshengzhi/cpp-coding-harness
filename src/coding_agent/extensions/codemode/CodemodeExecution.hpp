#pragma once

#include "coding_agent/extensions/ExtensionTool.hpp"

#include <cch/coding_agent/environment/FileSystem.hpp>
#include <cch/coding_agent/session/SessionEntry.hpp>
#include <cch/codemode/Sandbox.hpp>
#include <cch/codemode/Source.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/JsonValue.hpp>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::extensions {

/// pi `CodemodeNestedCall`: one nested tool call a script made, as the run
/// reports it in `details.calls` and in a failure's call summary.
struct CodemodeCall {
    /// pi `id`: the nested call's id (`<caller>/<n>`). A call the run cancelled
    /// before it settled keeps the `<caller>/?` placeholder, exactly as pi's
    /// handler does when `executeTool` rejects.
    std::string id;
    std::string name;
    /// pi `args`: the call argument's compact JSON, cut to pi's 200-character
    /// preview; empty when the call passed no argument.
    std::string args;
    /// pi `status`: `ok`, `error`, or `cancelled`.
    std::string status;
    /// pi `durationMs`: present once the call settled (ok, error, cancelled).
    std::optional<double> duration_ms;
    /// pi `error`: the failure text, cut to pi's 500-character preview.
    std::optional<std::string> error;
};

/// The application Session facilities one codemode run uses. pi's
/// `executeCodemode` reaches the same three things through
/// `ctx.sessionManager` and the output-file utility: the active branch (the
/// persistent store), `appendEntry` (store writes), and a place to write
/// spilled output and saved images. Bound by `AgentSession` once its store and
/// commitment channel exist; a source constructed without one keeps the
/// no-session behavior (empty store, dropped writes, no saved files).
struct CodemodeSessionState {
    /// The Session's filesystem capability, used for output spill files and
    /// saved images. Null when the caller has no filesystem.
    std::shared_ptr<harness::AsyncFileSystem> filesystem;
    /// pi `ctx.sessionManager.getBranch()`: the active branch entries,
    /// leaf-to-root, from which `load()` replays the store.
    std::function<support::Expected<std::vector<harness::session::SessionEntry>>()> branch;
    /// pi `options.appendEntry`: persist one `codemode-store` write set on the
    /// session's ordered commitment line.
    std::function<support::ExpectedVoid(std::string, support::JsonValue)> append_store;

    [[nodiscard]] bool persistent() const { return static_cast<bool>(branch) && static_cast<bool>(append_store); }
};

/// pi `readCodemodeStore`: apply every `codemode-store` entry of a branch, from
/// the root to the leaf (the store applies each entry's deletes, then its
/// sets), and return the resulting values.
[[nodiscard]] std::map<std::string, support::JsonValue> codemode_store_from_branch(
        const std::vector<harness::session::SessionEntry>& branch);

/// pi `previewArgs`: the call argument's compact JSON, cut to pi's 200-unit
/// preview; empty when the call passed no argument.
[[nodiscard]] std::string codemode_preview_args(std::string_view arguments_json);

/// pi's nested-call error preview: the failure text, or `Tool "<name>" failed`
/// when the tool produced none, cut to pi's 500-unit preview.
[[nodiscard]] std::string codemode_error_preview(std::string_view error_text, std::string_view tool_name);

/// pi `executeCodemode`'s result presentation: format the script's output
/// (numbered text items, one console block), append the returned value or the
/// `Script error:` block with its call summary, apply the output budget with
/// its spill file, save each image next to a path label, and build the
/// model-facing content and `details`. A successful run also persists its
/// store writes through `session`.
[[nodiscard]] support::AsyncResult<ExtensionToolResult> present_codemode_run(const codemode::CodemodeRunResult& run,
        std::vector<CodemodeCall> calls,
        const codemode::CodemodeSourceOptions& options,
        std::chrono::steady_clock::duration elapsed,
        const CodemodeSessionState* session,
        std::stop_token stop_token);

} // namespace cch::coding_agent::extensions
