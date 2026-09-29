// Lazy Tool Activation and Upstream `instructions` surfacing, end to end
// (issue #847; spec #833 stories 12-16, 18, 36).
//
// The cases cross the one production door — `create_agent_session_async` —
// for everything the product does with a `lazy` Upstream, and drive the
// Upstream-tool binding directly where a case is about a value the session
// assembly only ever fills. The injected seams are the MCP Host's own
// transport (`tests::ScriptedMcpTransport`) and the connection timer
// (`tests::ScriptedMcpDelay`); no second seam is introduced.

#include <cch/agent/harness/session/SessionStore.hpp>
#include <cch/coding_agent/AgentConfigDir.hpp>
#include "ai/ModelStreamBridge.hpp"
#include "coding_agent/AgentSession.hpp"
#include <cch/coding_agent/McpToolBinding.hpp>
#include "coding_agent/runtime/AgentSessionCreationRequest.hpp"
#include "coding_agent/runtime/McpToolBinding.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/AgentRootFixture.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/Json.hpp"
#include "support/ModelsFixture.hpp"
#include "support/PumpUntil.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/RuntimeLoopDriver.hpp"
#include "support/ScriptedMcpTransport.hpp"
#include "support/StreamAdapterFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

namespace runtime_ns = cch::coding_agent::runtime;
using coding_agent::McpPublishedTool;
using tests::ScriptedMcpAnswer;

/// Every wait in this file is a cap, not a wait: a passing case returns as
/// soon as its condition holds.
constexpr std::chrono::milliseconds kBudget{5000};

/// A property name that appears nowhere except inside the Upstream's own JSON
/// Schema, so "the search catalog carries no schema" is checkable text rather
/// than an assumption.
constexpr std::string_view kSchemaOnlyMarker{"schema_only_marker"};

/// One `tools/list` tool entry whose properties are required, so a call that
/// omits one is invalid against the Upstream's own schema.
[[nodiscard]] support::JsonValue upstream_tool_entry(std::string name, std::string description, std::string property) {
    using JsonValue = support::JsonValue;
    JsonValue::array_t required_values{JsonValue(property)};
    return JsonValue::object_t{
            {"name", JsonValue(std::move(name))},
            {"description", JsonValue(std::move(description))},
            {"inputSchema",
                    JsonValue::object_t{{"type", JsonValue("object")},
                            {"properties",
                                    JsonValue::object_t{
                                            {property, JsonValue::object_t{{"type", JsonValue("string")}}}}},
                            {"required", required_values}}},
    };
}

/// A conforming Modern Era Upstream that advertises the given catalog.
void answer_upstream(tests::ScriptedMcpTransport& transport,
        std::vector<support::JsonValue> tools,
        std::string instructions = "Call execute, not raw HTTP.") {
    transport.answer("server/discover",
            ScriptedMcpAnswer{.result = tests::discover_result("executor", "1.4.0", std::move(instructions))});
    transport.answer("tools/list", ScriptedMcpAnswer{.result = tests::tool_list_result(std::move(tools))});
    transport.answer("tools/call",
            ScriptedMcpAnswer{.result = tests::tool_call_result(support::JsonValue{support::JsonValue::array_t{}})});
}

/// One `tools/call` request's decoded `params.name`, in request order.
[[nodiscard]] std::vector<std::string> called_tool_names(const tests::ScriptedMcpTransport& transport) {
    std::vector<std::string> names;
    for (std::size_t index = 0; index < transport.request_count(); ++index) {
        const auto method = transport.recorded_method(index);
        REQUIRE(method.has_value());
        if (*method != "tools/call") {
            continue;
        }
        const auto params = transport.recorded_params(index);
        REQUIRE(params.has_value());
        const auto* object = params->get_if<support::JsonValue::object_t>();
        REQUIRE(object != nullptr);
        const auto found = object->find("name");
        REQUIRE(found != object->end());
        REQUIRE(found->second.holds<std::string>());
        names.push_back(found->second.get<std::string>());
    }
    return names;
}

/// Consume one `AsyncResult` to its terminal outcome on a local loop.
template <typename T> [[nodiscard]] support::Expected<T> drive_locally(support::AsyncResult<T> operation) {
    boost::asio::io_context local;
    auto outcome = std::make_shared<support::Expected<T>>(
            std::unexpected(support::make_error(support::ErrorCode::Busy, "the operation never completed")));
    boost::asio::co_spawn(
            local,
            [operation = std::move(operation), outcome]() mutable -> boost::asio::awaitable<void> {
                *outcome = co_await support::detail::await_async_result(std::move(operation));
                co_return;
            },
            boost::asio::detached);
    local.run();
    return std::move(*outcome);
}

/// An `agent::Tool` value driven through one invocation.
[[nodiscard]] support::Expected<agent::AsyncToolExecutionResult> execute_bound(
        agent::Tool tool, support::JsonValue arguments) {
    if (!tool.execute) {
        return std::unexpected(support::make_error(support::ErrorCode::Tool, "the bound tool has no execute"));
    }
    auto execute = std::move(tool.execute);
    return drive_locally(execute(agent::ToolInvocation{.call_id = "call-1",
                                         .name = tool.definition.name,
                                         .arguments = std::move(arguments),
                                         .raw_arguments = "{}"},
            std::stop_token{},
            agent::ToolUpdateSink{}));
}

[[nodiscard]] std::vector<std::string> tool_names_in_request(const tests::RecordedProviderRequest& request) {
    std::vector<std::string> names;
    names.reserve(request.context.tools.size());
    for (const auto& tool : request.context.tools) {
        names.push_back(tool.name);
    }
    return names;
}

/// The serialized JSON Schema one request offered for one tool, or an empty
/// string when the request did not offer that tool at all.
[[nodiscard]] std::string schema_in_request(const tests::RecordedProviderRequest& request, std::string_view tool_name) {
    for (const auto& tool : request.context.tools) {
        if (tool.name != tool_name) {
            continue;
        }
        const auto serialized = support::write_json(tool.parameters);
        return serialized ? *serialized : std::string{};
    }
    return {};
}

[[nodiscard]] bool contains(const std::vector<std::string>& values, std::string_view value) {
    return std::ranges::find(values, value) != values.end();
}

/// The last tool result the model saw, in tool-call order, beside its error
/// flag.
struct SeenToolResult {
    std::string text{};
    bool is_error{false};
};

[[nodiscard]] std::vector<SeenToolResult> tool_results(const std::vector<tests::RecordedProviderRequest>& requests) {
    std::vector<SeenToolResult> results;
    for (const auto& request : requests) {
        for (const auto& message : request.context.messages) {
            const auto* tool_result = std::get_if<ai::ToolResultMessage>(&message);
            if (tool_result == nullptr) {
                continue;
            }
            std::string text;
            for (const auto& block : tool_result->content) {
                if (const auto* content = std::get_if<ai::TextContent>(&block)) {
                    text += content->text;
                }
            }
            results.push_back(SeenToolResult{.text = std::move(text), .is_error = tool_result->is_error});
        }
    }
    return results;
}

/// One assistant message whose only content is a single tool call.
[[nodiscard]] ai::AssistantMessage tool_call_response(
        std::string call_id, std::string tool_name, std::string raw_arguments) {
    auto message = ai::assistant_text_message("");
    message.content.clear();
    message.content.emplace_back(
            ai::tool_call_content(std::move(call_id), std::move(tool_name), std::move(raw_arguments)));
    return message;
}

/// A scripted chat client that serves queued assistant messages in order and
/// records every request, so a case can read exactly which tools the model was
/// offered on which request, and with which System Prompt.
class ActivationProvider final : public tests::ScriptedProvider {
public:
    ActivationProvider() : ScriptedProvider("sdk-host") {}

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext context, coding_agent::ModelRuntimeTestStreamOptions options) override {
        return ai::detail::make_model_stream(
                [this, model = std::move(model), context = std::move(context), options = std::move(options)](
                        ai::AssistantEventSink sink)
                        -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
                    requests.push_back(tests::RecordedProviderRequest{model, context, options});
                    ai::AssistantMessage response = ai::assistant_text_message("done");
                    if (!responses.empty()) {
                        response = std::move(responses.front());
                        responses.erase(responses.begin());
                    }
                    response = tests::stamped_response(std::move(response), model);
                    if (sink) {
                        CCH_TRY_VOID(sink(ai::AssistantStartEvent{response}));
                    }
                    co_return response;
                });
    }

    std::vector<ai::AssistantMessage> responses;
    std::vector<tests::RecordedProviderRequest> requests;
};

/// A session assembled against a temp Agent Config Directory, with the MCP
/// Host's transport and timer injected and no ambient provider key resolving
/// as configured.
struct ActivationFixture {
    tests::TempWorkspace workspace;
    std::filesystem::path agent_dir;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard kimi_guard{"KIMI_API_KEY"};
    tests::EnvVarGuard deepseek_guard{"DEEPSEEK_API_KEY"};
    tests::RuntimeFixture runtime;
    tests::RuntimeLoopDriver driver;
    std::shared_ptr<tests::ScriptedMcpTransport> transport{std::make_shared<tests::ScriptedMcpTransport>()};
    tests::ScriptedMcpDelay delay;
    std::shared_ptr<ActivationProvider> client{std::make_shared<ActivationProvider>()};

    ActivationFixture() : driver(runtime) {
        home_guard.set(workspace.path().string());
        agent_dir = tests::agent_root_under_home(workspace.path());
        std::filesystem::create_directories(agent_dir);
        kimi_guard.unset();
        deepseek_guard.unset();
    }

    void write_settings(std::string_view content) const {
        std::ofstream output(agent_dir / "settings.json", std::ios::binary | std::ios::trunc);
        output << content;
    }

    void write_trust_store(std::string_view content) const {
        std::ofstream output(agent_dir / "mcp-trust.json", std::ios::binary | std::ios::trunc);
        output << content;
    }

    [[nodiscard]] std::unique_ptr<coding_agent::AgentSession> create() {
        return create_with(coding_agent::InMemorySessionTarget{});
    }

    [[nodiscard]] std::unique_ptr<coding_agent::AgentSession> create_with(coding_agent::SessionTarget target) {
        runtime_ns::AgentSessionCreationRequest request;
        request.execution_runtime_target = runtime.make_target();
        request.session_facts.no_skills = true;
        request.session_facts.no_prompt_templates = true;
        request.workspace = workspace.path();
        request.session_target = std::move(target);
        request.mcp_transport = transport;
        request.mcp_delay = [this](std::chrono::milliseconds wait, std::stop_token stop_token) {
            return delay.request(wait, stop_token);
        };
        auto models = tests::models_from_provider(client);
        auto created = runtime.run(coding_agent::create_agent_session_async(
                std::move(request), std::nullopt, tests::cli_fake_overrides(std::move(models))));
        REQUIRE(created.has_value());
        return std::move(created->session);
    }

    [[nodiscard]] std::expected<void, support::Error> prompt(coding_agent::AgentSession& session, std::string text) {
        boost::asio::io_context local;
        auto outcome = std::make_shared<std::expected<void, support::Error>>(
                std::unexpected(support::make_error(support::ErrorCode::Busy, "the prompt never completed")));
        boost::asio::co_spawn(
                local,
                [&outcome, &session, text = std::move(text)]() -> boost::asio::awaitable<void> {
                    *outcome = co_await session.prompt(std::move(text));
                    co_return;
                },
                boost::asio::detached);
        local.run();
        return std::move(*outcome);
    }

    /// Wait for `condition` on the driven Runtime loop. A cap, not a wait.
    template <typename Condition> [[nodiscard]] static bool wait_until(Condition condition) {
        const auto deadline = std::chrono::steady_clock::now() + kBudget;
        while (!condition() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        return condition();
    }
};

constexpr std::string_view kLazyServerSettings = R"({"mcpServers": {"executor": {"url": "https://mcp.example/mcp"}}})";
constexpr std::string_view kEagerServerSettings =
        R"({"mcpServers": {"executor": {"url": "https://mcp.example/mcp", "activation": "eager"}}})";
constexpr std::string_view kLazyAskServerSettings =
        R"({"mcpServers": {"executor": {"url": "https://mcp.example/mcp", "approval": "ask"}}})";
constexpr std::string_view kTrustedStore = R"({"executor": true})";

/// How many `toolsAdded` records the persisted transcript holds for one tool.
[[nodiscard]] std::size_t tools_added_records(const std::filesystem::path& session_file, std::string_view tool_name) {
    auto store = harness::session::SessionStore::open_existing(session_file);
    REQUIRE(store.has_value());
    std::size_t records = 0;
    for (const auto& entry : store->entries()) {
        if (entry.kind != harness::session::SessionEntryKind::Message || !entry.message) {
            continue;
        }
        const auto* system = std::get_if<ai::SystemMessage>(&*entry.message);
        if (system == nullptr) {
            continue;
        }
        const auto names = std::ranges::any_of(
                system->tools_added, [tool_name](const ai::Tool& tool) { return tool.name == tool_name; });
        if (names) {
            records += 1;
        }
    }
    return records;
}

/// One connecting Upstream on a scripted transport, for the cases that drive
/// the binding value directly.
[[nodiscard]] std::shared_ptr<mcp::UpstreamConnection> connecting_upstream(
        std::string server_id, const std::shared_ptr<tests::ScriptedMcpTransport>& transport) {
    transport->answer("server/discover", ScriptedMcpAnswer{.result = tests::discover_result()});
    transport->answer("tools/list", ScriptedMcpAnswer{.result = tests::tool_list_result({})});
    auto connection = std::make_shared<mcp::UpstreamConnection>(std::move(server_id),
            transport,
            mcp::UpstreamConnectionOptions{
                    .url = "https://mcp.example/mcp",
                    .delay = [](std::chrono::milliseconds,
                                     std::stop_token) { return support::AsyncResult<void>(support::Expected<void>{}); },
            });
    REQUIRE(drive_locally(connection->connect()).has_value());
    return connection;
}

} // namespace

TEST_CASE("the lazy catalog lists tools without their schemas and activates one on request", "[mcp][issue847][spec]") {
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    auto connection = connecting_upstream("executor", transport);
    transport->answer("tools/list",
            ScriptedMcpAnswer{.result = tests::tool_list_result({upstream_tool_entry(
                                      "execute", "run codemode", std::string{kSchemaOnlyMarker})})});

    auto binding = std::make_shared<runtime_ns::McpToolBinding>();
    const mcp::UpstreamToolDescriptor execute{
            .name = "execute",
            .description = "run codemode",
            .parameters = support::JsonValue{support::JsonValue::object_t{{"type", support::JsonValue("object")},
                    {"properties",
                            support::JsonValue::object_t{{std::string{kSchemaOnlyMarker},
                                    support::JsonValue::object_t{{"type", support::JsonValue("string")}}}}}}},
    };
    REQUIRE(binding->record_lazy_catalog(connection, mcp::UpstreamCatalog{.tools = {execute}}).has_value());
    // The server's own guidance is recorded for the System Prompt, not for the
    // catalog: the model must not have to search for it.
    REQUIRE(binding->record_lazy_catalog(
                           connection, mcp::UpstreamCatalog{.tools = {}, .instructions = "Call execute, not raw HTTP."})
                    .has_value());

    // Nothing is published: the eager publication list stays empty, and the
    // staged queue is empty too, because no tool has been activated.
    CHECK(binding->published().empty());
    CHECK(binding->take_pending().empty());
    // The two meta-tools are staged exactly once, by the first lazy catalog.
    auto meta_tools = binding->take_pending_meta_tools();
    REQUIRE(meta_tools.size() == 2);
    CHECK(meta_tools[0].definition.name == coding_agent::kMcpSearchToolName);
    CHECK(meta_tools[1].definition.name == coding_agent::kMcpActivateToolName);
    CHECK(binding->take_pending_meta_tools().empty());
    const auto instructions = binding->instructions();
    REQUIRE(instructions.size() == 1);
    CHECK(instructions.front().server_id == "executor");
    CHECK(instructions.front().text == "Call execute, not raw HTTP.");

    // The search result is three compact fields and no schema: the property
    // name that exists only inside the Upstream's own JSON Schema is absent.
    const auto searched = execute_bound(std::move(meta_tools[0]), *support::read_json(R"({"query":"codemode"})"));
    REQUIRE(searched.has_value());
    CHECK_FALSE(searched->is_error);
    const std::string text = std::get<ai::TextContent>(searched->content.front()).text;
    CHECK(text.find("executor") != std::string::npos);
    CHECK(text.find("mcp__executor__execute") != std::string::npos);
    CHECK(text.find("run codemode") != std::string::npos);
    CHECK(text.find(kSchemaOnlyMarker) == std::string::npos);

    // Activating an unknown name is one failed call with a reason, and it
    // changes nothing. An `agent::Tool` is move-only, so the call runs against
    // a second binding holding the same catalog rather than reusing the value
    // the successful activation below consumes.
    auto refused_binding = std::make_shared<runtime_ns::McpToolBinding>();
    REQUIRE(refused_binding->record_lazy_catalog(connection, mcp::UpstreamCatalog{.tools = {execute}}).has_value());
    auto refused_meta = refused_binding->take_pending_meta_tools();
    REQUIRE(refused_meta.size() == 2);
    const auto unknown =
            execute_bound(std::move(refused_meta[1]), *support::read_json(R"({"name":"mcp__executor__nope"})"));
    REQUIRE(unknown.has_value());
    CHECK(unknown->is_error);
    CHECK(refused_binding->take_pending().empty());

    // Activating the catalogued tool stages its callable binding, and nothing
    // else in the catalog.
    const auto activated =
            execute_bound(std::move(meta_tools[1]), *support::read_json(R"({"name":"mcp__executor__execute"})"));
    REQUIRE(activated.has_value());
    CHECK_FALSE(activated->is_error);
    auto staged = binding->take_pending();
    REQUIRE(staged.size() == 1);
    CHECK(staged.front().tool.qualified_name == "mcp__executor__execute");

    // Sticky: a second activation of the same tool is a success that changed
    // nothing, so a refresh or a repeated call never re-adds a live tool.
    const auto again = binding->activate("mcp__executor__execute");
    REQUIRE(again.has_value());
    CHECK(*again == runtime_ns::McpActivationOutcome::AlreadyActive);
    CHECK(binding->take_pending().empty());

    // A catalog refresh — what a reconnect and a cache hit both deliver —
    // re-records the same descriptor and stages nothing at all. A live tool is
    // never un-activated and never re-bound, because either would invalidate
    // the provider's prompt cache (spec #833 story 16).
    REQUIRE(binding->record_lazy_catalog(connection, mcp::UpstreamCatalog{.tools = {execute}}).has_value());
    CHECK(binding->take_pending().empty());
    CHECK(binding->search("", "").size() == 1);
}

TEST_CASE("a lazy server's tools stay out of the tool surface until the model activates one", "[mcp][issue847][spec]") {
    ActivationFixture fixture;
    fixture.write_settings(kLazyServerSettings);
    fixture.write_trust_store(kTrustedStore);
    answer_upstream(*fixture.transport,
            {upstream_tool_entry("search", "search the corpus", std::string{kSchemaOnlyMarker})},
            "Prefer search over raw HTTP.");

    auto owned = fixture.create();
    auto& session = fixture.runtime.adopt_session(std::move(owned));
    REQUIRE(ActivationFixture::wait_until([&fixture] { return fixture.transport->request_count("tools/list") == 1; }));

    // The model searches first. The search is answered from the discovered
    // catalog, so it costs no upstream request at all.
    fixture.client->responses.push_back(tool_call_response("call-0", "mcp_search", "{}"));
    fixture.client->responses.push_back(ai::assistant_text_message("found it"));
    REQUIRE(fixture.prompt(session, "what can you do?").has_value());

    const auto& requests = fixture.client->requests;
    REQUIRE(requests.size() >= 2);
    // Both requests carry the two meta-tools, and neither carries the Upstream
    // tool: a `lazy` schema never reaches a real request unactivated.
    for (const auto& request : requests) {
        const auto names = tool_names_in_request(request);
        CHECK(contains(names, "mcp_search"));
        CHECK(contains(names, "mcp_activate"));
        CHECK_FALSE(contains(names, "mcp__executor__search"));
        CHECK(schema_in_request(request, "mcp__executor__search").empty());
    }
    CHECK(fixture.transport->request_count("tools/call") == 0);
    const auto results = tool_results(requests);
    REQUIRE_FALSE(results.empty());
    CHECK_FALSE(results.front().is_error);
    CHECK(results.front().text.find("mcp__executor__search") != std::string::npos);
    // The catalog line is compact: no schema, and specifically not the property
    // that exists only inside the Upstream's JSON Schema.
    CHECK(results.front().text.find(kSchemaOnlyMarker) == std::string::npos);
}

TEST_CASE("an activated tool's schema reaches the next real model request and it can be called",
        "[mcp][issue847][spec]") {
    ActivationFixture fixture;
    fixture.write_settings(kLazyServerSettings);
    fixture.write_trust_store(kTrustedStore);
    answer_upstream(*fixture.transport,
            {upstream_tool_entry("search", "search the corpus", std::string{kSchemaOnlyMarker})},
            "Prefer search over raw HTTP.");
    fixture.transport->answer("tools/call",
            ScriptedMcpAnswer{
                    .result = tests::tool_call_result(support::JsonValue{support::JsonValue::array_t{
                            support::JsonValue{support::JsonValue::object_t{{"type", support::JsonValue("text")},
                                    {"text", support::JsonValue("the answer")}}}}})});

    auto owned = fixture.create();
    auto& session = fixture.runtime.adopt_session(std::move(owned));
    REQUIRE(ActivationFixture::wait_until([&fixture] { return fixture.transport->request_count("tools/list") == 1; }));

    // One run: search, activate, then call the tool the activation made
    // callable. The activation takes effect at the next turn boundary, which is
    // the request after the `mcp_activate` result.
    fixture.client->responses.push_back(tool_call_response("call-0", "mcp_search", R"({"query":"corpus"})"));
    fixture.client->responses.push_back(
            tool_call_response("call-1", "mcp_activate", R"({"name":"mcp__executor__search"})"));
    fixture.client->responses.push_back(
            tool_call_response("call-2", "mcp__executor__search", R"({"schema_only_marker":"q"})"));
    fixture.client->responses.push_back(ai::assistant_text_message("done"));
    REQUIRE(fixture.prompt(session, "search please").has_value());

    const auto& requests = fixture.client->requests;
    REQUIRE(requests.size() == 4);
    // The request that carried the `mcp_activate` call did not yet offer the
    // tool; the one after it does, with the Upstream's own schema.
    CHECK_FALSE(contains(tool_names_in_request(requests[1]), "mcp__executor__search"));
    CHECK(contains(tool_names_in_request(requests[2]), "mcp__executor__search"));
    CHECK(schema_in_request(requests[2], "mcp__executor__search").find(kSchemaOnlyMarker) != std::string::npos);

    // The call reached the Upstream under its own tool name, and the model saw
    // the Upstream's own result.
    REQUIRE(called_tool_names(*fixture.transport) == std::vector<std::string>{"search"});
    const auto results = tool_results(requests);
    REQUIRE_FALSE(results.empty());
    CHECK_FALSE(results.back().is_error);
    CHECK(results.back().text.find("the answer") != std::string::npos);

    // The read model and the model-facing list agree, and the transcript
    // recorded the activation once.
    const auto active = session.snapshot().agent_state.active_tool_names;
    CHECK(contains(active, "mcp__executor__search"));
    CHECK(contains(active, "mcp_search"));
    CHECK(contains(active, "mcp_activate"));
}

TEST_CASE("activation needs no approval and the transcript records it once", "[mcp][issue847][spec]") {
    ActivationFixture fixture;
    // `approval: ask` is the call-time gate. Activation is not a call to the
    // server, so the model's search-activate loop must not be interrupted by
    // it.
    fixture.write_settings(kLazyAskServerSettings);
    fixture.write_trust_store(kTrustedStore);
    answer_upstream(*fixture.transport, {upstream_tool_entry("search", "search the corpus", "query")});
    const auto session_file = fixture.workspace.path() / "activation-session.jsonl";

    auto owned = fixture.create_with(coding_agent::ExplicitOpenOrCreateSessionTarget{session_file});
    auto& session = fixture.runtime.adopt_session(std::move(owned));
    REQUIRE(ActivationFixture::wait_until([&fixture] { return fixture.transport->request_count("tools/list") == 1; }));

    fixture.client->responses.push_back(
            tool_call_response("call-0", "mcp_activate", R"({"name":"mcp__executor__search"})"));
    fixture.client->responses.push_back(ai::assistant_text_message("done"));
    REQUIRE(fixture.prompt(session, "activate it").has_value());

    const auto results = tool_results(fixture.client->requests);
    REQUIRE_FALSE(results.empty());
    CHECK_FALSE(results.front().is_error);
    // Nothing reached the Upstream: activation is not a tool call.
    CHECK(fixture.transport->request_count("tools/call") == 0);
    CHECK(contains(session.snapshot().agent_state.active_tool_names, "mcp__executor__search"));
    CHECK(tools_added_records(session_file, "mcp__executor__search") == 1);
    // The meta-tools are recorded under their own names, which is what makes a
    // resumed session's replay carry them too.
    CHECK(tools_added_records(session_file, "mcp_search") == 1);
    CHECK(tools_added_records(session_file, "mcp_activate") == 1);
    session.close();

    // Resume: the transcript names the tool, and the session restores it from
    // the catalog without the model activating it again and without a second
    // `toolsAdded` record.
    auto resumed = fixture.create_with(coding_agent::ExplicitResumeSessionTarget{session_file});
    auto& resumed_session = fixture.runtime.adopt_session(std::move(resumed));
    REQUIRE(ActivationFixture::wait_until([&fixture] { return fixture.transport->request_count("tools/list") == 2; }));
    fixture.client->responses.push_back(ai::assistant_text_message("hi"));
    REQUIRE(fixture.prompt(resumed_session, "carry on").has_value());

    CHECK(contains(resumed_session.snapshot().agent_state.active_tool_names, "mcp__executor__search"));
    // Neither the tool nor the meta-tools are recorded a second time: the
    // transcript is the record of the loadout, and a resume re-binds the
    // callable objects rather than re-announcing them.
    CHECK(tools_added_records(session_file, "mcp__executor__search") == 1);
    CHECK(tools_added_records(session_file, "mcp_search") == 1);
    CHECK(tools_added_records(session_file, "mcp_activate") == 1);
    // The restored tool is model-facing again, without a second activation.
    const auto& requests = fixture.client->requests;
    REQUIRE_FALSE(requests.empty());
    const auto& last = requests.back();
    CHECK(contains(tool_names_in_request(last), "mcp__executor__search"));
    CHECK(schema_in_request(last, "mcp__executor__search").find("query") != std::string::npos);
}

TEST_CASE("the meta-tools exist only once a lazy Upstream is connected", "[mcp][issue847][spec]") {
    SECTION("a configured but still pending Upstream registers nothing") {
        ActivationFixture fixture;
        fixture.write_settings(kLazyServerSettings);
        fixture.write_trust_store(kTrustedStore);
        // The Upstream never answers the era probe, so it never connects.
        fixture.transport->hold("server/discover");

        auto owned = fixture.create();
        auto& session = fixture.runtime.adopt_session(std::move(owned));
        REQUIRE(ActivationFixture::wait_until(
                [&fixture] { return fixture.transport->request_count("server/discover") == 1; }));
        REQUIRE(fixture.prompt(session, "hi").has_value());

        REQUIRE_FALSE(fixture.client->requests.empty());
        const auto names = tool_names_in_request(fixture.client->requests.front());
        CHECK_FALSE(contains(names, "mcp_search"));
        CHECK_FALSE(contains(names, "mcp_activate"));
        session.close();
    }

    SECTION("an eager Upstream publishes its tools and no meta-tool") {
        ActivationFixture fixture;
        fixture.write_settings(kEagerServerSettings);
        fixture.write_trust_store(kTrustedStore);
        answer_upstream(*fixture.transport, {upstream_tool_entry("search", "search the corpus", "query")});

        auto owned = fixture.create();
        auto& session = fixture.runtime.adopt_session(std::move(owned));
        REQUIRE(ActivationFixture::wait_until([&session] { return session.mcp_published_tools().size() == 1; }));
        REQUIRE(fixture.prompt(session, "hi").has_value());

        REQUIRE_FALSE(fixture.client->requests.empty());
        const auto names = tool_names_in_request(fixture.client->requests.back());
        // An eager server has nothing to search for and nothing to activate, so
        // the meta-tools are not registered at all.
        CHECK(contains(names, "mcp__executor__search"));
        CHECK_FALSE(contains(names, "mcp_search"));
        CHECK_FALSE(contains(names, "mcp_activate"));
    }

    SECTION("a connected lazy Upstream registers both meta-tools") {
        ActivationFixture fixture;
        fixture.write_settings(kLazyServerSettings);
        fixture.write_trust_store(kTrustedStore);
        answer_upstream(*fixture.transport, {upstream_tool_entry("search", "search the corpus", "query")});

        auto owned = fixture.create();
        auto& session = fixture.runtime.adopt_session(std::move(owned));
        REQUIRE(ActivationFixture::wait_until(
                [&fixture] { return fixture.transport->request_count("tools/list") == 1; }));
        REQUIRE(fixture.prompt(session, "hi").has_value());

        const auto active = session.snapshot().agent_state.active_tool_names;
        CHECK(contains(active, "mcp_search"));
        CHECK(contains(active, "mcp_activate"));
        // The meta-tools carry their own schemas into the request, and the
        // Upstream tool still carries none.
        REQUIRE_FALSE(fixture.client->requests.empty());
        const auto& last = fixture.client->requests.back();
        CHECK_FALSE(schema_in_request(last, "mcp_search").empty());
        CHECK_FALSE(schema_in_request(last, "mcp_activate").empty());
        CHECK(schema_in_request(last, "mcp__executor__search").empty());
    }
}

TEST_CASE("a server's own instructions reach the model in a real request", "[mcp][issue847][spec]") {
    ActivationFixture fixture;
    fixture.write_settings(kLazyServerSettings);
    fixture.write_trust_store(kTrustedStore);
    answer_upstream(*fixture.transport,
            {upstream_tool_entry("search", "search the corpus", "query")},
            "Prefer search over raw HTTP; batch your calls.");

    auto owned = fixture.create();
    auto& session = fixture.runtime.adopt_session(std::move(owned));
    REQUIRE(ActivationFixture::wait_until([&fixture] { return fixture.transport->request_count("tools/list") == 1; }));
    REQUIRE(fixture.prompt(session, "hi").has_value());

    // The guidance is a System Prompt section, so it is in every request the
    // session makes once the server has offered it — activated or not.
    REQUIRE_FALSE(fixture.client->requests.empty());
    const auto system_prompt = fixture.client->requests.back().context.system_prompt;
    REQUIRE(system_prompt.has_value());
    CHECK(system_prompt->find("Prefer search over raw HTTP; batch your calls.") != std::string::npos);
    CHECK(system_prompt->find("<mcp_upstreams>") != std::string::npos);
}

TEST_CASE("codemode upstream tools work as ordinary lazy tools", "[mcp][issue847][spec]") {
    // Story 36: an executor-style server's `execute`/`resume`/`skills` are
    // ordinary lazy tools. Activating one is all it takes; no pike-side
    // interpreter exists and none is needed.
    ActivationFixture fixture;
    fixture.write_settings(kLazyServerSettings);
    fixture.write_trust_store(kTrustedStore);
    answer_upstream(*fixture.transport,
            {upstream_tool_entry("execute", "run a codemode cell", "cell"),
                    upstream_tool_entry("resume", "resume a cell", "cell_id"),
                    upstream_tool_entry("skills", "list server skills", "name")},
            "Use execute to run code; resume continues a cell.");
    fixture.transport->answer("tools/call",
            ScriptedMcpAnswer{
                    .result = tests::tool_call_result(support::JsonValue{support::JsonValue::array_t{
                            support::JsonValue{support::JsonValue::object_t{{"type", support::JsonValue("text")},
                                    {"text", support::JsonValue("cell ran")}}}}})});

    auto owned = fixture.create();
    auto& session = fixture.runtime.adopt_session(std::move(owned));
    REQUIRE(ActivationFixture::wait_until([&fixture] { return fixture.transport->request_count("tools/list") == 1; }));

    fixture.client->responses.push_back(
            tool_call_response("call-0", "mcp_activate", R"({"name":"mcp__executor__execute"})"));
    fixture.client->responses.push_back(tool_call_response("call-1", "mcp__executor__execute", R"({"cell":"1+1"})"));
    fixture.client->responses.push_back(ai::assistant_text_message("done"));
    REQUIRE(fixture.prompt(session, "use codemode").has_value());

    // The search result listed all three; only the activated one is callable,
    // and it is the Upstream's own tool the call reached.
    REQUIRE(called_tool_names(*fixture.transport) == std::vector<std::string>{"execute"});
    const auto& last = fixture.client->requests.back();
    CHECK(contains(tool_names_in_request(last), "mcp__executor__execute"));
    CHECK_FALSE(contains(tool_names_in_request(last), "mcp__executor__resume"));
    CHECK_FALSE(contains(tool_names_in_request(last), "mcp__executor__skills"));
    const auto results = tool_results(fixture.client->requests);
    REQUIRE_FALSE(results.empty());
    CHECK(results.back().text.find("cell ran") != std::string::npos);
}
