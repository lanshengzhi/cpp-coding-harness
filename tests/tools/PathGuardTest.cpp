#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../src/tools/PathGuard.hpp"
#include "../support/TempWorkspace.hpp"

#include <filesystem>

using namespace cch;

TEST_CASE("PathGuard reads an existing file inside workspace", "[tools][pathguard][u6]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "hello world");
    auto guard = tools::PathGuard::create(workspace.path());
    REQUIRE(guard);

    auto content = guard->read_existing_file("note.txt");
    REQUIRE(content);
    CHECK(*content == "hello world");
}

TEST_CASE("PathGuard rejects final symlink when reading", "[tools][pathguard][u6]") {
    tests::TempWorkspace workspace;
    workspace.write("real.txt", "secret");
    std::filesystem::create_symlink(workspace.path() / "real.txt", workspace.path() / "link.txt");
    auto guard = tools::PathGuard::create(workspace.path());
    REQUIRE(guard);

    auto content = guard->read_existing_file("link.txt");
    REQUIRE_FALSE(content);
    CHECK(content.error().detail.find("symlink") != std::string::npos);
}

TEST_CASE("PathGuard rejects symlink in parent path when reading", "[tools][pathguard][u6]") {
    tests::TempWorkspace workspace;
    workspace.write("real/target.txt", "secret");
    std::filesystem::create_symlink(workspace.path() / "real", workspace.path() / "fake");
    auto guard = tools::PathGuard::create(workspace.path());
    REQUIRE(guard);

    auto content = guard->read_existing_file("fake/target.txt");
    REQUIRE_FALSE(content);
    CHECK(content.error().code == util::ErrorCode::Workspace);
}

TEST_CASE("PathGuard writes a file inside workspace", "[tools][pathguard][u6]") {
    tests::TempWorkspace workspace;
    auto guard = tools::PathGuard::create(workspace.path());
    REQUIRE(guard);

    auto written = guard->write_file("note.txt", "hello", false);
    REQUIRE(written);
    CHECK(*written == 5);
    CHECK(workspace.read("note.txt") == "hello");
}

TEST_CASE("PathGuard creates parent directories without following symlinks", "[tools][pathguard][u6]") {
    tests::TempWorkspace workspace;
    auto guard = tools::PathGuard::create(workspace.path());
    REQUIRE(guard);

    auto written = guard->write_file("nested/deep/note.txt", "hello", true);
    REQUIRE(written);
    CHECK(workspace.read("nested/deep/note.txt") == "hello");
}

TEST_CASE("PathGuard rejects writing through final symlink", "[tools][pathguard][u6]") {
    tests::TempWorkspace workspace;
    workspace.write("real.txt", "secret");
    std::filesystem::create_symlink(workspace.path() / "real.txt", workspace.path() / "link.txt");
    auto guard = tools::PathGuard::create(workspace.path());
    REQUIRE(guard);

    auto written = guard->write_file("link.txt", "modified", false);
    REQUIRE_FALSE(written);
    CHECK(written.error().detail.find("symlink") != std::string::npos);
    CHECK(workspace.read("real.txt") == "secret");
}

TEST_CASE("PathGuard rejects symlink in parent path when writing", "[tools][pathguard][u6]") {
    tests::TempWorkspace workspace;
    workspace.write("real/target.txt", "secret");
    std::filesystem::create_symlink(workspace.path() / "real", workspace.path() / "fake");
    auto guard = tools::PathGuard::create(workspace.path());
    REQUIRE(guard);

    auto written = guard->write_file("fake/new.txt", "x", true);
    REQUIRE_FALSE(written);
    CHECK(written.error().code == util::ErrorCode::Workspace);
}

TEST_CASE("PathGuard rejects missing parent when create_parents is false", "[tools][pathguard][u6]") {
    tests::TempWorkspace workspace;
    auto guard = tools::PathGuard::create(workspace.path());
    REQUIRE(guard);

    auto written = guard->write_file("nested/note.txt", "hello", false);
    REQUIRE_FALSE(written);
    CHECK(written.error().detail.find("parent") != std::string::npos);
}
