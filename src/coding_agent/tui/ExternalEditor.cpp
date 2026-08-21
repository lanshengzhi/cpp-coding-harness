#include "coding_agent/tui/ExternalEditor.hpp"

#include "coding_agent/tui/ErrorPresentation.hpp"

#include <cch/tui/Tui.hpp>
#include <cch/support/Error.hpp>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace cch::coding_agent::tui {
namespace {

/// Fresh hex suffix for the `pi-editor-*` temp directory (pi's
/// `mkdtempSync` random name).
[[nodiscard]] std::string random_suffix() {
    std::random_device random;
    std::array<std::uint8_t, 6> bytes{};
    for (auto& byte : bytes) byte = static_cast<std::uint8_t>(random());
    std::string value;
    for (const auto byte : bytes) value += std::format("{:02x}", byte);
    return value;
}

/// pi `external-editor.ts`: split the command on spaces (the editor command
/// is a plain argv string).
[[nodiscard]] std::vector<std::string> split_command(std::string_view command) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (begin <= command.size()) {
        const auto space = command.find(' ', begin);
        const auto end = space == std::string_view::npos ? command.size() : space;
        if (end > begin) parts.emplace_back(command.substr(begin, end - begin));
        if (space == std::string_view::npos) break;
        begin = space + 1;
    }
    return parts;
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

/// pi `editInExternalEditor`: spawn the editor with the file path appended
/// and inherited stdio, then await its exit code. Returns the exit code, or
/// `std::nullopt` when the spawn itself failed (pi resolves `null`).
[[nodiscard]] std::optional<int> run_editor(
    const std::vector<std::string>& parts,
    const std::filesystem::path& file_path) {
    if (parts.empty()) return std::nullopt;
    const auto pid = ::fork();
    if (pid < 0) return std::nullopt;
    if (pid == 0) {
        std::vector<char*> argv;
        argv.reserve(parts.size() + 2);
        for (const auto& part : parts) {
            argv.push_back(const_cast<char*>(part.c_str()));
        }
        const auto path = file_path.string();
        argv.push_back(const_cast<char*>(path.c_str()));
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return std::nullopt;
    }
    if (!WIFEXITED(status)) return std::nullopt;
    return WEXITSTATUS(status);
}

} // namespace

std::string external_editor_command() {
    if (const char* visual = std::getenv("VISUAL");
        visual != nullptr && visual[0] != '\0') {
        return visual;
    }
    if (const char* editor = std::getenv("EDITOR");
        editor != nullptr && editor[0] != '\0') {
        return editor;
    }
    return "nano";
}

boost::asio::awaitable<support::Expected<std::optional<std::string>>>
edit_in_external_editor(std::string command, std::string content) {
    const auto parts = split_command(command);
    if (parts.empty()) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "external editor command is empty"));
    }

    std::error_code error;
    auto base = std::filesystem::temp_directory_path(error);
    if (error || base.empty()) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Process,
            "external editor temporary directory is unavailable",
            error.message()));
    }
    // pi `mkdtempSync(join(tmpdir(), "pi-editor-"))`.
    std::filesystem::path directory;
    for (std::size_t attempt = 0; attempt < 16; ++attempt) {
        auto candidate = base / std::format("pi-editor-{}", random_suffix());
        if (std::filesystem::create_directory(candidate, error) && !error) {
            directory = std::move(candidate);
            break;
        }
        error.clear();
    }
    if (directory.empty()) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Process,
            "could not create the external editor temporary directory"));
    }

    const auto file_path = directory / "prompt.md";
    // Best-effort cleanup, exactly like pi's `finally` rmSync.
    struct DirectoryGuard {
        std::filesystem::path path;
        ~DirectoryGuard() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } guard{directory};

    {
        std::ofstream output(file_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Process,
                "could not write the external editor prompt file"));
        }
        output << content;
    }

    // pi prints the launch notice to stdout before spawning.
    std::printf(
        "Launching external editor: %s\nPi will resume when the editor exits.\n",
        command.c_str());
    std::fflush(stdout);

    const auto exit_code = run_editor(parts, file_path);
    if (!exit_code || *exit_code != 0) {
        co_return support::Expected<std::optional<std::string>>{std::nullopt};
    }

    auto edited = read_file(file_path);
    if (!edited.empty() && edited.back() == '\n') {
        edited.pop_back();
    }
    co_return support::Expected<std::optional<std::string>>{std::move(edited)};
}

boost::asio::awaitable<support::Expected<std::optional<std::string>>>
run_external_editor_flow(cch::tui::Tui& tui, std::string content) {
    const auto command = external_editor_command();
    const auto stopped = tui.stop();
    if (!stopped) {
        co_return std::unexpected(presentation_error(
            stopped.error(),
            "Native TUI external editor stop failed"));
    }
    auto result = co_await edit_in_external_editor(command, std::move(content));
    // Restore the TUI on every exit path (pi's `finally`).
    if (auto started = tui.start(); !started) {
        co_return std::unexpected(presentation_error(
            started.error(),
            "Native TUI external editor resume failed"));
    }
    if (auto rendered = tui.render(); !rendered) {
        co_return std::unexpected(startup_error(rendered.error()));
    }
    if (!result || !*result) {
        co_return std::optional<std::string>{std::nullopt};
    }
    co_return std::optional<std::string>{std::move(**result)};
}

} // namespace cch::coding_agent::tui
