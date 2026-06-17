#pragma once

#include "../../include/cch/util/Error.hpp"

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <filesystem>
#include <map>
#include <string>

namespace cch::util {

struct ProcessRequest {
    std::string command;
    std::filesystem::path working_directory;
    std::chrono::milliseconds timeout{30000};
    std::map<std::string, std::string> environment;
    bool use_explicit_environment{false};
    std::size_t max_output_bytes{50 * 1024};
    std::size_t max_output_lines{2000};
};

struct ProcessResult {
    int exit_code{-1};
    std::string output;
    bool timed_out{false};
};

class ProcessRunner {
public:
    virtual ~ProcessRunner() = default;
    [[nodiscard]] virtual boost::asio::awaitable<Expected<ProcessResult>> run(ProcessRequest request) = 0;
};

class DefaultProcessRunner : public ProcessRunner {
public:
    [[nodiscard]] boost::asio::awaitable<Expected<ProcessResult>> run(ProcessRequest request) override;
};

} // namespace cch::util
