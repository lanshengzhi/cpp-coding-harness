#include "agent/harness/WorkspaceFileSystem.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>

#include <sys/stat.h>

using namespace cch;

TEST_CASE("WorkspaceFileSystem reads an existing file inside workspace", "[harness][filesystem][u2][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "hello world");
    auto guard = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(guard);

    auto content = guard->read_existing_file("note.txt");
    REQUIRE(content);
    CHECK(*content == "hello world");
}

TEST_CASE("WorkspaceFileSystem rejects final symlink when reading", "[harness][filesystem][u2][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("real.txt", "secret");
    std::filesystem::create_symlink(workspace.path() / "real.txt", workspace.path() / "link.txt");
    auto guard = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(guard);

    auto content = guard->read_existing_file("link.txt");
    REQUIRE_FALSE(content);
    CHECK(content.error().detail.find("symlink") != std::string::npos);
}

TEST_CASE("WorkspaceFileSystem rejects symlink in parent path when reading", "[harness][filesystem][u2][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("real/target.txt", "secret");
    std::filesystem::create_symlink(workspace.path() / "real", workspace.path() / "fake");
    auto guard = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(guard);

    auto content = guard->read_existing_file("fake/target.txt");
    REQUIRE_FALSE(content);
    CHECK(content.error().code == support::ErrorCode::Workspace);
}

TEST_CASE("WorkspaceFileSystem writes a file inside workspace", "[harness][filesystem][u2][spec]") {
    tests::TempWorkspace workspace;
    auto guard = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(guard);

    auto written = guard->write_file("note.txt", "hello", false);
    REQUIRE(written);
    CHECK(*written == 5);
    CHECK(workspace.read("note.txt") == "hello");
}

TEST_CASE("WorkspaceFileSystem creates parent directories without following symlinks",
        "[harness][filesystem][u2][spec]") {
    tests::TempWorkspace workspace;
    auto guard = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(guard);

    auto written = guard->write_file("nested/deep/note.txt", "hello", true);
    REQUIRE(written);
    CHECK(workspace.read("nested/deep/note.txt") == "hello");
}

TEST_CASE("WorkspaceFileSystem rejects writing through final symlink", "[harness][filesystem][u2][spec]") {
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

TEST_CASE("WorkspaceFileSystem rejects symlink in parent path when writing", "[harness][filesystem][u2][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("real/target.txt", "secret");
    std::filesystem::create_symlink(workspace.path() / "real", workspace.path() / "fake");
    auto guard = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(guard);

    auto written = guard->write_file("fake/new.txt", "x", true);
    REQUIRE_FALSE(written);
    CHECK(written.error().code == support::ErrorCode::Workspace);
}

TEST_CASE(
        "WorkspaceFileSystem rejects missing parent when create_parents is false", "[harness][filesystem][u2][spec]") {
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

TEST_CASE(
        "WorkspaceFileSystem fileInfo returns metadata without following symlinks", "[harness][filesystem][u2][spec]") {
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

TEST_CASE("WorkspaceFileSystem fileInfo returns not_found for missing path", "[harness][filesystem][u2][spec]") {
    tests::TempWorkspace workspace;
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    auto info = fs->fileInfo("nonexistent.txt");
    REQUIRE_FALSE(info);
    CHECK(info.error().code == harness::FileErrorCode::NotFound);
}

TEST_CASE(
        "WorkspaceFileSystem returns not_found for missing parent paths", "[harness][filesystem][u2][issue558][spec]") {
    tests::TempWorkspace workspace;
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    auto read = fs->readTextFile("missing/note.txt");
    REQUIRE_FALSE(read);
    CHECK(read.error().code == harness::FileErrorCode::NotFound);

    auto info = fs->fileInfo("missing/note.txt");
    REQUIRE_FALSE(info);
    CHECK(info.error().code == harness::FileErrorCode::NotFound);

    auto listing = fs->listDir("missing/nested");
    REQUIRE_FALSE(listing);
    CHECK(listing.error().code == harness::FileErrorCode::NotFound);
}

TEST_CASE("WorkspaceFileSystem listDir returns direct children", "[harness][filesystem][u2][spec]") {
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

    auto trailing = fs->listDir("empty/");
    REQUIRE(trailing);
    CHECK(trailing->empty());
}

TEST_CASE("WorkspaceFileSystem listDir returns error for regular file", "[harness][filesystem][u2][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "hello");
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    auto listing = fs->listDir("note.txt");
    REQUIRE_FALSE(listing);
    CHECK(listing.error().code == harness::FileErrorCode::NotDirectory);
}

TEST_CASE("WorkspaceFileSystem readTextLines with max count", "[harness][filesystem][u2][spec]") {
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

TEST_CASE("WorkspaceFileSystem readBinaryFile preserves exact bytes", "[harness][filesystem][u2][spec]") {
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

TEST_CASE("WorkspaceFileSystem writeFile and appendFile with binary content", "[harness][filesystem][u2][spec]") {
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

TEST_CASE("WorkspaceFileSystem createDir and remove with recursive", "[harness][filesystem][u2][spec]") {
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

TEST_CASE("WorkspaceFileSystem remove rejects workspace root", "[harness][filesystem][u2][spec]") {
    tests::TempWorkspace workspace;
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    auto result = fs->remove(".", true);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == harness::FileErrorCode::Invalid);
}

TEST_CASE("WorkspaceFileSystem remove does not follow symlinks to outside", "[harness][filesystem][u2][spec]") {
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

TEST_CASE("WorkspaceFileSystem rejects symlink parents for metadata and mutations",
        "[harness][filesystem][u2][issue558][spec]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace outside;
    outside.write("secret.txt", "outside");
    std::filesystem::create_symlink(outside.path(), workspace.path() / "escape");
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    auto existing = fs->exists("escape/secret.txt");
    REQUIRE_FALSE(existing);
    CHECK(existing.error().code == harness::FileErrorCode::PermissionDenied);

    auto info = fs->fileInfo("escape/secret.txt");
    REQUIRE_FALSE(info);
    CHECK(info.error().code == harness::FileErrorCode::PermissionDenied);

    auto listing = fs->listDir("escape");
    REQUIRE_FALSE(listing);
    CHECK(listing.error().code == harness::FileErrorCode::NotDirectory);

    auto written = fs->write_file("escape/new.txt", "x", true);
    REQUIRE_FALSE(written);
    CHECK(written.error().code == support::ErrorCode::Workspace);

    auto created = fs->createDir("escape/new", true);
    REQUIRE_FALSE(created);
    CHECK(created.error().code == harness::FileErrorCode::PermissionDenied);
    CHECK_FALSE(std::filesystem::exists(outside.path() / "new"));

    auto removed = fs->remove("escape/secret.txt");
    REQUIRE_FALSE(removed);
    CHECK(removed.error().code == harness::FileErrorCode::PermissionDenied);
    CHECK(outside.read("secret.txt") == "outside");
}

TEST_CASE("WorkspaceFileSystem exists returns false for missing path", "[harness][filesystem][u2][spec]") {
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

TEST_CASE("WorkspaceFileSystem canonicalPath resolves symlinks within workspace", "[harness][filesystem][u2][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("target.txt", "data");
    std::filesystem::create_symlink(workspace.path() / "target.txt", workspace.path() / "link.txt");
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    auto canonical = fs->canonicalPath("link.txt");
    REQUIRE(canonical);
    CHECK(*canonical == (workspace.path() / "target.txt").string());
}

TEST_CASE("WorkspaceFileSystem createTempDir and createTempFile", "[harness][filesystem][u2][spec]") {
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

TEST_CASE("WorkspaceFileSystem rejects append beyond the fixed file limit without replacement",
        "[harness][filesystem][capacity][issue558][spec]") {
    tests::TempWorkspace workspace;
    const std::string original(harness::kFileSystemCapacity.max_file_bytes, 'x');
    workspace.write("bounded.txt", original);
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    auto result = fs->appendFile("bounded.txt", std::string{"y"});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == harness::FileErrorCode::ResourceLimit);
    CHECK(workspace.read("bounded.txt") == original);

    auto oversized =
            fs->appendFile("new-bounded.txt", std::string(harness::kFileSystemCapacity.max_file_bytes + 1, 'z'));
    REQUIRE_FALSE(oversized);
    CHECK(oversized.error().code == harness::FileErrorCode::ResourceLimit);
    CHECK_FALSE(std::filesystem::exists(workspace.path() / "new-bounded.txt"));
}

TEST_CASE("WorkspaceFileSystem absolutePath and joinPath", "[harness][filesystem][u2][spec]") {
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

TEST_CASE("WorkspaceFileSystem joinPath joins segments without workspace containment",
        "[harness][filesystem][u2][spec][issue699]") {
    tests::TempWorkspace workspace;
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    // No containment (ADR 0057): joined segments that normalize outside the
    // workspace root are returned after lexical normalization, not rejected.
    auto escaped = fs->joinPath({"..", "sibling.txt"});
    REQUIRE(escaped);
    CHECK(*escaped == (workspace.path().parent_path() / "sibling.txt").string());

    auto absolute = fs->joinPath({"/tmp", "joined.txt"});
    REQUIRE(absolute);
    CHECK(*absolute == "/tmp/joined.txt");
}

TEST_CASE("WorkspaceFileSystem accepts absolute paths inside the workspace",
        "[harness][filesystem][u2][spec][issue618]") {
    tests::TempWorkspace workspace;
    workspace.write("sub/file.txt", "content");
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    const auto inside = (workspace.path() / "sub" / "file.txt").string();
    auto abs = fs->absolutePath(inside);
    REQUIRE(abs);
    CHECK(*abs == inside);

    auto read = fs->readTextFile(inside);
    REQUIRE(read);
    CHECK(*read == "content");

    auto missing = fs->readTextFile((workspace.path() / "nope.txt").string());
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == harness::FileErrorCode::NotFound);
}

TEST_CASE("WorkspaceFileSystem honors absolute paths outside the workspace",
        "[harness][filesystem][u2][spec][issue696][issue698]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace outside;
    outside.write("note.txt", "outside body");
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    // Uniform pi resolveToCwd resolution (ADR 0057): absolute paths are
    // honored anywhere on the host filesystem.
    const auto target = (outside.path() / "note.txt").string();
    auto abs = fs->absolutePath(target);
    REQUIRE(abs);
    CHECK(*abs == target);

    auto read = fs->readTextFile(target);
    REQUIRE(read);
    CHECK(*read == "outside body");

    // A missing outside path surfaces the OS-level NotFound, not a
    // containment rejection.
    auto missing = fs->readTextFile((outside.path() / "nope.txt").string());
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == harness::FileErrorCode::NotFound);
}

TEST_CASE("WorkspaceFileSystem metadata listing and mutation operate outside the workspace",
        "[harness][filesystem][u2][spec][issue699]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace outside;
    outside.write("dir/note.txt", "l1\nl2\nl3\n");
    outside.write("dir/bin.dat", std::string{"\x01\x02", 2});
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    // No containment (ADR 0057): every operation accepts a valid external
    // absolute path and surfaces ordinary OS-level results.
    const auto dir = (outside.path() / "dir").string();

    auto info = fs->fileInfo(dir + "/note.txt");
    REQUIRE(info);
    CHECK(info->name == "note.txt");
    CHECK(info->kind == harness::FileKind::File);
    CHECK(info->size == 9);

    auto listing = fs->listDir(dir);
    REQUIRE(listing);
    CHECK(listing->size() == 2);

    auto present = fs->exists(dir + "/note.txt");
    REQUIRE(present);
    CHECK(*present);
    auto absent = fs->exists(dir + "/missing.txt");
    REQUIRE(absent);
    CHECK_FALSE(*absent);

    auto lines = fs->readTextLines(dir + "/note.txt");
    REQUIRE(lines);
    CHECK(lines->size() == 3);

    auto binary = fs->readBinaryFile(dir + "/bin.dat");
    REQUIRE(binary);
    CHECK(binary->size() == 2);

    auto created = fs->createDir(dir + "/new/nested", true);
    CHECK(created);
    std::error_code created_ec;
    CHECK(std::filesystem::is_directory(outside.path() / "dir" / "new" / "nested", created_ec));

    // A missing external parent surfaces the OS-level NotFound, not a
    // containment rejection.
    auto missing_create = fs->createDir(dir + "/absent/nested", false);
    REQUIRE_FALSE(missing_create);
    CHECK(missing_create.error().code == harness::FileErrorCode::NotFound);
    auto missing_remove = fs->remove(dir + "/absent/deep.txt");
    REQUIRE_FALSE(missing_remove);
    CHECK(missing_remove.error().code == harness::FileErrorCode::NotFound);

    auto removed_file = fs->remove(dir + "/bin.dat");
    CHECK(removed_file);
    std::error_code bin_ec;
    CHECK_FALSE(std::filesystem::exists(outside.path() / "dir" / "bin.dat", bin_ec));

    auto removed_tree = fs->remove(dir + "/new", true);
    CHECK(removed_tree);
    std::error_code tree_ec;
    CHECK_FALSE(std::filesystem::exists(outside.path() / "dir" / "new", tree_ec));
}

TEST_CASE("WorkspaceFileSystem applies pi resolveToCwd preprocessing to read paths",
        "[harness][filesystem][u2][spec][issue699]") {
    tests::TempWorkspace workspace;
    workspace.write("local.txt", "local");
    tests::TempWorkspace outside;
    outside.write("external.txt", "external");
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    // Leading "@" mention prefixes strip before resolution (pi
    // normalizePath), for workspace-relative and absolute read targets.
    auto at_relative = fs->readTextFile("@local.txt");
    REQUIRE(at_relative);
    CHECK(*at_relative == "local");

    auto at_absolute = fs->readTextFile("@" + (outside.path() / "external.txt").string());
    REQUIRE(at_absolute);
    CHECK(*at_absolute == "external");

    // "~" expands against $HOME (pi expandTilde) before resolution.
    tests::TempWorkspace fake_home;
    fake_home.write("documents/home.txt", "home body");
    const tests::EnvVarGuard home{"HOME", fake_home.path().string()};
    auto tilde = fs->readTextFile("~/documents/home.txt");
    REQUIRE(tilde);
    CHECK(*tilde == "home body");
}

TEST_CASE("WorkspaceFileSystem resolves '..' segments lexically against the workspace root",
        "[harness][filesystem][u2][spec][issue696][issue698]") {
    tests::TempWorkspace workspace;
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    // pi resolveToCwd: a ".." segment normalizes against the workspace root
    // and lands in its parent directory rather than being rejected.
    auto abs = fs->absolutePath("../outside.txt");
    REQUIRE(abs);
    CHECK(*abs == (fs->root().parent_path() / "outside.txt").string());
}

TEST_CASE("WorkspaceFileSystem writes an absolute path under the OS temp directory",
        "[harness][filesystem][u2][spec][issue619]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace outside;
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    // pi resolveToCwd semantics (#619): absolute paths are honored anywhere,
    // and write still creates parent directories.
    const auto target = (outside.path() / "nested" / "deep" / "report.html").string();
    auto written = fs->write_file(target, "<html></html>", true);
    REQUIRE(written);
    CHECK(*written == 13);
    CHECK(outside.read("nested/deep/report.html") == "<html></html>");

    auto pi_write = fs->writeFile((outside.path() / "pi-shaped.txt").string(), std::string{"via writeFile"});
    REQUIRE(pi_write);
    CHECK(outside.read("pi-shaped.txt") == "via writeFile");
}

TEST_CASE("WorkspaceFileSystem resolves '..' segments in write paths by normalization",
        "[harness][filesystem][u2][spec][issue619]") {
    tests::TempWorkspace workspace;
    workspace.write("sub/placeholder.txt", "x");
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    // A ".." segment that stays inside the workspace normalizes instead of
    // being rejected; workspace-relative behavior is unchanged otherwise.
    auto inside = fs->write_file("sub/../note.txt", "hello", true);
    REQUIRE(inside);
    CHECK(workspace.read("note.txt") == "hello");

    // A ".." segment that escapes the workspace root normalizes against it
    // (pi resolveToCwd) and lands outside.
    const auto escaped_name = workspace.path().filename().string() + "-escaped.txt";
    const auto escaped_target = workspace.path().parent_path() / escaped_name;
    auto escaped = fs->write_file("../" + escaped_name, "escaped", true);
    REQUIRE(escaped);
    std::error_code cleanup_ec;
    const std::string escaped_content = [&] {
        std::ifstream input(escaped_target, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }();
    std::filesystem::remove(escaped_target, cleanup_ec);
    CHECK(escaped_content == "escaped");

    // Absolute paths containing ".." are honored after lexical normalization.
    tests::TempWorkspace outside;
    outside.write("keep/marker.txt", "x");
    const auto normalized = (outside.path() / "keep" / ".." / "normalized.txt").string();
    auto absolute = fs->write_file(normalized, "abs", true);
    REQUIRE(absolute);
    CHECK(outside.read("normalized.txt") == "abs");
}

TEST_CASE("WorkspaceFileSystem applies pi resolveToCwd preprocessing to write paths",
        "[harness][filesystem][u2][spec][issue619]") {
    tests::TempWorkspace workspace;
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    // pi normalizePath: a leading "@" mention prefix is stripped before
    // resolution, for both workspace-relative and absolute targets.
    auto at_relative = fs->write_file("@at-stripped.txt", "at", true);
    REQUIRE(at_relative);
    CHECK(workspace.read("at-stripped.txt") == "at");

    tests::TempWorkspace outside;
    auto at_absolute = fs->write_file("@" + (outside.path() / "at-outside.txt").string(), "at-abs", true);
    REQUIRE(at_absolute);
    CHECK(outside.read("at-outside.txt") == "at-abs");

    // pi UNICODE_SPACES: no-break-style spaces (U+00A0, U+202F shown here)
    // normalize to ASCII space before resolution.
    auto nbsp = fs->write_file("uni\u00A0ver.txt", "nbsp", true);
    REQUIRE(nbsp);
    CHECK(workspace.read("uni ver.txt") == "nbsp");
    auto narrow = fs->write_file("narrow\u202Fspace.txt", "nnbsp", true);
    REQUIRE(narrow);
    CHECK(workspace.read("narrow space.txt") == "nnbsp");

    // pi expandTilde: "~" expands against $HOME before resolution.
    tests::TempWorkspace fake_home;
    const char* previous_home = std::getenv("HOME");
    const std::string saved_home = previous_home != nullptr ? previous_home : "";
    REQUIRE(setenv("HOME", fake_home.path().c_str(), 1) == 0);
    auto tilde = fs->write_file("~/tilde/note.txt", "home", true);
    if (previous_home != nullptr) {
        REQUIRE(setenv("HOME", saved_home.c_str(), 1) == 0);
    } else {
        REQUIRE(unsetenv("HOME") == 0);
    }
    REQUIRE(tilde);
    CHECK(fake_home.read("tilde/note.txt") == "home");
}

TEST_CASE("WorkspaceFileSystem reads and appends outside the workspace",
        "[harness][filesystem][u2][spec][issue619][issue696][issue698]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace outside;
    outside.write("scratch/log.txt", "first\n");
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    // Uniform pi resolveToCwd resolution (ADR 0057): one read operation
    // serves workspace and outside paths; there is no separate write scope.
    const auto target = (outside.path() / "scratch" / "log.txt").string();
    auto read = fs->readTextFile(target);
    REQUIRE(read);
    CHECK(*read == "first\n");

    auto appended = fs->appendFile(target, std::string{"second\n"});
    REQUIRE(appended);
    CHECK(outside.read("scratch/log.txt") == "first\nsecond\n");
}

TEST_CASE("WorkspaceFileSystem refuses symlinks outside the workspace",
        "[harness][filesystem][u2][spec][issue619][issue696][issue698]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace outside;
    outside.write("realdir/target.txt", "secret");
    outside.write("real.txt", "secret");
    std::error_code symlink_ec;
    std::filesystem::create_symlink(outside.path() / "real.txt", outside.path() / "link.txt", symlink_ec);
    REQUIRE(!symlink_ec);
    std::filesystem::create_symlink(outside.path() / "realdir", outside.path() / "fakedir", symlink_ec);
    REQUIRE(!symlink_ec);
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    // The no-follow symlink policy applies uniformly: honoring absolute paths
    // anywhere does not follow symlinks to get there.
    auto final_link = fs->write_file((outside.path() / "link.txt").string(), "modified", true);
    REQUIRE_FALSE(final_link);
    CHECK(final_link.error().detail.find("symlink") != std::string::npos);
    CHECK(outside.read("real.txt") == "secret");

    auto parent_link = fs->write_file((outside.path() / "fakedir" / "new.txt").string(), "x", true);
    REQUIRE_FALSE(parent_link);
    std::error_code exists_ec;
    CHECK_FALSE(std::filesystem::exists(outside.path() / "realdir" / "new.txt", exists_ec));

    auto linked_read = fs->readTextFile((outside.path() / "link.txt").string());
    REQUIRE_FALSE(linked_read);
    CHECK(linked_read.error().code == harness::FileErrorCode::PermissionDenied);
}

TEST_CASE("WorkspaceFileSystem enforces reviewed path and file contracts", "[harness][filesystem][issue557][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("existing.txt", "content");
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    auto nul_path = fs->readTextFile(std::string{"existing.txt\0suffix", 19});
    REQUIRE_FALSE(nul_path);
    CHECK(nul_path.error().code == harness::FileErrorCode::PermissionDenied);

    auto zero_lines = fs->readTextLines("missing.txt", 0);
    REQUIRE(zero_lines);
    CHECK(zero_lines->empty());

    workspace.write("crlf.txt", "first\r\nsecond\r\n");
    auto crlf_lines = fs->readTextLines("crlf.txt");
    REQUIRE(crlf_lines);
    REQUIRE(crlf_lines->size() == 2);
    CHECK((*crlf_lines)[0] == "first");
    CHECK((*crlf_lines)[1] == "second");

    auto existing_file_dir = fs->createDir("existing.txt");
    REQUIRE_FALSE(existing_file_dir);
    CHECK(existing_file_dir.error().code == harness::FileErrorCode::Invalid);

    std::filesystem::create_symlink(workspace.path() / "existing.txt", workspace.path() / "existing-link");
    auto existing_link_dir = fs->createDir("existing-link");
    REQUIRE_FALSE(existing_link_dir);

    auto empty_write = fs->writeFile("empty.bin", harness::BinaryData{});
    REQUIRE(empty_write);
    auto empty_append = fs->appendFile("empty.bin", harness::BinaryData{});
    REQUIRE(empty_append);
    auto empty_read = fs->readBinaryFile("empty.bin");
    REQUIRE(empty_read);
    CHECK(empty_read->empty());

    std::filesystem::permissions(workspace.path() / "existing.txt",
            std::filesystem::perms::owner_read,
            std::filesystem::perm_options::replace);
    auto read_only_write = fs->writeFile("existing.txt", std::string{"replacement"});
    REQUIRE_FALSE(read_only_write);
    std::filesystem::permissions(workspace.path() / "existing.txt",
            std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace);
    CHECK(workspace.read("existing.txt") == "content");

    const auto fifo = workspace.path() / "fifo";
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
    auto fifo_read = fs->readTextFile("fifo");
    REQUIRE_FALSE(fifo_read);
    CHECK(fifo_read.error().code == harness::FileErrorCode::IsDirectory);
    auto fifo_info = fs->fileInfo("fifo");
    REQUIRE_FALSE(fifo_info);
    CHECK(fifo_info.error().code == harness::FileErrorCode::Invalid);
    auto root_entries = fs->listDir(".");
    REQUIRE(root_entries);
    CHECK(std::none_of(
            root_entries->begin(), root_entries->end(), [](const auto& entry) { return entry.name == "fifo"; }));
}

TEST_CASE("WorkspaceFileSystem reads absolute paths outside the workspace without an authorization list",
        "[harness][filesystem][u2][spec][issue696][issue698]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace skill_home;
    skill_home.write("my-skill/SKILL.md", "skill body");
    skill_home.write("my-skill/refs/extra.md", "extra");
    workspace.write("local.txt", "local");

    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    // Skills carrying absolute paths resolve without an authorization list
    // (ADR 0057 retires the #629 skill-root allowlist).
    auto skill = fs->readTextFile((skill_home.path() / "my-skill" / "SKILL.md").string());
    REQUIRE(skill);
    CHECK(*skill == "skill body");

    auto nested = fs->readTextFile((skill_home.path() / "my-skill" / "refs" / "extra.md").string());
    REQUIRE(nested);
    CHECK(*nested == "extra");

    auto local = fs->readTextFile("local.txt");
    REQUIRE(local);
    CHECK(*local == "local");
}

TEST_CASE("WorkspaceFileSystem reads and writes outside paths uniformly",
        "[harness][filesystem][u2][spec][issue619][issue696][issue698]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace outside;
    outside.write("my-skill/SKILL.md", "skill body");
    outside.write("unrelated.md", "unrelated");

    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    // No containment and no allowlist (ADR 0057): outside paths read like
    // any other path, including through ".." lexical normalization.
    auto other = fs->readTextFile((outside.path() / "unrelated.md").string());
    REQUIRE(other);
    CHECK(*other == "unrelated");

    auto escaped = fs->readTextFile((outside.path() / "my-skill" / ".." / "unrelated.md").string());
    REQUIRE(escaped);
    CHECK(*escaped == "unrelated");

    auto write = fs->writeFile((outside.path() / "my-skill" / "written.md").string(), std::string{"x"});
    REQUIRE(write);
    std::error_code exists_ec;
    CHECK(std::filesystem::exists(outside.path() / "my-skill" / "written.md", exists_ec));
}

TEST_CASE("WorkspaceFileSystem refuses symlink reads outside the workspace",
        "[harness][filesystem][u2][spec][issue629][issue696][issue698]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace outside;
    outside.write("outside-target.txt", "secret");
    outside.write("my-skill/SKILL.md", "skill body");
    std::error_code symlink_ec;
    std::filesystem::create_symlink(
            outside.path() / "outside-target.txt", outside.path() / "my-skill" / "link.md", symlink_ec);
    REQUIRE(!symlink_ec);

    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    // The no-follow symlink policy applies uniformly, outside the workspace
    // as inside it.
    auto linked = fs->readTextFile((outside.path() / "my-skill" / "link.md").string());
    REQUIRE_FALSE(linked);
    CHECK(linked.error().code == harness::FileErrorCode::PermissionDenied);
}

TEST_CASE("WorkspaceFileSystem addresses the filesystem root uniformly", "[harness][filesystem][u2][spec][issue698]") {
    tests::TempWorkspace workspace;
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    // pi resolveToCwd resolves "/" to the filesystem root; metadata, listing,
    // and existence operate on it like any other path even though the
    // parent+filename walk cannot address a root.
    auto info = fs->fileInfo("/");
    REQUIRE(info);
    CHECK(info->kind == harness::FileKind::Directory);

    auto listing = fs->listDir("/");
    REQUIRE(listing);
    CHECK_FALSE(listing->empty());

    auto root_exists = fs->exists("/");
    REQUIRE(root_exists);
    CHECK(*root_exists);

    auto created = fs->createDir("/", true);
    REQUIRE(created);

    auto removed = fs->remove("/", true);
    REQUIRE_FALSE(removed);
    CHECK(removed.error().code == harness::FileErrorCode::Invalid);
}

TEST_CASE("WorkspaceFileSystem rejections name the accepted path form",
        "[harness][filesystem][u2][spec][issue696][issue698]") {
    tests::TempWorkspace workspace;
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);

    auto empty = fs->readTextFile("");
    REQUIRE_FALSE(empty);
    CHECK(empty.error().code == harness::FileErrorCode::PermissionDenied);
    CHECK(empty.error().message.find("use a workspace-relative path or an absolute path") != std::string::npos);

    auto escaped = fs->readTextFile(std::string{"../outside.txt\0suffix", 21});
    REQUIRE_FALSE(escaped);
    CHECK(escaped.error().code == harness::FileErrorCode::PermissionDenied);
    CHECK(escaped.error().message.find("NUL") != std::string::npos);
}
