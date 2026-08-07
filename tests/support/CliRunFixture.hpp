#pragma once

#include "cli/CliParse.hpp"
#include "coding_agent/runtime/AsyncCliRuntime.hpp"
#include "ai/providers/FakeProvider.hpp"
#include "support/TempWorkspace.hpp"

#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace cch::tests {

/// In-process CLI run options. `args` are the argv tokens after the program
/// name; `cwd` chdirs for the run (restored after); `env` applies environment
/// overrides for the run (nullopt value unsets); `stdin_text` feeds the piped
/// stream; `models` injects the deterministic fake provider catalog (the
/// surface the deleted `--fake` flag used to drive).
struct CliRunOptions {
    std::vector<std::string> args;
    std::optional<std::filesystem::path> cwd;
    std::vector<std::pair<std::string, std::optional<std::string>>> env;
    std::string stdin_text;
    std::shared_ptr<ai::Models> models;
    bool stdin_is_terminal{false};
    bool stdout_is_terminal{false};
};

struct CliRunResult {
    int exit_code{0};
    std::string stdout_text;
    std::string stderr_text;
};

namespace detail {

class CliRunCwdGuard final {
public:
    explicit CliRunCwdGuard(const std::optional<std::filesystem::path>& target) {
        std::error_code ec;
        previous_ = std::filesystem::current_path(ec);
        if (target) {
            std::filesystem::current_path(*target, ec);
        }
    }
    ~CliRunCwdGuard() {
        if (!previous_.empty()) {
            std::error_code ec;
            std::filesystem::current_path(previous_, ec);
        }
    }
    CliRunCwdGuard(const CliRunCwdGuard&) = delete;
    CliRunCwdGuard& operator=(const CliRunCwdGuard&) = delete;

private:
    std::filesystem::path previous_;
};

class CliRunEnvGuard final {
public:
    explicit CliRunEnvGuard(
        const std::vector<std::pair<std::string, std::optional<std::string>>>& overrides) {
        guards_.reserve(overrides.size());
        for (const auto& [name, value] : overrides) {
            guards_.emplace_back(name, value);
        }
    }
    CliRunEnvGuard(const CliRunEnvGuard&) = delete;
    CliRunEnvGuard& operator=(const CliRunEnvGuard&) = delete;

private:
    class SingleGuard {
    public:
        SingleGuard(const std::string& name, const std::optional<std::string>& value)
            : name_(name) {
            if (const char* current = std::getenv(name_.c_str()); current != nullptr) {
                previous_ = current;
            }
            if (value) {
                setenv(name_.c_str(), value->c_str(), 1);
            } else {
                unsetenv(name_.c_str());
            }
        }
        SingleGuard(SingleGuard&& other) noexcept
            : name_(std::move(other.name_)), previous_(std::move(other.previous_)) {}
        ~SingleGuard() {
            if (previous_) {
                setenv(name_.c_str(), previous_->c_str(), 1);
            } else {
                unsetenv(name_.c_str());
            }
        }
        SingleGuard(const SingleGuard&) = delete;
        SingleGuard& operator=(const SingleGuard&) = delete;

    private:
        std::string name_;
        std::optional<std::string> previous_;
    };

    std::vector<SingleGuard> guards_;
};

} // namespace detail

inline CliRunResult run_cli(CliRunOptions options) {
    // Isolate ambient user configuration: when the test controls neither HOME
    // nor PI_CODING_AGENT_DIR, point the agent config directory at a fresh
    // temp root so no real user settings/sessions are consulted or written.
    const bool controls_config_dir = std::any_of(
        options.env.begin(), options.env.end(), [](const auto& entry) {
            return entry.first == "HOME" || entry.first == "PI_CODING_AGENT_DIR";
        });
    TempWorkspace isolated_config;
    if (!controls_config_dir) {
        options.env.emplace_back(
            "PI_CODING_AGENT_DIR", isolated_config.path().string());
    }

    detail::CliRunCwdGuard cwd_guard{options.cwd};
    detail::CliRunEnvGuard env_guard{options.env};

    std::vector<std::string> args{"cpp_harness"};
    args.insert(args.end(), options.args.begin(), options.args.end());
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& argument : args) {
        argv.push_back(argument.data());
    }

    std::istringstream input{options.stdin_text};
    std::ostringstream output;
    std::ostringstream error;

    std::shared_ptr<ai::Models> models = std::move(options.models);
    if (!models) {
        models = ai::providers::make_scripted_fake_models();
    }

    const cli::FrontendEnvironment environment{
        .stdin_is_terminal = options.stdin_is_terminal,
        .stdout_is_terminal = options.stdout_is_terminal,
        .native_tui_supported = true,
    };
    const int exit_code = cli::run_cli_entry(
        static_cast<int>(argv.size()),
        argv.data(),
        cli::CliStreams{input, output, error},
        cli::CliRuntimeOptions{
            .environment = environment,
            .environment_explicit = true,
            .models = std::move(models),
        });
    return CliRunResult{
        .exit_code = exit_code,
        .stdout_text = output.str(),
        .stderr_text = error.str(),
    };
}

} // namespace cch::tests
