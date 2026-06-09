#pragma once

#include "Result.hpp"

#include <boost/process.hpp>

#include <chrono>
#include <filesystem>
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
};

struct ProcessResult {
    int exit_code{-1};
    std::string output;
    bool timed_out{false};
};

class ProcessRunner {
public:
    virtual ~ProcessRunner() = default;
    [[nodiscard]] virtual Result<ProcessResult> run(const ProcessRequest& request) = 0;
};

class DefaultProcessRunner : public ProcessRunner {
public:
    [[nodiscard]] Result<ProcessResult> run(const ProcessRequest& request) override {
        namespace bp = boost::process;
        try {
            bp::ipstream output;
            bp::ipstream error;
            bp::environment child_environment = boost::this_process::environment();
            if (!request.environment.empty()) {
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
            result.exit_code = child.exit_code();

            std::ostringstream collected;
            std::string line;
            while (std::getline(output, line)) {
                collected << line << '\n';
            }
            while (std::getline(error, line)) {
                collected << line << '\n';
            }
            result.output = collected.str();
            return Result<ProcessResult>::success(std::move(result));
        } catch (const std::exception& e) {
            return Result<ProcessResult>::failure(e.what());
        }
    }
};

} // namespace cch::util
