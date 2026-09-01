#pragma once

#include <cch/agent/AgentTool.hpp>
#include <cch/agent/harness/FileSystem.hpp>
#include <cch/agent/harness/Shell.hpp>

#include <memory>
#include <optional>
#include <string>

namespace cch::tools {

/// Live session facts the model Bash Tool exposes as `PI_*` environment
/// variables (pi `core/tools/bash.ts` `resolveSpawnContext`): the session id
/// and file, the active model's provider/id, and the active thinking level,
/// read at execution time from the session's live state. The session runtime
/// owns and updates this holder; the bash tool is executor-confined with the
/// runtime, so no synchronization is required. Absent fields map to pi's
/// deleted (unset) variables: the execution environment shadows them with an
/// empty value.
struct BashSessionEnvironment {
    /// pi `ctx.sessionManager.getSessionId()` → `PI_SESSION_ID` (always set).
    std::string session_id{};
    /// pi `getSessionFile()` → `PI_SESSION_FILE` (only when persisted).
    std::optional<std::string> session_file{};
    /// pi `ctx.model.provider` → `PI_PROVIDER` (empty for the placeholder
    /// "unknown" model, matching pi's truthy-model guard).
    std::string provider{};
    /// pi `ctx.model.id` → `PI_MODEL`.
    std::string model{};
    /// pi `ctx.thinkingLevel` → `PI_REASONING_LEVEL` (empty when unset).
    std::optional<std::string> reasoning_level{};
};

[[nodiscard]] agent::Tool make_async_read_file_tool(std::shared_ptr<harness::AsyncFileSystem> filesystem);
[[nodiscard]] agent::Tool make_async_write_file_tool(std::shared_ptr<harness::AsyncFileSystem> filesystem);
[[nodiscard]] agent::Tool make_async_edit_tool(std::shared_ptr<harness::AsyncFileSystem> filesystem);
/// The model-facing Bash Tool. `shell` runs the command; `filesystem` writes
/// the redacted full-output spill file when streamed output exceeds the
/// retained bound. `session_environment` (when provided) exposes the live
/// `PI_*` session facts on every executed command; without it the tool
/// injects no environment (pi `exposeSessionEnvironment: false`).
[[nodiscard]] agent::Tool make_async_bash_tool(std::shared_ptr<harness::AsyncShell> shell,
        std::shared_ptr<harness::AsyncFileSystem> filesystem,
        std::shared_ptr<BashSessionEnvironment> session_environment = {});

} // namespace cch::tools
