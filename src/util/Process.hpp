#pragma once

#include <cch/util/Error.hpp>
#include "OutputLimiter.hpp"

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace cch::util {

struct ProcessRequest {
    std::string command;
    std::filesystem::path working_directory;
    std::chrono::milliseconds timeout{30000};
    std::map<std::string, std::string> environment;
    bool use_explicit_environment{false};
    OutputLimit output_limit;

    /// Called with stdout chunks as they are produced.
    std::optional<std::move_only_function<void(std::string_view)>> on_stdout;
    /// Called with stderr chunks as they are produced.
    std::optional<std::move_only_function<void(std::string_view)>> on_stderr;
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
    [[nodiscard]] virtual boost::asio::awaitable<Expected<ProcessResult>> run(ProcessRequest request) = 0;
};

class DefaultAsyncProcessRunner final : public AsyncProcessRunner {
public:
    [[nodiscard]] boost::asio::awaitable<Expected<ProcessResult>> run(ProcessRequest request) override;
};

} // namespace cch::util
