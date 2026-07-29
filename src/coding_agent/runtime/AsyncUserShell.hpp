#pragma once

#include <cch/util/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace cch::coding_agent::runtime {

/// One terminal outcome from the private Session-owned User Shell. Output is
/// treated as untrusted until the Agent Session runtime sanitizes and bounds it.
struct UserShellResult {
    std::string output;
    std::optional<int> exit_code{std::nullopt};
    bool cancelled{false};
    bool truncated{false};
    std::optional<std::string> full_output_path;
    /// A retained-output artifact failure does not erase the terminal outcome.
    std::optional<util::Error> artifact_error;
};

using UserShellUpdateSink =
    std::move_only_function<util::ExpectedVoid(std::string_view)>;

/// Private, independently owned direct-user Shell capability. Implementations
/// receive the raw command only at process launch and deliver stdout/stderr in
/// callback-arrival order through one move-only update sink.
class AsyncUserShell {
public:
    virtual ~AsyncUserShell() = default;

    [[nodiscard]] virtual boost::asio::awaitable<util::Expected<UserShellResult>> execute(
        std::string command,
        UserShellUpdateSink update_sink,
        std::stop_token stop_token) = 0;
};

} // namespace cch::coding_agent::runtime
