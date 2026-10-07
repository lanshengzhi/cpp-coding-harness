#pragma once

// The `pike mcp` CLI subcommand (spec #882, ticket #884): pi's `runMcpCommand`
// (`packages/coding-agent/src/extensions/mcp/cli.ts` at 7c10bd43). It reads and
// writes the same `mcp.json` the `/mcp` manager uses and, like pi, connects
// every enabled server for `list` and drives the `mcp-auth.json`-backed OAuth
// sign-in for `login`. The connection seam is injectable so the command's
// output and exit codes are testable without a live server.

#include "coding_agent/mcp/McpAuthStore.hpp"
#include "coding_agent/mcp/McpConfigFile.hpp"

#include <cch/support/AsyncResult.hpp>

#include <boost/asio/awaitable.hpp>

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::cli {

/// One server's connection outcome for `pike mcp list` (pi `ServerReport`'s
/// `state`, `tools`, and `error`).
struct McpProbeResult {
    enum class State { Connected, NeedsAuth, Failed };
    State state{State::Failed};
    /// The server-side tool names `tools/list` reported, in server order.
    std::vector<std::string> tools;
    /// pi `connection.error` when the server did not connect.
    std::string error;
};

/// The connection seam `pike mcp list`/`login` drive (pi `createConnection`).
/// The production implementation connects the configured stdio or streamable
/// HTTP server, handshakes, and lists its tools; tests inject a scripted one.
class McpServerProbe {
public:
    virtual ~McpServerProbe() = default;
    [[nodiscard]] virtual boost::asio::awaitable<McpProbeResult> probe(
            const coding_agent::mcp::McpConfigEntry& entry, const std::filesystem::path& agent_dir) = 0;
};

/// Inputs `run_mcp_command` needs from the environment. `probe` and
/// `open_browser` default to the production behavior; tests set `probe` to a
/// scripted one.
struct McpCommandOptions {
    /// The workspace whose project `mcp.json` (`<cwd>/.pi/mcp.json`) is read
    /// for `--local` and for the trust note.
    std::filesystem::path cwd;
    /// The Agent Config Directory holding the global `mcp.json`, the
    /// `mcp-auth.json` credential store, and the project-trust store.
    std::filesystem::path agent_dir;
    /// The project's trust decision (pi `ProjectTrustStore.get(cwd) === true`):
    /// the project `mcp.json` is loaded and `--local` writes into it only when
    /// true.
    bool project_trusted{false};
    std::ostream* output{nullptr};
    std::ostream* error{nullptr};
    /// The login paste-input stream (pi's `stdin`); only read when
    /// `stdin_is_terminal`.
    std::istream* input{nullptr};
    bool stdin_is_terminal{false};
    std::shared_ptr<McpServerProbe> probe{nullptr};
    /// pi `openBrowser`: opens the authorization URL. Null leaves the URL to
    /// the user (the production CLI passes the platform opener).
    std::function<void(std::string_view)> open_browser{nullptr};
};

/// pi's `HELP` for `pi mcp`, with `pike` substituted for the app name.
[[nodiscard]] std::string mcp_help_text();

/// Run `pike mcp <args>` (pi `runMcpCommand`) and return the process exit code.
/// `args` are the tokens after `mcp`. `help`/`-h` and an empty command line
/// print the help and return 0; an unknown command returns 1.
[[nodiscard]] int run_mcp_command(const std::vector<std::string>& args, McpCommandOptions options);

} // namespace cch::cli
