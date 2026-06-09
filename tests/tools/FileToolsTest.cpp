#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../src/tools/Tools.hpp"
#include "../support/TempWorkspace.hpp"

#include <filesystem>

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
    std::filesystem::remove(outside, ec);
}
