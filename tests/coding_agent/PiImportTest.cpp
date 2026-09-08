#include "coding_agent/compat/pi/PiImport.hpp"

#include "support/EnvVarGuard.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace {

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output);
    output << content;
    REQUIRE(output.good());
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("pi import copies config and session history without changing the source",
        "[coding_agent][compat-pi][issue626]") {
    cch::tests::TempWorkspace temp;
    const auto source = temp.path() / "pi-agent";
    const auto destination = temp.path() / "pike-agent";
    const auto session = source / "sessions" / "--workspace--" / "session.jsonl";
    write_file(source / "settings.json", "{\"theme\":\"dark\"}\n");
    write_file(source / "models.json", "{\"models\":[]}\n");
    write_file(session,
            "{\"type\":\"session\",\"version\":3,\"id\":\"session-1\"}\n"
            "{\"type\":\"message\",\"id\":\"entry-1\"}\n");

    const auto imported = cch::coding_agent::compat::pi::import_state({
            .source_directory = source,
            .destination_directory = destination,
    });
    REQUIRE(imported);
    CHECK(imported->files_copied == 3);
    CHECK(imported->directories_copied == 2);
    CHECK(read_file(destination / "settings.json") == "{\"theme\":\"dark\"}\n");
    CHECK(read_file(destination / "models.json") == "{\"models\":[]}\n");
    CHECK(read_file(destination / "sessions" / "--workspace--" / "session.jsonl") == read_file(session));
    CHECK(std::filesystem::exists(source / "settings.json"));
    CHECK(std::filesystem::exists(session));
}

TEST_CASE("pi import refuses to overwrite an existing product namespace", "[coding_agent][compat-pi][issue626]") {
    cch::tests::TempWorkspace temp;
    const auto source = temp.path() / "pi-agent";
    const auto destination = temp.path() / "pike-agent";
    write_file(source / "settings.json", "pi state\n");
    write_file(destination / "settings.json", "product state\n");

    const auto imported = cch::coding_agent::compat::pi::import_state({
            .source_directory = source,
            .destination_directory = destination,
    });
    REQUIRE_FALSE(imported);
    CHECK(imported.error().message.find("already exists") != std::string::npos);
    CHECK(read_file(destination / "settings.json") == "product state\n");
    CHECK(read_file(source / "settings.json") == "pi state\n");
}

TEST_CASE("pi import rejects an unknown session entry shape before creating the destination",
        "[coding_agent][compat-pi][issue626]") {
    cch::tests::TempWorkspace temp;
    const auto source = temp.path() / "pi-agent";
    const auto destination = temp.path() / "pike-agent";
    write_file(source / "sessions" / "broken.jsonl",
            "{\"type\":\"session\",\"version\":3}\n"
            "{\"type\":\"future_entry\"}\n");

    const auto imported = cch::coding_agent::compat::pi::import_state({
            .source_directory = source,
            .destination_directory = destination,
    });
    REQUIRE_FALSE(imported);
    CHECK(imported.error().message.find("unknown entry shape") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(destination));
}

TEST_CASE("pi import default source follows the explicit pi home layout", "[coding_agent][compat-pi][issue626]") {
    const cch::tests::EnvVarGuard home{"HOME", std::string{"/tmp/import-home"}};
    CHECK(cch::coding_agent::compat::pi::default_source_directory() ==
            std::filesystem::path{"/tmp/import-home/.pi/agent"});
}
