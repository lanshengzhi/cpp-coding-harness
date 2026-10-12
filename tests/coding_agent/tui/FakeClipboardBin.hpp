#pragma once

// Fake clipboard helper bin for the #993 clipboard tests: one temporary
// directory of executable POSIX helper scripts plus the invocation log those
// scripts append to. The copy policy and the Linux clipboard reader resolve
// helpers through the injected `ClipboardEnvironment::search_path`, never the
// process `PATH`, so every frozen helper branch is driven deterministically.
//
// Owner-local by design (CODING_STANDARDS §11.5): it exercises the private
// coding_agent clipboard seam and is used only by coding_agent TUI tests.

#include "support/TempWorkspace.hpp"

#include <filesystem>
#include <fstream>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui::testing {

/// One fake helper's behavior: what it prints, what it reports, and whether it
/// consumes its stdin (a clipboard *writer* receives the payload on stdin; a
/// *reader* is launched with no input at all).
struct FakeClipboardHelper {
    std::string name{};
    std::string output{};
    int exit_code{0};
    int delay_seconds{0};
    bool reads_stdin{false};
};

/// A temporary bin directory of fake helpers plus their invocation log. Each
/// installed script logs its own name, its arguments, and — for a writer — the
/// payload it read from stdin, then runs the requested behavior.
class FakeClipboardBin final {
public:
    FakeClipboardBin() { std::filesystem::create_directories(directory_); }

    [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }
    /// The injected helper resolution path; the process `PATH` is never used.
    [[nodiscard]] std::string search_path() const { return directory_.string(); }
    [[nodiscard]] const std::filesystem::path& log_path() const { return log_path_; }

    /// Install one helper from its behavior. A helper that only delays is
    /// `exec`ed: the delay process *is* the helper, so a cancellation or timeout
    /// kills exactly the launched child instead of leaving a skipped `sleep`.
    void install(const FakeClipboardHelper& helper) const {
        std::string body;
        if (helper.delay_seconds > 0) {
            body += helper.output.empty() && helper.exit_code == 0
                            ? std::format("exec sleep {}\n", helper.delay_seconds)
                            : std::format("sleep {}\n", helper.delay_seconds);
        }
        if (!helper.output.empty()) body += std::format("printf '%s' {}\n", shell_quote(helper.output));
        body += std::format("exit {}\n", helper.exit_code);
        install_script(helper.name, std::move(body), helper.reads_stdin);
    }

    /// Install a helper whose body runs after the log line, for helpers that
    /// inspect their arguments (`wslpath`) or spawn background holders.
    void install_script(std::string_view name, std::string body, bool reads_stdin = false) const {
        const auto quoted_log = shell_quote(log_path_.string());
        std::string script = "#!/bin/sh\n";
        script += std::format("printf '%s' {} >> {}\n", shell_quote(std::string{name}), quoted_log);
        script += std::format("for argument in \"$@\"; do printf ' %s' \"$argument\" >> {}; done\n", quoted_log);
        if (reads_stdin) script += std::format("printf ' ' >> {}\ncat >> {}\n", quoted_log, quoted_log);
        script += std::format("printf '\\0' >> {}\n", quoted_log);
        script += std::move(body);

        const auto path = directory_ / std::string{name};
        {
            std::ofstream output{path, std::ios::binary | std::ios::trunc};
            output << script;
        }
        std::error_code permission_error;
        std::filesystem::permissions(path,
                std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                        std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
                        std::filesystem::perms::others_exec,
                std::filesystem::perm_options::replace,
                permission_error);
    }

    /// Every logged invocation in order, as `<command> <args...>`; a writer's
    /// line continues with the payload it read from stdin.
    [[nodiscard]] std::vector<std::string> invocations() const {
        std::vector<std::string> lines;
        std::ifstream input{log_path_, std::ios::binary};
        for (std::string line; std::getline(input, line, '\0');) {
            if (!line.empty()) {
                lines.push_back(std::move(line));
            }
        }
        return lines;
    }

    /// Quote one shell word for a script body written by a test.
    [[nodiscard]] static std::string shell_quote(std::string_view value) {
        std::string quoted{"'"};
        for (const char character : value) {
            if (character == '\'') {
                quoted += "'\\''";
            } else {
                quoted.push_back(character);
            }
        }
        quoted.push_back('\'');
        return quoted;
    }

private:
    tests::TempWorkspace workspace_{};
    std::filesystem::path directory_{workspace_.path() / "clipboard-bin"};
    std::filesystem::path log_path_{workspace_.path() / "clipboard-invocations.log"};
};

} // namespace cch::coding_agent::tui::testing
