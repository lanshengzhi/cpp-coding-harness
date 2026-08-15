#pragma once

#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace cch::coding_agent::runtime {

/// One terminal outcome from the private Session-owned User Shell. Streamed
/// output is treated as untrusted: the Agent Session runtime sanitizes,
/// redacts, bounds, and spills it from the update sink, so the result carries
/// only the terminal process status.
struct UserShellResult {
    std::optional<int> exit_code{std::nullopt};
    bool cancelled{false};
};

using UserShellUpdateSink =
    std::move_only_function<support::ExpectedVoid(std::string_view)>;

/// Private, independently owned direct-user Shell capability. Implementations
/// receive the raw command only at process launch and deliver stdout/stderr in
/// callback-arrival order through one move-only update sink.
class AsyncUserShell {
public:
    virtual ~AsyncUserShell() = default;

    [[nodiscard]] virtual support::AsyncResult<UserShellResult> execute(
        std::string command,
        UserShellUpdateSink update_sink,
        std::stop_token stop_token) = 0;
};

} // namespace cch::coding_agent::runtime
