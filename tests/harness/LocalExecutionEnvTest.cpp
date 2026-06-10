#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../src/harness/LocalExecutionEnv.hpp"
#include "../support/TempWorkspace.hpp"

#include <cstdlib>
#include <memory>

using namespace cch;

namespace {
class FakeProcessRunner final : public util::ProcessRunner {
public:
    util::Result<util::ProcessResult> run(const util::ProcessRequest& request) override {
        requests.push_back(request);
        return util::Result<util::ProcessResult>::success(next);
    }

    util::ProcessResult next;
    std::vector<util::ProcessRequest> requests;
};
}

TEST_CASE("local execution env reads workspace files with offset and limit", "[harness][u5]") {
    tests::TempWorkspace workspace;
    workspace.write("notes.txt", "alpha\nbeta\ngamma\n");
    harness::LocalExecutionEnv env(workspace.path());

    auto read = env.read_file("notes.txt", 2, 1);

    REQUIRE(read.ok());
    CHECK(read.value().content == "beta");
}

TEST_CASE("local execution env preserves workspace containment errors", "[harness][u5]") {
    tests::TempWorkspace workspace;
    harness::LocalExecutionEnv env(workspace.path());

    auto read = env.read_file("../outside.txt", 1, 0);

    REQUIRE_FALSE(read.ok());
    CHECK(read.error().find("escapes workspace") != std::string::npos);
}

TEST_CASE("local execution env writes and edits through filesystem capability", "[harness][u5]") {
    tests::TempWorkspace workspace;
    harness::LocalExecutionEnv env(workspace.path());

    auto written = env.write_file("nested/out.txt", "one fish\ntwo fish\n", true);
    REQUIRE(written.ok());
    CHECK(written.value().bytes_written == 18);

    auto edited = env.edit_file("nested/out.txt", "two fish", "red fish");
    REQUIRE(edited.ok());
    CHECK(edited.value().old_preview == "two fish");
    CHECK(edited.value().new_preview == "red fish");
    CHECK(workspace.read("nested/out.txt") == "one fish\nred fish\n");
}

TEST_CASE("local execution env shell capability gates bash and sanitizes environment", "[harness][u5]") {
#if defined(__unix__) || defined(__APPLE__)
    setenv("OPENAI_API_KEY", "sk-test-secret", 1);
    setenv("CCH_VISIBLE_ENV", "visible", 1);
#endif
    tests::TempWorkspace workspace;
    auto runner = std::make_shared<FakeProcessRunner>();
    runner->next.exit_code = 0;
    runner->next.output = "ok";
    harness::LocalExecutionEnv disabled(workspace.path(), false, {}, runner);

    auto blocked = disabled.run_shell("echo no", std::chrono::milliseconds(100));
    REQUIRE_FALSE(blocked.ok());
    CHECK(runner->requests.empty());

    harness::LocalExecutionEnv enabled(workspace.path(), true, {}, runner);
    auto shell = enabled.run_shell("echo ok", std::chrono::milliseconds(123));

    REQUIRE(shell.ok());
    REQUIRE(runner->requests.size() == 1);
    CHECK(runner->requests[0].working_directory == workspace.path());
    CHECK(runner->requests[0].timeout.count() == 123);
    CHECK(runner->requests[0].use_explicit_environment);
    CHECK(runner->requests[0].environment.find("OPENAI_API_KEY") == runner->requests[0].environment.end());
#if defined(__unix__) || defined(__APPLE__)
    CHECK(runner->requests[0].environment.find("CCH_VISIBLE_ENV") != runner->requests[0].environment.end());
    unsetenv("OPENAI_API_KEY");
    unsetenv("CCH_VISIBLE_ENV");
#endif
}
