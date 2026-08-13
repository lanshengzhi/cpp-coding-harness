#include "harness/WorkspaceFileSystem.hpp"
#include "../support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace cch;

TEST_CASE("WorkspaceFileSystem reads an existing file inside workspace", "[harness][filesystem][u2]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "hello world");
    auto guard = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(guard);

    auto content = guard->read_existing_file("note.txt");
    REQUIRE(content);
    CHECK(*content == "hello world");
}

TEST_CASE("WorkspaceFileSystem rejects final symlink when reading", "[harness][filesystem][u2]") {
    tests::TempWorkspace workspace;
    workspace.write("real.txt", "secret");
    std::filesystem::create_symlink(workspace.path() / "real.txt", workspace.path() / "link.txt");
    auto guard = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(guard);

    auto content = guard->read_existing_file("link.txt");
    REQUIRE_FALSE(content);
    CHECK(content.error().detail.find("symlink") != std::string::npos);
}

TEST_CASE("WorkspaceFileSystem rejects symlink in parent path when reading", "[harness][filesystem][u2]") {
    tests::TempWorkspace workspace;
    workspace.write("real/target.txt", "secret");
    std::filesystem::create_symlink(workspace.path() / "real", workspace.path() / "fake");
    auto guard = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(guard);

    auto content = guard->read_existing_file("fake/target.txt");
    REQUIRE_FALSE(content);
    CHECK(content.error().code == util::ErrorCode::Workspace);
}

TEST_CASE("WorkspaceFileSystem writes a file inside workspace", "[harness][filesystem][u2]") {
    tests::TempWorkspace workspace;
    auto guard = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(guard);

    auto written = guard->write_file("note.txt", "hello", false);
    REQUIRE(written);
    CHECK(*written == 5);
    CHECK(workspace.read("note.txt") == "hello");
}

TEST_CASE("WorkspaceFileSystem creates parent directories without following symlinks", "[harness][filesystem][u2]") {
    tests::TempWorkspace workspace;
    auto guard = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(guard);

    auto written = guard->write_file("nested/deep/note.txt", "hello", true);
    REQUIRE(written);
    CHECK(workspace.read("nested/deep/note.txt") == "hello");
}

TEST_CASE("WorkspaceFileSystem rejects writing through final symlink", "[harness][filesystem][u2]") {
    tests::TempWorkspace workspace;
    workspace.write("real.txt", "secret");
    std::filesystem::create_symlink(workspace.path() / "real.txt", workspace.path() / "link.txt");
    auto guard = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(guard);

    auto written = guard->write_file("link.txt", "modified", false);
    REQUIRE_FALSE(written);
    CHECK(written.error().detail.find("symlink") != std::string::npos);
    CHECK(workspace.read("real.txt") == "secret");
}

TEST_CASE("WorkspaceFileSystem rejects symlink in parent path when writing", "[harness][filesystem][u2]") {
    tests::TempWorkspace workspace;
    workspace.write("real/target.txt", "secret");
    std::filesystem::create_symlink(workspace.path() / "real", workspace.path() / "fake");
    auto guard = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(guard);

    auto written = guard->write_file("fake/new.txt", "x", true);
    REQUIRE_FALSE(written);
    CHECK(written.error().code == util::ErrorCode::Workspace);
}

TEST_CASE("WorkspaceFileSystem rejects missing parent when create_parents is false", "[harness][filesystem][u2]") {
    tests::TempWorkspace workspace;
    auto guard = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(guard);

    auto written = guard->write_file("nested/note.txt", "hello", false);
    REQUIRE_FALSE(written);
    CHECK(written.error().detail.find("parent") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Pi-shaped filesystem tests
// ---------------------------------------------------------------------------

TEST_CASE("WorkspaceFileSystem fileInfo returns metadata without following symlinks", "[harness][filesystem][u2]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "hello world");
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    auto info = fs->fileInfo("note.txt");
    REQUIRE(info);
    CHECK(info->name == "note.txt");
    CHECK(info->kind == harness::FileKind::File);
    CHECK(info->size == 11);

    // Symlink metadata reports the symlink itself.
    std::filesystem::create_symlink(workspace.path() / "note.txt", workspace.path() / "link.txt");
    auto link_info = fs->fileInfo("link.txt");
    REQUIRE(link_info);
    CHECK(link_info->name == "link.txt");
    CHECK(link_info->kind == harness::FileKind::Symlink);
}

TEST_CASE("WorkspaceFileSystem fileInfo returns not_found for missing path", "[harness][filesystem][u2]") {
    tests::TempWorkspace workspace;
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    auto info = fs->fileInfo("nonexistent.txt");
    REQUIRE_FALSE(info);
    CHECK(info.error().code == harness::FileErrorCode::NotFound);
}

TEST_CASE("WorkspaceFileSystem listDir returns direct children", "[harness][filesystem][u2]") {
    tests::TempWorkspace workspace;
    workspace.write("a.txt", "a");
    workspace.write("sub/b.txt", "b");
    std::filesystem::create_symlink(workspace.path() / "a.txt", workspace.path() / "link.txt");
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    auto listing = fs->listDir(".");
    REQUIRE(listing);
    CHECK(listing->size() == 3);

    bool has_a = false, has_sub = false, has_link = false;
    for (const auto& entry : *listing) {
        if (entry.name == "a.txt" && entry.kind == harness::FileKind::File) has_a = true;
        if (entry.name == "sub" && entry.kind == harness::FileKind::Directory) has_sub = true;
        if (entry.name == "link.txt" && entry.kind == harness::FileKind::Symlink) has_link = true;
    }
    CHECK(has_a);
    CHECK(has_sub);
    CHECK(has_link);

    // listDir on empty directory
    std::filesystem::create_directory(workspace.path() / "empty");
    auto empty = fs->listDir("empty");
    REQUIRE(empty);
    CHECK(empty->empty());
}

TEST_CASE("WorkspaceFileSystem listDir returns error for regular file", "[harness][filesystem][u2]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "hello");
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    auto listing = fs->listDir("note.txt");
    REQUIRE_FALSE(listing);
    CHECK(listing.error().code == harness::FileErrorCode::NotDirectory);
}

TEST_CASE("WorkspaceFileSystem readTextLines with max count", "[harness][filesystem][u2]") {
    tests::TempWorkspace workspace;
    workspace.write("lines.txt", "line1\nline2\nline3\nline4\nline5\n");
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    auto lines = fs->readTextLines("lines.txt", 2);
    REQUIRE(lines);
    CHECK(lines->size() == 2);
    CHECK((*lines)[0] == "line1");
    CHECK((*lines)[1] == "line2");

    auto all = fs->readTextLines("lines.txt");
    REQUIRE(all);
    CHECK(all->size() == 5);
}

TEST_CASE("WorkspaceFileSystem readBinaryFile preserves exact bytes", "[harness][filesystem][u2]") {
    tests::TempWorkspace workspace;
    std::string data("\x00\xFF\x7F\x80", 4);
    workspace.write("bin.dat", data);
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    auto bin = fs->readBinaryFile("bin.dat");
    REQUIRE(bin);
    CHECK(bin->size() == 4);
    CHECK(bin->at(0) == std::byte{0x00});
    CHECK(bin->at(1) == std::byte{0xFF});
    CHECK(bin->at(2) == std::byte{0x7F});
    CHECK(bin->at(3) == std::byte{0x80});
}

TEST_CASE("WorkspaceFileSystem writeFile and appendFile with binary content", "[harness][filesystem][u2]") {
    tests::TempWorkspace workspace;
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    harness::BinaryData bin{std::byte{0x01}, std::byte{0x02}};
    auto wrote = fs->writeFile("bin.dat", bin);
    REQUIRE(wrote);

    auto append = fs->appendFile("bin.dat", harness::BinaryData{std::byte{0x03}});
    REQUIRE(append);

    auto read = fs->readBinaryFile("bin.dat");
    REQUIRE(read);
    CHECK(read->size() == 3);
    CHECK(read->at(0) == std::byte{0x01});
    CHECK(read->at(2) == std::byte{0x03});
}

TEST_CASE("WorkspaceFileSystem createDir and remove with recursive", "[harness][filesystem][u2]") {
    tests::TempWorkspace workspace;
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    auto created = fs->createDir("nested/deep", true);
    REQUIRE(created);
    CHECK(std::filesystem::is_directory(workspace.path() / "nested" / "deep"));

    // Remove recursively
    auto removed = fs->remove("nested", true);
    REQUIRE(removed);
    CHECK_FALSE(std::filesystem::exists(workspace.path() / "nested"));
}

TEST_CASE("WorkspaceFileSystem remove rejects workspace root", "[harness][filesystem][u2]") {
    tests::TempWorkspace workspace;
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    auto result = fs->remove(".", true);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == harness::FileErrorCode::Invalid);
}

TEST_CASE("WorkspaceFileSystem remove does not follow symlinks to outside", "[harness][filesystem][u2]") {
    tests::TempWorkspace workspace;
    workspace.write("keep.txt", "safe");
    std::filesystem::create_symlink(workspace.path() / "keep.txt", workspace.path() / "link.txt");
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    // Removing the symlink should remove the link but not the target.
    auto removed = fs->remove("link.txt");
    REQUIRE(removed);
    CHECK_FALSE(std::filesystem::exists(workspace.path() / "link.txt"));
    CHECK(std::filesystem::exists(workspace.path() / "keep.txt"));
}

TEST_CASE("WorkspaceFileSystem exists returns false for missing path", "[harness][filesystem][u2]") {
    tests::TempWorkspace workspace;
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    auto e = fs->exists("missing.txt");
    REQUIRE(e);
    CHECK_FALSE(*e);

    workspace.write("here.txt", "x");
    auto e2 = fs->exists("here.txt");
    REQUIRE(e2);
    CHECK(*e2);
}

TEST_CASE("WorkspaceFileSystem canonicalPath resolves symlinks within workspace", "[harness][filesystem][u2]") {
    tests::TempWorkspace workspace;
    workspace.write("target.txt", "data");
    std::filesystem::create_symlink(workspace.path() / "target.txt", workspace.path() / "link.txt");
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    auto canonical = fs->canonicalPath("link.txt");
    REQUIRE(canonical);
    CHECK(*canonical == (workspace.path() / "target.txt").string());
}

TEST_CASE("WorkspaceFileSystem createTempDir and createTempFile", "[harness][filesystem][u2]") {
    tests::TempWorkspace workspace;
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    auto dir_result = fs->createTempDir("test-");
    REQUIRE(dir_result);
    CHECK(dir_result->find(".cch-tmp") != std::string::npos);
    CHECK(std::filesystem::is_directory(*dir_result));

    auto file_result = fs->createTempFile("pfx-", "-sfx");
    REQUIRE(file_result);
    CHECK(file_result->find(".cch-tmp") != std::string::npos);
    CHECK(file_result->find("pfx-") != std::string::npos);
    CHECK(file_result->find("-sfx") != std::string::npos);
}

TEST_CASE("WorkspaceFileSystem absolutePath and joinPath", "[harness][filesystem][u2]") {
    tests::TempWorkspace workspace;
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    auto abs = fs->absolutePath("sub/file.txt");
    REQUIRE(abs);
    CHECK(*abs == (workspace.path() / "sub" / "file.txt").string());

    auto joined = fs->joinPath({"sub", "file.txt"});
    REQUIRE(joined);
    CHECK(*joined == (workspace.path() / "sub" / "file.txt").string());
}

TEST_CASE("WorkspaceFileSystem rejects absolute paths", "[harness][filesystem][u2]") {
    tests::TempWorkspace workspace;
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    auto abs = fs->absolutePath("/etc/passwd");
    REQUIRE_FALSE(abs);

    auto read = fs->readTextFile("/etc/passwd");
    REQUIRE_FALSE(read);
}

TEST_CASE("WorkspaceFileSystem rejects path escapes", "[harness][filesystem][u2]") {
    tests::TempWorkspace workspace;
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    auto abs = fs->absolutePath("../outside.txt");
    REQUIRE_FALSE(abs);
    CHECK(abs.error().code == harness::FileErrorCode::PermissionDenied);
}
