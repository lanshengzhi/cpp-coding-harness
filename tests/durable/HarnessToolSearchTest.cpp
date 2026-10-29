#include <cch/durable/Harness.hpp>
#include <cch/durable/SqliteStorage.hpp>
#include <cch/durable/ToolSearch.hpp>
#include "durable/FrozenExpectation.hpp"
#include "durable/Outcome.hpp"
#include "support/Json.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cch::tests::durable {
using namespace cch::durable;

namespace {

struct ToolCall {
    std::string name;
    support::JsonValue arguments{support::JsonValue::object_t{}};
};

struct Script {
    std::vector<ToolCall> calls{};
    std::vector<std::vector<std::string>> offered{};
    std::size_t next{0};
};

ai::ModelStreamFactory scripted(std::shared_ptr<Script> script) {
    return [script = std::move(script)](ai::Model, ai::AiContext context, ai::SimpleStreamOptions) mutable {
        std::vector<std::string> offered;
        for (const auto& tool : context.tools)
            offered.push_back(tool.name);
        script->offered.push_back(std::move(offered));
        auto message = ai::assistant_text_message("done");
        if (script->next < script->calls.size()) {
            const auto& call = script->calls[script->next];
            message.content = {ai::ToolCallContent{
                    .id = "call-" + std::to_string(script->next + 1), .name = call.name, .arguments = call.arguments}};
            message.stop_reason = ai::AssistantStopReason::ToolUse;
        }
        ++script->next;
        return ai::ModelStream{ai::ModelStreamProducer{[message = std::move(message)](ai::AssistantEventSink sink,
                                                               ai::ModelStreamCompletion completion) mutable noexcept {
            auto delivered = sink(ai::AssistantDoneEvent{.reason = message.stop_reason, .message = message});
            if (!delivered)
                completion(std::unexpected(delivered.error()));
            else
                completion(std::move(message));
        }}};
    };
}

ToolRegistration tool(std::string name, std::string description, bool deferred = false) {
    ai::Tool decl{.name = name,
            .description = std::move(description),
            .parameters = support::JsonValue{support::JsonValue::object_t{{"type", "object"},
                    {"properties",
                            support::JsonValue::object_t{{"number",
                                    support::JsonValue::object_t{
                                            {"type", "number"}, {"description", "Issue number"}}}}}}}};
    return ToolRegistration{.declaration = std::move(decl),
            .execute = [tool_name = std::move(name)](
                               support::JsonValue, std::stop_token) -> support::AsyncResult<std::vector<ai::Content>> {
                return support::AsyncResult<std::vector<ai::Content>>{support::Expected<std::vector<ai::Content>>{
                        std::vector<ai::Content>{ai::text_content(tool_name + " ran")}}};
            },
            .deferred = deferred};
}

ToolRegistration failing_tool(std::string name, std::string description, bool deferred = false) {
    ai::Tool decl{.name = name,
            .description = std::move(description),
            .parameters = support::JsonValue{support::JsonValue::object_t{{"type", "object"}}}};
    return ToolRegistration{.declaration = std::move(decl),
            .execute = [tool_name = std::move(name)](
                               support::JsonValue, std::stop_token) -> support::AsyncResult<std::vector<ai::Content>> {
                return support::AsyncResult<std::vector<ai::Content>>{std::unexpected(
                        support::make_error(support::ErrorCode::Tool, tool_name + ": requested failure"))};
            },
            .deferred = deferred};
}

std::unique_ptr<Harness> open(Storage& storage, Registry& registry, std::shared_ptr<Script> script) {
    auto opened = consume(
            Harness::open(storage, HarnessOptions{.registry = &registry, .stream = scripted(std::move(script))}));
    if (!opened.has_value()) {
        FAIL("Harness::open failed: " << opened.error().message);
    }
    REQUIRE(opened.has_value());
    return std::move(opened.value());
}

ConversationId root(Harness& harness) {
    auto root = consume(harness.root());
    REQUIRE(root.has_value());
    return root.value();
}

SettledSubmission submit(Harness& harness, ConversationId conversation, std::string text = "go") {
    auto submitted = consume(harness.submit(
            conversation, SubmissionDraft{.body = InputSubmissionDraft{.content = chord::Json{std::move(text)}}}));
    REQUIRE(submitted.has_value());
    auto settled = consume(submitted.value().wait());
    REQUIRE(settled.has_value());
    CHECK(settled.value().status == "done");
    return std::move(settled.value());
}

support::JsonValue conversation_json(Harness& harness, ConversationId conversation) {
    auto view = consume(harness.conversation_view(conversation));
    REQUIRE(view.has_value());
    return view.value().to_support();
}

std::vector<support::JsonValue> tool_results(Harness& harness, ConversationId conversation) {
    std::vector<support::JsonValue> results;
    const auto json = conversation_json(harness, conversation);
    for (const auto& entry : json.at("entries").get_array()) {
        if (entry.at("kind").get_string() == "pi.tool-result")
            results.push_back(entry.at("data").at("model").get_array().front());
    }
    return results;
}

std::string result_text(const support::JsonValue& result) {
    std::string text;
    for (const auto& block : result.at("content").get_array()) {
        if (block.at("type").get_string() == "text") text += block.at("text").get_string();
    }
    return text;
}

std::vector<std::string> strings(const support::JsonValue& json) {
    std::vector<std::string> result;
    for (const auto& item : json.get_array())
        result.push_back(item.get_string());
    return result;
}

std::vector<std::vector<std::string>> system_loads(Harness& harness, ConversationId conversation) {
    std::vector<std::vector<std::string>> loads;
    const auto json = conversation_json(harness, conversation);
    for (const auto& entry : json.at("entries").get_array()) {
        if (entry.at("kind").get_string() != "pi.system") continue;
        const auto& message = entry.at("data").at("model").get_array().front();
        const auto added = message.get_if<support::JsonValue::object_t>()->find("toolsAdded");
        if (added == message.get_if<support::JsonValue::object_t>()->end()) continue;
        std::vector<std::string> names;
        for (const auto& tool : added->second.get_array())
            names.push_back(tool.at("name").get_string());
        loads.push_back(std::move(names));
    }
    return loads;
}

std::vector<std::vector<std::string>> frozen_system_loads(const support::JsonValue& expected) {
    std::vector<std::vector<std::string>> loads;
    for (const auto& entry : expected.get_array())
        loads.push_back(strings(entry.at("added")));
    return loads;
}

} // namespace

TEST_CASE("Durable tool_search loads one ranked deferred tool and the next request executes it",
        "[durable][tool_search][issue1095][spec]") {
    MemoryStorage storage;
    Registry registry;
    Extension tools_ext{.name = "tools"};
    tools_ext.tools.push_back(tool("alpha", "Alpha tool"));
    tools_ext.tools.push_back(tool("github_get_issue", "Get a GitHub issue by number", true));
    tools_ext.tools.push_back(tool("github_list_repos", "List the repositories of a user", true));
    REQUIRE(registry.install(std::move(tools_ext)).has_value());
    REQUIRE(registry.install(tool_search_extension()).has_value());
    auto script = std::make_shared<Script>(
            Script{.calls = {
                           {"tool_search",
                                   support::JsonValue{
                                           support::JsonValue::object_t{{"query", "github issue"}, {"limit", 1.0}}}},
                           {"github_get_issue", support::JsonValue{support::JsonValue::object_t{{"number", 42.0}}}},
                   }});
    auto harness = open(storage, registry, script);
    const auto conversation = root(*harness);

    submit(*harness, conversation);

    const auto frozen = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1056/codemode-toolsearch.json",
            "tool_search ranks a deferred tool, loads it through control.addTools, and the next call runs it");
    REQUIRE(frozen.has_value());
    REQUIRE(script->offered.size() == 3);
    CHECK(script->offered.front() == strings(frozen->expected().at("offeredBefore")));
    CHECK(script->offered.at(1) == strings(frozen->expected().at("offeredAfter")));
    CHECK(script->offered.back() == strings(frozen->expected().at("offeredAfter")));
    const auto results = tool_results(*harness, conversation);
    REQUIRE(results.size() == 2);
    CHECK(result_text(results.at(0)) == frozen->expected().at("searchResult").get_string());
    CHECK(result_text(results.at(1)) == frozen->expected().at("toolResult").get_string());
    CHECK(system_loads(*harness, conversation) == frozen_system_loads(frozen->expected().at("systemToolsAdded")));

    auto doc_record = storage.find_document(
            "pi.agent:v1", DocumentAddress{.conversation_id = conversation}, DocumentPoint::current());
    REQUIRE(doc_record.has_value());
    REQUIRE(doc_record->has_value());
    auto stored = storage.document((*doc_record)->id, DocumentPoint::current());
    REQUIRE(stored.has_value());
    REQUIRE(stored->has_value());
    const auto* agent = (*stored)->value.as_object();
    REQUIRE(agent != nullptr);
    CHECK(json_equals(agent->at("modelTools"), frozen->expected().at("modelTools")));
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("Durable tool_search no-match leaves the offered set and persisted loadout unchanged",
        "[durable][tool_search][issue1095][spec]") {
    MemoryStorage storage;
    Registry registry;
    Extension tools_ext{.name = "tools"};
    tools_ext.tools.push_back(tool("github_get_issue", "Get a GitHub issue by number", true));
    REQUIRE(registry.install(std::move(tools_ext)).has_value());
    REQUIRE(registry.install(tool_search_extension()).has_value());
    auto script = std::make_shared<Script>(Script{
            .calls = {
                    {"tool_search", support::JsonValue{support::JsonValue::object_t{{"query", "weather forecast"}}}}}});
    auto harness = open(storage, registry, script);
    const auto conversation = root(*harness);

    submit(*harness, conversation);

    const auto frozen = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1056/codemode-toolsearch.json",
            "an unrelated tool_search query loads nothing and leaves the offered set unchanged");
    REQUIRE(frozen.has_value());
    REQUIRE(script->offered.size() == 2);
    CHECK(script->offered.at(0) == strings(frozen->expected().at("offered")));
    CHECK(script->offered.at(1) == strings(frozen->expected().at("offered")));
    const auto results = tool_results(*harness, conversation);
    REQUIRE(results.size() == 1);
    CHECK(result_text(results.front()) == frozen->expected().at("searchResult").get_string());
    auto doc_record = storage.find_document(
            "pi.agent:v1", DocumentAddress{.conversation_id = conversation}, DocumentPoint::current());
    REQUIRE(doc_record.has_value());
    REQUIRE(doc_record->has_value());
    auto stored = storage.document((*doc_record)->id, DocumentPoint::current());
    REQUIRE(stored.has_value());
    REQUIRE(stored->has_value());
    const auto* agent = (*stored)->value.as_object();
    REQUIRE(agent != nullptr);
    CHECK_FALSE(agent->contains("modelTools"));
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("Repeated durable tool_search calls in one run load each ranked match once in order and keep them declared",
        "[durable][tool_search][issue1095][spec]") {
    MemoryStorage storage;
    Registry registry;
    Extension tools_ext{.name = "response-tools"};
    tools_ext.tools.push_back(tool("slow", "Respond after a short delay", true));
    tools_ext.tools.push_back(tool("hang", "Respond after a very long wait and hang", true));
    tools_ext.tools.push_back(
            tool("crash", "Respond after an extremely long wait and crash and exit the process", true));
    REQUIRE(registry.install(std::move(tools_ext)).has_value());
    REQUIRE(registry.install(tool_search_extension()).has_value());

    const auto search = [] {
        return ToolCall{
                "tool_search", support::JsonValue{support::JsonValue::object_t{{"query", "respond"}, {"limit", 1.0}}}};
    };
    auto script = std::make_shared<Script>(Script{.calls = {
                                                          search(),
                                                          search(),
                                                          search(),
                                                          {"slow", support::JsonValue{support::JsonValue::object_t{}}},
                                                  }});
    auto harness = open(storage, registry, script);
    const auto conversation = root(*harness);

    submit(*harness, conversation, "load the response tools one at a time");

    const auto results = tool_results(*harness, conversation);
    REQUIRE(results.size() == 4);
    CHECK(result_text(results.at(0)) ==
            "Loaded 1 tool. They are available from your next call:\n- slow: Respond after a short delay");
    CHECK(result_text(results.at(1)) ==
            "Loaded 1 tool. They are available from your next call:\n- hang: Respond after a very long wait and hang");
    CHECK(result_text(results.at(2)) ==
            "Loaded 1 tool. They are available from your next call:\n- crash: Respond after an extremely long wait and "
            "crash and exit the process");
    CHECK(result_text(results.at(3)) == "slow ran");

    REQUIRE(script->offered.size() == 5);
    const auto offered = [&](std::size_t request, std::string_view name) {
        return std::ranges::find(script->offered.at(request), name) != script->offered.at(request).end();
    };
    for (const auto name : {"slow", "hang", "crash"}) {
        CHECK_FALSE(offered(0, name));
    }
    CHECK(offered(1, "slow"));
    CHECK_FALSE(offered(1, "hang"));
    CHECK_FALSE(offered(1, "crash"));
    CHECK(offered(2, "slow"));
    CHECK(offered(2, "hang"));
    CHECK_FALSE(offered(2, "crash"));
    CHECK(offered(3, "slow"));
    CHECK(offered(3, "hang"));
    CHECK(offered(3, "crash"));
    CHECK(offered(4, "slow"));
    CHECK(offered(4, "hang"));
    CHECK(offered(4, "crash"));

    CHECK(system_loads(*harness, conversation) == std::vector<std::vector<std::string>>{
                                                          {"tool_search"},
                                                          {"slow"},
                                                          {"hang"},
                                                          {"crash"},
                                                  });

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("Reopened durable sessions load the next-ranked deferred tool and never re-announce an already loaded one",
        "[durable][tool_search][issue1095][spec]") {
    tests::TempWorkspace workspace;
    const auto db_path = workspace.path() / "reopen.sqlite";
    Registry registry;
    Extension tools_ext{.name = "response-tools"};
    tools_ext.tools.push_back(tool("slow", "Respond after a short delay", true));
    tools_ext.tools.push_back(tool("hang", "Respond after a very long wait and hang", true));
    tools_ext.tools.push_back(
            tool("crash", "Respond after an extremely long wait and crash and exit the process", true));
    REQUIRE(registry.install(std::move(tools_ext)).has_value());
    REQUIRE(registry.install(tool_search_extension()).has_value());

    const auto search = [] {
        return ToolCall{
                "tool_search", support::JsonValue{support::JsonValue::object_t{{"query", "respond"}, {"limit", 1.0}}}};
    };

    // First session: starts without deferred tools, loads only the best match (slow).
    {
        auto storage = SqliteStorage::open(db_path);
        REQUIRE(storage.has_value());
        auto script = std::make_shared<Script>(Script{.calls = {search()}});
        auto harness = open(**storage, registry, script);
        const auto conversation = root(*harness);

        submit(*harness, conversation, "load the best response tool");

        const auto results = tool_results(*harness, conversation);
        REQUIRE(results.size() == 1);
        CHECK(result_text(results.front()) ==
                "Loaded 1 tool. They are available from your next call:\n- slow: Respond after a short delay");
        REQUIRE(script->offered.size() == 2);
        CHECK_FALSE(std::ranges::contains(script->offered.at(0), "slow"));
        CHECK_FALSE(std::ranges::contains(script->offered.at(0), "hang"));
        CHECK_FALSE(std::ranges::contains(script->offered.at(0), "crash"));
        CHECK(std::ranges::contains(script->offered.at(1), "slow"));
        CHECK_FALSE(std::ranges::contains(script->offered.at(1), "hang"));
        CHECK_FALSE(std::ranges::contains(script->offered.at(1), "crash"));

        REQUIRE(consume(harness->close()).has_value());
        harness.reset();
        REQUIRE((*storage)->close().has_value());
    }

    // Second session: reopens SQLite, reloads slow on request 0, loads hang, and runs slow.
    {
        auto storage = SqliteStorage::open(db_path);
        REQUIRE(storage.has_value());
        auto script =
                std::make_shared<Script>(Script{.calls = {
                                                        search(),
                                                        {"slow", support::JsonValue{support::JsonValue::object_t{}}},
                                                }});
        auto harness = open(**storage, registry, script);
        const auto conversation = ConversationId{.value = 1};

        submit(*harness, conversation, "load the next response tool");

        const auto results = tool_results(*harness, conversation);
        REQUIRE(results.size() == 3);
        CHECK(result_text(results.at(1)) == "Loaded 1 tool. They are available from your next call:\n- hang: Respond "
                                            "after a very long wait and hang");
        CHECK(result_text(results.at(2)) == "slow ran");

        REQUIRE(script->offered.size() == 3);
        CHECK(std::ranges::contains(script->offered.at(0), "slow"));
        CHECK_FALSE(std::ranges::contains(script->offered.at(0), "hang"));
        CHECK_FALSE(std::ranges::contains(script->offered.at(0), "crash"));
        CHECK(std::ranges::contains(script->offered.at(1), "slow"));
        CHECK(std::ranges::contains(script->offered.at(1), "hang"));
        CHECK_FALSE(std::ranges::contains(script->offered.at(1), "crash"));

        REQUIRE(consume(harness->close()).has_value());
        harness.reset();
        REQUIRE((*storage)->close().has_value());
    }

    // Third session: reopens SQLite, carries both earlier loadouts, loads crash.
    {
        auto storage = SqliteStorage::open(db_path);
        REQUIRE(storage.has_value());
        auto script = std::make_shared<Script>(Script{.calls = {search()}});
        auto harness = open(**storage, registry, script);
        const auto conversation = ConversationId{.value = 1};

        submit(*harness, conversation, "load the last response tool");

        const auto results = tool_results(*harness, conversation);
        REQUIRE(results.size() == 4);
        CHECK(result_text(results.at(3)) ==
                "Loaded 1 tool. They are available from your next call:\n- crash: Respond after an extremely long "
                "wait and crash and exit the process");

        REQUIRE(script->offered.size() == 2);
        CHECK(std::ranges::contains(script->offered.at(0), "slow"));
        CHECK(std::ranges::contains(script->offered.at(0), "hang"));
        CHECK_FALSE(std::ranges::contains(script->offered.at(0), "crash"));
        CHECK(std::ranges::contains(script->offered.at(1), "slow"));
        CHECK(std::ranges::contains(script->offered.at(1), "hang"));
        CHECK(std::ranges::contains(script->offered.at(1), "crash"));

        REQUIRE(consume(harness->close()).has_value());
        harness.reset();
        REQUIRE((*storage)->close().has_value());
    }
}

TEST_CASE("Forked durable sessions persist independent deferred loadouts across reopen and run their respective tools",
        "[durable][tool_search][issue1095][spec]") {
    tests::TempWorkspace workspace;
    const auto db_path = workspace.path() / "fork.sqlite";
    Registry registry;
    Extension tools_ext{.name = "test-tools"};
    tools_ext.tools.push_back(tool("echo", "Echo incoming text back", true));
    tools_ext.tools.push_back(failing_tool("fail", "Always report error fail", true));
    REQUIRE(registry.install(std::move(tools_ext)).has_value());
    REQUIRE(registry.install(tool_search_extension()).has_value());

    ConversationId echo_branch_id{};
    ConversationId fail_branch_id{};

    {
        auto storage = SqliteStorage::open(db_path);
        REQUIRE(storage.has_value());
        auto script = std::make_shared<Script>(Script{.calls = {}});
        auto harness = open(**storage, registry, script);
        const auto parent = root(*harness);

        auto settled = submit(*harness, parent, "shared fork point");
        REQUIRE(settled.entry.has_value());
        const auto fork_point = *settled.entry;

        auto fork_echo = consume(harness->fork_conversation(parent, fork_point));
        if (!fork_echo.has_value()) {
            FAIL("fork_echo error: " << fork_echo.error().message);
        }
        auto fork_fail = consume(harness->fork_conversation(parent, fork_point));
        if (!fork_fail.has_value()) {
            FAIL("fork_fail error: " << fork_fail.error().message);
        }
        REQUIRE(fork_echo.has_value());
        REQUIRE(fork_fail.has_value());
        CHECK(fork_echo.value().id != fork_fail.value().id);
        echo_branch_id = fork_echo.value().id;
        fail_branch_id = fork_fail.value().id;
        REQUIRE(consume(harness->close()).has_value());
        harness.reset();
        REQUIRE((*storage)->close().has_value());
    }

    // Echo branch: search and run echo.
    {
        auto storage = SqliteStorage::open(db_path);
        if (!storage.has_value()) {
            FAIL("SqliteStorage::open failed: " << storage.error().message);
        }
        REQUIRE(storage.has_value());
        auto echo_script = std::make_shared<Script>(
                Script{.calls = {
                               ToolCall{"tool_search",
                                       support::JsonValue{support::JsonValue::object_t{
                                               {"query", "echo incoming text"}, {"limit", 1.0}}}},
                               ToolCall{"echo", support::JsonValue{support::JsonValue::object_t{}}},
                       }});
        auto echo_harness = open(**storage, registry, echo_script);
        submit(*echo_harness, echo_branch_id, "load echo");
        REQUIRE(echo_script->offered.size() == 3);
        CHECK_FALSE(std::ranges::contains(echo_script->offered.at(0), "echo"));
        CHECK_FALSE(std::ranges::contains(echo_script->offered.at(0), "fail"));
        CHECK(std::ranges::contains(echo_script->offered.at(1), "echo"));
        CHECK_FALSE(std::ranges::contains(echo_script->offered.at(1), "fail"));
        const auto echo_results = tool_results(*echo_harness, echo_branch_id);
        REQUIRE(echo_results.size() == 2);
        CHECK(result_text(echo_results.at(0)) ==
                "Loaded 1 tool. They are available from your next call:\n- echo: Echo incoming text back");
        CHECK(result_text(echo_results.at(1)) == "echo ran");
        CHECK_FALSE(echo_results.at(1).at("isError").get_boolean());
        REQUIRE(consume(echo_harness->close()).has_value());
        echo_harness.reset();
        REQUIRE((*storage)->close().has_value());
    }

    // Fail branch: search and run fail.
    {
        auto storage = SqliteStorage::open(db_path);
        REQUIRE(storage.has_value());
        auto fail_script = std::make_shared<Script>(
                Script{.calls = {
                               ToolCall{"tool_search",
                                       support::JsonValue{support::JsonValue::object_t{
                                               {"query", "always report error fail"}, {"limit", 1.0}}}},
                               ToolCall{"fail", support::JsonValue{support::JsonValue::object_t{}}},
                       }});
        auto fail_harness = open(**storage, registry, fail_script);
        submit(*fail_harness, fail_branch_id, "load fail");
        REQUIRE(fail_script->offered.size() == 3);
        CHECK_FALSE(std::ranges::contains(fail_script->offered.at(0), "echo"));
        CHECK_FALSE(std::ranges::contains(fail_script->offered.at(0), "fail"));
        CHECK_FALSE(std::ranges::contains(fail_script->offered.at(1), "echo"));
        CHECK(std::ranges::contains(fail_script->offered.at(1), "fail"));
        const auto fail_results = tool_results(*fail_harness, fail_branch_id);
        REQUIRE(fail_results.size() == 2);
        CHECK(result_text(fail_results.at(0)) ==
                "Loaded 1 tool. They are available from your next call:\n- fail: Always report error fail");
        CHECK(result_text(fail_results.at(1)) == "fail: requested failure");
        CHECK(fail_results.at(1).at("isError").get_boolean());
        REQUIRE(consume(fail_harness->close()).has_value());
        fail_harness.reset();
        REQUIRE((*storage)->close().has_value());
    }

    // Reopen SQLite storage: each fork exposes its independent loadout and runs its tool.
    {
        auto storage = SqliteStorage::open(db_path);
        REQUIRE(storage.has_value());

        // Reopened echo branch: offers echo, not fail; runs echo.
        auto reopened_echo_script = std::make_shared<Script>(
                Script{.calls = {
                               ToolCall{"echo", support::JsonValue{support::JsonValue::object_t{}}},
                       }});
        auto echo_harness = open(**storage, registry, reopened_echo_script);
        submit(*echo_harness, echo_branch_id, "run echo again");
        REQUIRE(reopened_echo_script->offered.size() == 2);
        CHECK(std::ranges::contains(reopened_echo_script->offered.at(0), "echo"));
        CHECK_FALSE(std::ranges::contains(reopened_echo_script->offered.at(0), "fail"));
        const auto echo_results = tool_results(*echo_harness, echo_branch_id);
        REQUIRE(echo_results.size() == 3);
        CHECK(result_text(echo_results.back()) == "echo ran");
        CHECK_FALSE(echo_results.back().at("isError").get_boolean());
        REQUIRE(consume(echo_harness->close()).has_value());
        echo_harness.reset();
        REQUIRE((*storage)->close().has_value());
    }

    {
        auto storage = SqliteStorage::open(db_path);
        REQUIRE(storage.has_value());

        // Reopened fail branch: offers fail, not echo; runs fail.
        auto reopened_fail_script = std::make_shared<Script>(
                Script{.calls = {
                               ToolCall{"fail", support::JsonValue{support::JsonValue::object_t{}}},
                       }});
        auto fail_harness = open(**storage, registry, reopened_fail_script);
        submit(*fail_harness, fail_branch_id, "run fail again");
        REQUIRE(reopened_fail_script->offered.size() == 2);
        CHECK_FALSE(std::ranges::contains(reopened_fail_script->offered.at(0), "echo"));
        CHECK(std::ranges::contains(reopened_fail_script->offered.at(0), "fail"));
        const auto fail_results = tool_results(*fail_harness, fail_branch_id);
        REQUIRE(fail_results.size() == 3);
        CHECK(result_text(fail_results.back()) == "fail: requested failure");
        CHECK(fail_results.back().at("isError").get_boolean());
        REQUIRE(consume(fail_harness->close()).has_value());
        fail_harness.reset();
        REQUIRE((*storage)->close().has_value());
    }
}

} // namespace cch::tests::durable
