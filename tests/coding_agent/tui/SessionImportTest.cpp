// The file half of pi v1.3.0 `/import` (spec #1044): pi's
// `AgentSessionRuntime.importFromJsonl` copies the source JSONL file into
// the session directory and opens it from there.
//
// These cases pin the parts of pi's rule that are pure facts — the path
// argument parsing (`getPathCommandArgument`), the destination derivation
// with pi's `-1`, `-2`, ... collision suffix, and the exclusive copy. The
// interactive confirmation and the runtime switch are covered by
// ImportSlashCommandInteractiveTest.cpp.

#include "coding_agent/tui/SessionImport.hpp"

#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

using namespace cch;

namespace {

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void write_file(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
}

} // namespace

TEST_CASE("the import path argument follows pi's quoted and unquoted spellings",
        "[coding_agent][tui][import][issue1044][spec]") {
    // pi `getPathCommandArgument`: a quoted argument keeps its inner text.
    CHECK(coding_agent::tui::path_command_argument("\"path/to/session.jsonl\"") ==
            std::optional<std::string>{"path/to/session.jsonl"});
    CHECK(coding_agent::tui::path_command_argument("\"path with spaces/session.jsonl\"") ==
            std::optional<std::string>{"path with spaces/session.jsonl"});

    // pi keeps an apostrophe inside an unquoted argument.
    CHECK(coding_agent::tui::path_command_argument("john's/session.jsonl") ==
            std::optional<std::string>{"john's/session.jsonl"});

    // pi stops an unquoted argument at the first whitespace.
    CHECK(coding_agent::tui::path_command_argument("session.jsonl extra") ==
            std::optional<std::string>{"session.jsonl"});

    // pi returns no argument for an empty or unterminated-quote argument.
    CHECK_FALSE(coding_agent::tui::path_command_argument("").has_value());
    CHECK_FALSE(coding_agent::tui::path_command_argument("\"unterminated").has_value());
}

TEST_CASE("the import destination keeps pi's collision suffix rule", "[coding_agent][tui][import][issue1044][spec]") {
    tests::TempWorkspace workspace;
    const auto session_dir = workspace.path() / "sessions";
    const auto source = workspace.path() / "source" / "session.jsonl";
    write_file(source, "{\"type\":\"session\"}\n");

    // pi: the destination is the session directory joined with the source's
    // base name.
    const auto first = coding_agent::tui::import_destination_path(session_dir, source);
    CHECK(first == session_dir / "session.jsonl");

    // pi: while the destination exists, the suffix grows `-1`, `-2`, ...
    std::filesystem::create_directories(session_dir);
    write_file(session_dir / "session.jsonl", "existing\n");
    const auto second = coding_agent::tui::import_destination_path(session_dir, source);
    CHECK(second == session_dir / "session-1.jsonl");

    write_file(session_dir / "session-1.jsonl", "existing\n");
    const auto third = coding_agent::tui::import_destination_path(session_dir, source);
    CHECK(third == session_dir / "session-2.jsonl");
}

TEST_CASE("the import destination keeps the source extension on the collision suffix",
        "[coding_agent][tui][import][issue1044][spec]") {
    tests::TempWorkspace workspace;
    const auto session_dir = workspace.path() / "sessions";
    std::filesystem::create_directories(session_dir);
    const auto source = workspace.path() / "a.b.jsonl";
    write_file(source, "{}\n");
    write_file(session_dir / "a.b.jsonl", "existing\n");

    // pi `path.parse`: the extension is the last dot of the base name, so the
    // suffix lands on the name and the extension is preserved.
    const auto destination = coding_agent::tui::import_destination_path(session_dir, source);
    CHECK(destination == session_dir / "a.b-1.jsonl");
}

TEST_CASE("an import source already stored in the session directory is opened in place",
        "[coding_agent][tui][import][issue1044][spec]") {
    tests::TempWorkspace workspace;
    const auto session_dir = workspace.path() / "sessions";
    std::filesystem::create_directories(session_dir);
    const auto source = session_dir / "already-here.jsonl";
    write_file(source, "{\"type\":\"session\"}\n");

    // pi `importFromJsonl`: a source that already resolves to the
    // destination is not copied again.
    const auto copied = coding_agent::tui::copy_imported_session(session_dir, source);
    REQUIRE(copied.has_value());
    CHECK(*copied == source);
    CHECK(read_file(source) == "{\"type\":\"session\"}\n");
}

TEST_CASE("an imported session file is copied into the session directory",
        "[coding_agent][tui][import][issue1044][spec]") {
    tests::TempWorkspace workspace;
    const auto session_dir = workspace.path() / "sessions";
    const auto source = workspace.path() / "source" / "session.jsonl";
    write_file(source, "{\"type\":\"session\"}\n");

    const auto copied = coding_agent::tui::copy_imported_session(session_dir, source);
    REQUIRE(copied.has_value());
    // pi creates the session directory when it is missing.
    CHECK(std::filesystem::exists(session_dir));
    CHECK(*copied == session_dir / "session.jsonl");
    CHECK(read_file(*copied) == "{\"type\":\"session\"}\n");
    // The source is left untouched.
    CHECK(read_file(source) == "{\"type\":\"session\"}\n");
}

TEST_CASE("an import never overwrites the session file it collides with",
        "[coding_agent][tui][import][issue1044][spec]") {
    tests::TempWorkspace workspace;
    const auto session_dir = workspace.path() / "sessions";
    std::filesystem::create_directories(session_dir);
    const auto source = workspace.path() / "source" / "session.jsonl";
    write_file(source, "imported\n");
    write_file(session_dir / "session.jsonl", "existing\n");

    const auto copied = coding_agent::tui::copy_imported_session(session_dir, source);
    REQUIRE(copied.has_value());
    CHECK(*copied == session_dir / "session-1.jsonl");
    // The existing file keeps its content; the import landed beside it.
    CHECK(read_file(session_dir / "session.jsonl") == "existing\n");
    CHECK(read_file(session_dir / "session-1.jsonl") == "imported\n");
}

TEST_CASE("the missing import file error carries pi's wording", "[coding_agent][tui][import][issue1044][spec]") {
    const auto error = coding_agent::tui::session_import_file_not_found_error("/tmp/missing-session.jsonl");
    // pi `SessionImportFileNotFoundError.message`.
    CHECK(error.message == "File not found: /tmp/missing-session.jsonl");
}
