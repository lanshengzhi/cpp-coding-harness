#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../src/tools/Tools.hpp"
#include "../support/TempWorkspace.hpp"

#include <cstdlib>

using namespace cch;

namespace {
class FakeProcessRunner final : public util::ProcessRunner {
public:
    util::Result<util::ProcessResult> run(const util::ProcessRequest& request) override {
        requests.push_back(request);
        if (!error.empty()) {
            return util::Result<util::ProcessResult>::failure(error);
        }
        return util::Result<util::ProcessResult>::success(next);
    }

    util::ProcessResult next;
    std::string error;
    std::vector<util::ProcessRequest> requests;
};

agent::ToolContext context_for(const tests::TempWorkspace& workspace, bool enabled) {
    agent::ToolContext context;
    context.workspace = workspace.path();
    context.bash_enabled = enabled;
    return context;
}
}

TEST_CASE("bash is disabled by default", "[tools][u3]") {
    tests::TempWorkspace workspace;
    auto runner = std::make_shared<FakeProcessRunner>();
    auto tool = tools::make_bash_tool(runner);
    boost::json::object args;
    args["command"] = "echo should-not-run";

    auto result = tool->execute(args, context_for(workspace, false));

    REQUIRE(result.is_error);
    CHECK(runner->requests.empty());
}

TEST_CASE("enabled bash captures output and non-zero exit status", "[tools][u3]") {
    tests::TempWorkspace workspace;
    auto runner = std::make_shared<FakeProcessRunner>();
    runner->next.exit_code = 2;
    runner->next.output = "stdout\nstderr\n";
    auto tool = tools::make_bash_tool(runner);
    boost::json::object args;
    args["command"] = "printf hi";
    args["timeout_ms"] = 1234;

    auto result = tool->execute(args, context_for(workspace, true));

    REQUIRE(result.is_error);
    REQUIRE(runner->requests.size() == 1);
    CHECK(runner->requests[0].working_directory == workspace.path());
    CHECK(runner->requests[0].timeout.count() == 1234);
    CHECK(result.content.find("exit_code: 2") != std::string::npos);
    CHECK(result.content.find("stdout") != std::string::npos);
}

TEST_CASE("bash timeout is represented as an error result", "[tools][u3]") {
    tests::TempWorkspace workspace;
    auto runner = std::make_shared<FakeProcessRunner>();
    runner->next.exit_code = -1;
    runner->next.timed_out = true;
    runner->next.output = "partial";
    auto tool = tools::make_bash_tool(runner);
    boost::json::object args;
    args["command"] = "sleep forever";

    auto result = tool->execute(args, context_for(workspace, true));

    REQUIRE(result.is_error);
    CHECK(result.content.find("timed_out: true") != std::string::npos);
    CHECK(result.content.find("partial") != std::string::npos);
}

TEST_CASE("bash runner receives sanitized environment", "[tools][u3]") {
#if defined(__unix__) || defined(__APPLE__)
    setenv("OPENAI_API_KEY", "sk-test-secret", 1);
    setenv("CCH_VISIBLE_ENV", "visible", 1);
#endif
    tests::TempWorkspace workspace;
    auto runner = std::make_shared<FakeProcessRunner>();
    runner->next.exit_code = 0;
    auto tool = tools::make_bash_tool(runner);
    boost::json::object args;
    args["command"] = "env";

    auto result = tool->execute(args, context_for(workspace, true));

    REQUIRE_FALSE(result.is_error);
    REQUIRE(runner->requests.size() == 1);
    CHECK(runner->requests[0].environment.find("OPENAI_API_KEY") == runner->requests[0].environment.end());
#if defined(__unix__) || defined(__APPLE__)
    CHECK(runner->requests[0].environment.find("CCH_VISIBLE_ENV") != runner->requests[0].environment.end());
    unsetenv("OPENAI_API_KEY");
    unsetenv("CCH_VISIBLE_ENV");
#endif
}
