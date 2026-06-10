#pragma once

#include "../../include/cch/util/Error.hpp"

#include <boost/process/v1.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <array>
#include <map>
#include <sstream>
#include <string>
#include <thread>

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
    [[nodiscard]] virtual Expected<ProcessResult> run(const ProcessRequest& request) = 0;
};

class DefaultProcessRunner : public ProcessRunner {
public:
    [[nodiscard]] Expected<ProcessResult> run(const ProcessRequest& request) override {
        namespace bp = boost::process::v1;
        try {
            bp::ipstream output;
            bp::ipstream error;
            bp::environment child_environment = boost::this_process::environment();
            if (request.use_explicit_environment || !request.environment.empty()) {
                child_environment = bp::environment{};
                for (const auto& [key, value] : request.environment) {
                    child_environment[key] = value;
                }
            }
            bp::child child(
                bp::search_path("bash"),
                "-lc",
                request.command,
                bp::start_dir = request.working_directory.string(),
                bp::std_out > output,
                bp::std_err > error,
                child_environment);

            auto drain = [](bp::ipstream& stream, std::string& sink, std::size_t max_bytes, std::size_t max_lines, bool& truncated) {
                std::size_t bytes = 0;
                std::size_t lines = 0;
                bool line_limit_reached = max_lines == 0;
                std::array<char, 4096> buffer{};
                while (stream) {
                    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                    const auto got = stream.gcount();
                    if (got <= 0) {
                        break;
                    }

                    std::size_t offset = 0;
                    const auto available = static_cast<std::size_t>(got);
                    if (!line_limit_reached && bytes < max_bytes) {
                        while (offset < available && bytes < max_bytes && !line_limit_reached) {
                            const char ch = buffer[offset++];
                            sink.push_back(ch);
                            ++bytes;
                            if (ch == '\n') {
                                ++lines;
                                if (lines >= max_lines) {
                                    line_limit_reached = true;
                                }
                            }
                        }
                    }
                    if (offset < available) {
                        truncated = true;
                    }
                }
            };
            std::string stdout_data;
            std::string stderr_data;
            bool stdout_truncated = false;
            bool stderr_truncated = false;
            std::thread stdout_thread(drain, std::ref(output), std::ref(stdout_data), request.max_output_bytes, request.max_output_lines, std::ref(stdout_truncated));
            std::thread stderr_thread(drain, std::ref(error), std::ref(stderr_data), request.max_output_bytes, request.max_output_lines, std::ref(stderr_truncated));

            const auto deadline = std::chrono::steady_clock::now() + request.timeout;
            ProcessResult result;
            while (child.running()) {
                if (request.timeout.count() > 0 && std::chrono::steady_clock::now() >= deadline) {
                    result.timed_out = true;
                    child.terminate();
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            child.wait();
            if (stdout_thread.joinable()) {
                stdout_thread.join();
            }
            if (stderr_thread.joinable()) {
                stderr_thread.join();
            }
            result.exit_code = child.exit_code();
            result.output = stdout_data + stderr_data;
            if (stdout_truncated || stderr_truncated) {
                result.output += "\n[output truncated]";
            }
            return result;
        } catch (const std::exception& e) {
            return std::unexpected(make_error(ErrorCode::Process, "process execution failed", e.what()));
        }
    }
};

} // namespace cch::util
