#pragma once

#include <cch/util/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <filesystem>
#include <string>

namespace cch::harness {

struct AsyncFileReadResult {
    std::string content;
    bool truncated{false};
};

struct AsyncFileWriteResult {
    std::size_t bytes_written{0};
};

struct AsyncFileEditResult {
    std::string old_preview;
    std::string new_preview;
};

struct AsyncShellResult {
    int exit_code{-1};
    std::string output;
    bool timed_out{false};
};

class AsyncExecutionEnv {
public:
    virtual ~AsyncExecutionEnv() = default;

    [[nodiscard]] virtual const std::filesystem::path& workspace() const = 0;
    [[nodiscard]] virtual bool bash_enabled() const = 0;

    [[nodiscard]] virtual boost::asio::awaitable<util::Expected<AsyncFileReadResult>> read_file(
        std::string path,
        int offset,
        int limit) = 0;
    [[nodiscard]] virtual boost::asio::awaitable<util::Expected<AsyncFileWriteResult>> write_file(
        std::string path,
        std::string content,
        bool create_parents) = 0;
    [[nodiscard]] virtual boost::asio::awaitable<util::Expected<AsyncFileEditResult>> edit_file(
        std::string path,
        std::string old_text,
        std::string new_text) = 0;
    [[nodiscard]] virtual boost::asio::awaitable<util::Expected<AsyncShellResult>> run_shell(
        std::string command,
        std::chrono::milliseconds timeout) = 0;
};

} // namespace cch::harness
