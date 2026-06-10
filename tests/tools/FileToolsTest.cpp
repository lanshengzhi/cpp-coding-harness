#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../src/harness/ExecutionEnv.hpp"
#include "../../src/tools/Tools.hpp"
#include "../support/TempWorkspace.hpp"

#include <filesystem>
#include <memory>
#include <string>

using namespace cch;

namespace {
agent::ToolContext context_for(const tests::TempWorkspace& workspace) {
    agent::ToolContext context;
    context.workspace = workspace.path();
    return context;
}

class FakeExecutionEnv final : public harness::ExecutionEnv {
public:
    const std::filesystem::path& workspace() const override { return workspace_; }
    bool bash_enabled() const override { return false; }

    util::Result<harness::FileReadResult> read_file(const std::string& path, int offset, int limit) override {
        read_path = path;
        read_offset = offset;
        read_limit = limit;
        return util::Result<harness::FileReadResult>::success(read_result);
    }

    util::Result<harness::FileWriteResult> write_file(const std::string&, const std::string&, bool) override {
        return util::Result<harness::FileWriteResult>::failure("not implemented");
    }

    util::Result<harness::FileEditResult> edit_file(const std::string&, const std::string&, const std::string&) override {
        return util::Result<harness::FileEditResult>::failure("not implemented");
    }

    util::Result<harness::ShellResult> run_shell(const std::string&, std::chrono::milliseconds) override {
        return util::Result<harness::ShellResult>::failure("not implemented");
    }

    std::filesystem::path workspace_{"/fake"};
    harness::FileReadResult read_result{"from fake env", false};
    std::string read_path;
    int read_offset{0};
    int read_limit{0};
};
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

TEST_CASE("read_file can be driven by a fake execution environment", "[tools][u5]") {
    auto fake_env = std::make_shared<FakeExecutionEnv>();
    agent::ToolContext context;
    context.execution_env = fake_env;
    auto tool = tools::make_read_file_tool();

    boost::json::object args;
    args["path"] = "virtual.txt";
    args["offset"] = 3;
    args["limit"] = 4;
    auto result = tool->execute(args, context);

    REQUIRE_FALSE(result.is_error);
    CHECK(result.content.find("path: virtual.txt") != std::string::npos);
    CHECK(result.content.find("from fake env") != std::string::npos);
    CHECK(fake_env->read_path == "virtual.txt");
    CHECK(fake_env->read_offset == 3);
    CHECK(fake_env->read_limit == 4);
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

TEST_CASE("write_file rejects missing parents and directory targets with stable errors", "[tools][u1]") {
    tests::TempWorkspace workspace;
    auto tool = tools::make_write_file_tool();

    boost::json::object missing_parent;
    missing_parent["path"] = "missing/out.txt";
    missing_parent["content"] = "created";
    auto missing_result = tool->execute(missing_parent, context_for(workspace));
    CHECK(missing_result.is_error);
    CHECK(missing_result.content.find("parent directory does not exist") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(workspace.path() / "missing"));

    std::filesystem::create_directory(workspace.path() / "dir-target");
    boost::json::object directory_target;
    directory_target["path"] = "dir-target";
    directory_target["content"] = "nope";
    auto directory_result = tool->execute(directory_target, context_for(workspace));
    CHECK(directory_result.is_error);
    CHECK(directory_result.content.find("target is not a regular file") != std::string::npos);
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
