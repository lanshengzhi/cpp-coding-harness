#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../src/tools/Tools.hpp"
#include "../support/TempWorkspace.hpp"

#include <filesystem>
#include <string>

using namespace cch;

namespace {
agent::ToolContext context_for(const tests::TempWorkspace& workspace) {
    agent::ToolContext context;
    context.workspace = workspace.path();
    return context;
}
}

TEST_CASE("read_file returns workspace file content with path label", "[tools][u3]") {
    tests::TempWorkspace workspace;
    workspace.write("notes.txt", "alpha\nbeta\n");
    auto tool = tools::make_read_file_tool();

    boost::json::object args;
    args["path"] = "notes.txt";
    auto result = tool->execute(args, context_for(workspace));

    REQUIRE_FALSE(result.is_error);
    CHECK(result.content.find("path: notes.txt") != std::string::npos);
    CHECK(result.content.find("alpha") != std::string::npos);
}

TEST_CASE("read_file stops at output cap and marks truncation", "[tools][u3]") {
    tests::TempWorkspace workspace;
    std::string content;
    for (int i = 0; i < 2100; ++i) {
        content += "line\n";
    }
    workspace.write("large.txt", content);
    auto tool = tools::make_read_file_tool();

    boost::json::object args;
    args["path"] = "large.txt";
    auto result = tool->execute(args, context_for(workspace));

    REQUIRE_FALSE(result.is_error);
    CHECK(result.content.find("[output truncated]") != std::string::npos);
}

TEST_CASE("write_file creates file after validating existing parent", "[tools][u3]") {
    tests::TempWorkspace workspace;
    auto tool = tools::make_write_file_tool();

    boost::json::object args;
    args["path"] = "out.txt";
    args["content"] = "created";
    auto result = tool->execute(args, context_for(workspace));

    REQUIRE_FALSE(result.is_error);
    CHECK(workspace.read("out.txt") == "created");
    CHECK(result.content.find(workspace.path().string()) == std::string::npos);
}

TEST_CASE("write_file does not follow dangling temporary symlinks", "[tools][u3]") {
#if defined(__unix__) || defined(__APPLE__)
    tests::TempWorkspace workspace;
    auto outside = workspace.path().parent_path() / "outside-temp-target.txt";
    auto temp_link = workspace.path() / ".out.txt.tmp-0";
    std::error_code ec;
    std::filesystem::create_symlink(outside, temp_link, ec);
    if (!ec) {
        auto tool = tools::make_write_file_tool();
        boost::json::object args;
        args["path"] = "out.txt";
        args["content"] = "created";
        auto result = tool->execute(args, context_for(workspace));

        REQUIRE_FALSE(result.is_error);
        CHECK(workspace.read("out.txt") == "created");
        CHECK_FALSE(std::filesystem::exists(outside));
    }
#endif
}

TEST_CASE("write_file rejects missing content but permits explicit empty content", "[tools][u3]") {
    tests::TempWorkspace workspace;
    auto tool = tools::make_write_file_tool();

    boost::json::object missing_content;
    missing_content["path"] = "empty.txt";
    auto missing = tool->execute(missing_content, context_for(workspace));
    CHECK(missing.is_error);
    CHECK_FALSE(std::filesystem::exists(workspace.path() / "empty.txt"));

    boost::json::object empty_content;
    empty_content["path"] = "empty.txt";
    empty_content["content"] = "";
    auto empty = tool->execute(empty_content, context_for(workspace));
    REQUIRE_FALSE(empty.is_error);
    CHECK(workspace.read("empty.txt").empty());
}

TEST_CASE("edit_file replaces one unique exact text region", "[tools][u3]") {
    tests::TempWorkspace workspace;
    workspace.write("story.txt", "one fish\ntwo fish\n");
    auto tool = tools::make_edit_file_tool();

    boost::json::object args;
    args["path"] = "story.txt";
    args["old_text"] = "two fish";
    args["new_text"] = "red fish";
    auto result = tool->execute(args, context_for(workspace));

    REQUIRE_FALSE(result.is_error);
    CHECK(workspace.read("story.txt") == "one fish\nred fish\n");
}

TEST_CASE("edit_file rejects zero and ambiguous matches", "[tools][u3]") {
    tests::TempWorkspace workspace;
    workspace.write("story.txt", "same\nsame\n");
    auto tool = tools::make_edit_file_tool();

    boost::json::object missing;
    missing["path"] = "story.txt";
    missing["old_text"] = "absent";
    missing["new_text"] = "x";
    auto no_match = tool->execute(missing, context_for(workspace));
    CHECK(no_match.is_error);

    boost::json::object ambiguous;
    ambiguous["path"] = "story.txt";
    ambiguous["old_text"] = "same";
    ambiguous["new_text"] = "x";
    auto many = tool->execute(ambiguous, context_for(workspace));
    CHECK(many.is_error);
    CHECK(workspace.read("story.txt") == "same\nsame\n");
}

TEST_CASE("edit_file rejects missing new_text but permits explicit empty replacement", "[tools][u3]") {
    tests::TempWorkspace workspace;
    workspace.write("story.txt", "keep\ndelete me\n");
    auto tool = tools::make_edit_file_tool();

    boost::json::object missing_new;
    missing_new["path"] = "story.txt";
    missing_new["old_text"] = "delete me";
    auto missing = tool->execute(missing_new, context_for(workspace));
    CHECK(missing.is_error);
    CHECK(workspace.read("story.txt") == "keep\ndelete me\n");

    boost::json::object explicit_delete;
    explicit_delete["path"] = "story.txt";
    explicit_delete["old_text"] = "delete me";
    explicit_delete["new_text"] = "";
    auto deleted = tool->execute(explicit_delete, context_for(workspace));
    REQUIRE_FALSE(deleted.is_error);
    CHECK(workspace.read("story.txt") == "keep\n\n");
}

TEST_CASE("file tools reject traversal and final symlink escapes", "[tools][u3]") {
    tests::TempWorkspace workspace;
    workspace.write("safe.txt", "safe");
    auto read_tool = tools::make_read_file_tool();
    boost::json::object traversal;
    traversal["path"] = "../outside.txt";
    auto read_result = read_tool->execute(traversal, context_for(workspace));
    CHECK(read_result.is_error);

    auto outside = workspace.path().parent_path() / "outside-target.txt";
    {
        std::ofstream output(outside);
        output << "outside";
    }
    auto symlink = workspace.path() / "link.txt";
    std::error_code ec;
    std::filesystem::create_symlink(outside, symlink, ec);
    if (!ec) {
        auto write_tool = tools::make_write_file_tool();
        boost::json::object args;
        args["path"] = "link.txt";
        args["content"] = "nope";
        auto result = write_tool->execute(args, context_for(workspace));
        CHECK(result.is_error);
    }

    auto broken = workspace.path() / "broken-link.txt";
    std::filesystem::create_symlink(workspace.path().parent_path() / "missing-outside.txt", broken, ec);
    if (!ec) {
        auto write_tool = tools::make_write_file_tool();
        boost::json::object args;
        args["path"] = "broken-link.txt";
        args["content"] = "nope";
        auto result = write_tool->execute(args, context_for(workspace));
        CHECK(result.is_error);
    }

    std::filesystem::create_directories(workspace.path() / "real-dir", ec);
    auto linked_parent = workspace.path() / "linked-dir";
    std::filesystem::create_directory_symlink(workspace.path() / "real-dir", linked_parent, ec);
    if (!ec) {
        auto write_tool = tools::make_write_file_tool();
        boost::json::object args;
        args["path"] = "linked-dir/file.txt";
        args["content"] = "nope";
        auto result = write_tool->execute(args, context_for(workspace));
        CHECK(result.is_error);
        CHECK_FALSE(std::filesystem::exists(workspace.path() / "real-dir" / "file.txt"));

        boost::json::object create_args;
        create_args["path"] = "linked-dir/new-parent/file.txt";
        create_args["content"] = "nope";
        create_args["create_parents"] = true;
        auto create_result = write_tool->execute(create_args, context_for(workspace));
        CHECK(create_result.is_error);
        CHECK_FALSE(std::filesystem::exists(workspace.path() / "real-dir" / "new-parent"));
    }
    std::filesystem::remove(outside, ec);
}
