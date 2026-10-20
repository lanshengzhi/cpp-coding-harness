#pragma once

// Long-lived MCP server connection over newline-delimited compact JSON (pi
// `packages/mcp/src/transports/stdio.ts`): one child process group, one
// stdin/stdout pipe pair, one JSON-RPC message per `\n`-terminated line. The
// concrete process/framing machinery is private to cch_mcp; callers use this
// interface.

#include <cch/mcp/Client.hpp>
#include <cch/mcp/StdioServerConfig.hpp>

#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <memory>

namespace cch::mcp {

/// Launch `config`, run the MCP `initialize` handshake, and return the
/// connected client. A missing executable, an empty command, or an invalid
/// handshake response is reported through the shared error channel — there is
/// no fallback server and no silent empty connection.
class StdioClient : public Client {
public:
    [[nodiscard]] static support::AsyncResult<std::shared_ptr<StdioClient>> connect(StdioServerConfig config);

    /// Whether the child has exited or the transport was closed. A closed
    /// client is not reusable.
    [[nodiscard]] virtual bool closed() const noexcept = 0;
};

} // namespace cch::mcp
