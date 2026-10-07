// Spec #882, ticket #885: the Agent-owned nested-tool-call seam
// (pi `NestedToolCallRunner`). The cheap check "a nested call runs" passes while
// the recorded shape is wrong; these tests therefore assert pi's limits and id
// format directly on the record a model-issued call leaves behind.

#include <cch/agent/NestedToolCalls.hpp>

#include "support/StreamAdapterFixture.hpp"

#include <cch/ai/Content.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace cch;

namespace {

struct FakeHost {
    std::vector<agent::ToolInvocation> calls;
    bool error{false};
    std::string text{"ok"};
};

[[nodiscard]] agent::NestedToolCallRunner make_runner(FakeHost& host) {
    agent::NestedToolCallHost closures;
    closures.get_tools = [] { return std::vector<ai::Tool>{}; };
    closures.is_sequential = [] { return false; };
    closures.run_tool_call = [&host](agent::ToolInvocation invocation, std::string, std::stop_token) {
        host.calls.push_back(std::move(invocation));
        agent::AsyncToolExecutionResult result;
        result.content.emplace_back(ai::text_content(host.text));
        result.is_error = host.error;
        return support::AsyncResult<agent::AsyncToolExecutionResult>{
                support::Expected<agent::AsyncToolExecutionResult>{std::move(result)}};
    };
    closures.emit = [](const agent::AgentLifecycleEvent&) { return support::ExpectedVoid{}; };
    return agent::NestedToolCallRunner{std::move(closures)};
}

[[nodiscard]] support::JsonValue record_json(const agent::NestedToolCalls& calls) {
    return agent::nested_calls_to_json(calls);
}

} // namespace

TEST_CASE("nested calls get the {callerId}/{n} id and are recorded on the caller",
        "[agent][codemode][issue885][spec]") {
    FakeHost host;
    auto runner = make_runner(host);

    for (int i = 0; i < 3; ++i) {
        auto outcome = tests::run_async_result(runner.execute("call_1", "echo", support::JsonValue{}, {}));
        REQUIRE(outcome.has_value());
        CHECK_FALSE(outcome->is_error);
    }

    REQUIRE(host.calls.size() == 3);
    CHECK(host.calls[0].call_id == "call_1/1");
    CHECK(host.calls[1].call_id == "call_1/2");
    CHECK(host.calls[2].call_id == "call_1/3");

    auto summary = runner.take_record("call_1");
    REQUIRE(summary.has_value());
    REQUIRE(summary->calls.has_value());
    CHECK(summary->calls->complete);
    REQUIRE(summary->calls->calls.size() == 3);
    CHECK(summary->calls->calls[0].status == "ok");
    CHECK(summary->calls->calls[0].duration_ms.has_value());
    CHECK_FALSE(runner.take_record("call_1").has_value());
}

TEST_CASE("the nested-call record drops calls beyond pi's 256-call limit",
        "[agent][codemode][issue885][spec]") {
    FakeHost host;
    auto runner = make_runner(host);

    for (int i = 0; i < 258; ++i) {
        auto outcome = tests::run_async_result(runner.execute("call_1", "echo", support::JsonValue{}, {}));
        REQUIRE(outcome.has_value());
    }

    auto summary = runner.take_record("call_1");
    REQUIRE(summary.has_value());
    REQUIRE(summary->calls.has_value());
    CHECK(summary->calls->calls.size() == 256);
    CHECK_FALSE(summary->calls->complete);
}

TEST_CASE("arguments over pi's per-call cap are omitted but counted",
        "[agent][codemode][issue885][spec]") {
    FakeHost host;
    auto runner = make_runner(host);

    support::JsonValue big{support::JsonValue::object_t{{"x", std::string(9 * 1024, 'a')}}};
    auto outcome = tests::run_async_result(runner.execute("call_1", "echo", std::move(big), {}));
    REQUIRE(outcome.has_value());

    auto summary = runner.take_record("call_1");
    REQUIRE(summary.has_value());
    REQUIRE(summary->calls.has_value());
    CHECK_FALSE(summary->calls->complete);
    REQUIRE(summary->calls->calls.size() == 1);
    CHECK_FALSE(summary->calls->calls[0].arguments.has_value());
    REQUIRE(summary->calls->calls[0].arguments_bytes.has_value());
    CHECK(*summary->calls->calls[0].arguments_bytes > 8 * 1024);
}

TEST_CASE("a nested failure records the error text truncated to pi's 500 chars",
        "[agent][codemode][issue885][spec]") {
    FakeHost host;
    host.error = true;
    host.text = std::string(800, 'e');
    auto runner = make_runner(host);

    auto outcome = tests::run_async_result(runner.execute("call_1", "echo", support::JsonValue{}, {}));
    REQUIRE(outcome.has_value());
    CHECK(outcome->is_error);

    auto summary = runner.take_record("call_1");
    REQUIRE(summary.has_value());
    REQUIRE(summary->calls.has_value());
    REQUIRE(summary->calls->calls.size() == 1);
    CHECK(summary->calls->calls[0].status == "error");
    REQUIRE(summary->calls->calls[0].error.has_value());
    CHECK(summary->calls->calls[0].error->size() == 500);
    CHECK(record_json(*summary->calls).get_object().contains("calls"));
}
