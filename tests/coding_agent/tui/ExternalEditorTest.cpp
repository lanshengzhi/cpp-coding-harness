#include "coding_agent/tui/ExternalEditor.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

using namespace cch;

namespace {

/// Run one external-editor round to completion on a detached io_context.
[[nodiscard]] support::Expected<std::optional<std::string>> run_editor_flow(
        const std::string& command, const std::string& content) {
    boost::asio::io_context io;
    std::optional<support::Expected<std::optional<std::string>>> result;
    boost::asio::co_spawn(
            io,
            [&]() -> boost::asio::awaitable<void> {
                result = co_await coding_agent::tui::edit_in_external_editor(command, content);
            },
            boost::asio::detached);
    io.run();
    REQUIRE(result.has_value());
    return std::move(*result);
}

/// A fake editor script that rewrites its file argument with the given
/// printf payload (the payload carries the literal bytes to write).
[[nodiscard]] std::string write_editor_script(
        const tests::TempWorkspace& workspace, const std::string& printf_payload) {
    const auto script = workspace.path() / "fake-editor.sh";
    {
        std::ofstream output(script, std::ios::binary | std::ios::trunc);
        output << "#!/bin/sh\nprintf '" << printf_payload << "' > \"$1\"\n";
    }
    std::error_code error;
    std::filesystem::permissions(script, std::filesystem::perms::owner_all, error);
    return script.string();
}

} // namespace

TEST_CASE("external editor strips a UTF-8 BOM from the resumed content", "[coding_agent][tui][external-editor][spec]") {
    tests::TempWorkspace workspace;
    // The fake editor writes a BOM-prefixed body with a trailing newline:
    // pi `external-editor.ts` resumes
    // `stripBom(readFileSync(...)).replace(/\n$/, "")`.
    const auto command = write_editor_script(workspace, "\\357\\273\\277edited body\\n");

    const auto result = run_editor_flow(command, "initial content");

    REQUIRE(result.has_value());
    // Without the strip the prompt would resume with the BOM bytes attached.
    CHECK(**result == "edited body");
}

TEST_CASE("external editor resumes plain edited content unchanged", "[coding_agent][tui][external-editor][spec]") {
    tests::TempWorkspace workspace;
    const auto command = write_editor_script(workspace, "plain edit\\n");

    const auto result = run_editor_flow(command, "initial content");

    REQUIRE(result.has_value());
    REQUIRE(result->has_value());
    CHECK(**result == "plain edit");
}
