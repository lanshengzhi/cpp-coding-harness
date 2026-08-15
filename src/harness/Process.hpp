#pragma once

#include <cch/support/Error.hpp>
#include "harness/OutputLimiter.hpp"

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace cch::harness {

struct ProcessRequest {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::filesystem::path working_directory;
    std::chrono::milliseconds timeout{30000};
    std::map<std::string, std::string> environment;
    bool use_explicit_environment{false};
    OutputLimit output_limit;
    std::stop_token stop_token;

    /// Called with stdout chunks as they are produced. A throwing callback is
    /// contained: it is deactivated after its first exception and the failure
    /// surfaces as a Process error result, while the pipe keeps draining.
    std::optional<std::move_only_function<void(std::string_view)>> on_stdout;
    /// Called with stderr chunks as they are produced. Failure containment is
    /// the same as on_stdout.
    std::optional<std::move_only_function<void(std::string_view)>> on_stderr;
    /// Redirect stderr into the stdout pipe at spawn so one consumer observes
    /// both streams in process emission order. Honored only on Linux/macOS;
    /// elsewhere stderr stays a separate stream. The stderr capture and
    /// on_stderr callback stay empty when the merge is applied.
    bool merge_stderr{false};
};

struct ProcessResult {
    int exit_code{-1};
    /// Combined stdout+stderr (compatibility field).
    std::string output;
    /// Separate stdout stream.
    std::string stdout_output;
    /// Separate stderr stream.
    std::string stderr_output;
    bool timed_out{false};
    /// Per-stream truncation flags.
    bool stdout_truncated{false};
    bool stderr_truncated{false};
};

class AsyncProcessRunner {
public:
    virtual ~AsyncProcessRunner() = default;
    [[nodiscard]] virtual boost::asio::awaitable<support::Expected<ProcessResult>> run(ProcessRequest request) = 0;
};

class DefaultAsyncProcessRunner final : public AsyncProcessRunner {
public:
    [[nodiscard]] boost::asio::awaitable<support::Expected<ProcessResult>> run(ProcessRequest request) override;
};

} // namespace cch::harness
