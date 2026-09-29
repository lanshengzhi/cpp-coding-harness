// Eager Upstream MCP Server tools, end to end (issue #842; spec #833 stories
// 19-21, 23 on the integration side, 25, 36, 37).
//
// The cases cross the one production door — `create_agent_session_async` —
// for everything the product does with a discovered tool, and drive the
// binding value directly where a case is about a value the session assembly
// only ever fills. The injected seams are the MCP Host's own transport
// (`tests::ScriptedMcpTransport`) and the connection timer
// (`tests::ScriptedMcpDelay`); no second seam is introduced.

#include <cch/agent/harness/session/SessionStore.hpp>
#include <cch/coding_agent/AgentConfigDir.hpp>
#include "ai/ModelStreamBridge.hpp"
#include "coding_agent/AgentSession.hpp"
#include <cch/coding_agent/McpToolBinding.hpp>
#include "coding_agent/runtime/AgentSessionCreationRequest.hpp"
#include "coding_agent/runtime/McpToolApprovalPolicy.hpp"
#include "coding_agent/runtime/McpToolBinding.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/AgentRootFixture.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ExpectedMacros.hpp"
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

#include <atomic>
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

/// One `tools/list` tool entry whose properties are required, so a call that
/// omits one is invalid against the Upstream's own schema.
[[nodiscard]] support::JsonValue required_tool_entry(std::string name, std::string property) {
    using JsonValue = support::JsonValue;
    JsonValue::array_t required_values{JsonValue(property)};
    return JsonValue::object_t{
            {"name", JsonValue(std::move(name))},
            {"description", JsonValue("an upstream tool")},
            {"inputSchema",
                    JsonValue::object_t{{"type", JsonValue("object")},
                            {"properties",
                                    JsonValue::object_t{
                                            {property, JsonValue::object_t{{"type", JsonValue("string")}}}}},
                            {"required", required_values}}},
    };
}

/// A conforming Modern Era Upstream that advertises the given catalog.
void answer_upstream(tests::ScriptedMcpTransport& transport, std::vector<support::JsonValue> tools) {
    transport.answer("server/discover", ScriptedMcpAnswer{.result = tests::discover_result()});
    transport.answer("tools/list", ScriptedMcpAnswer{.result = tests::tool_list_result(std::move(tools))});
}

/// Answer `tools/call` with one completed result carrying `content`.
void answer_tool_call(tests::ScriptedMcpTransport& transport, support::JsonValue content) {
    transport.answer("tools/call", ScriptedMcpAnswer{.result = tests::tool_call_result(std::move(content))});
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

/// Consume one `AsyncResult` to its terminal outcome on a local loop. The
/// scripted transport and the scripted timer settle inline, so a driven
/// operation is finished before this returns.
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

/// An `agent::Tool` value driven through one invocation, for the cases that
/// exercise a bound tool's execute operation directly.
[[nodiscard]] support::Expected<agent::AsyncToolExecutionResult> execute_bound(
        agent::Tool tool, agent::ToolInvocation invocation) {
    if (!tool.execute) {
        return std::unexpected(support::make_error(support::ErrorCode::Tool, "the bound tool has no execute"));
    }
    auto execute = std::move(tool.execute);
    return drive_locally(execute(std::move(invocation), std::stop_token{}, agent::ToolUpdateSink{}));
}

/// One connecting Upstream with its own scripted transport, so a case can tell
/// which server a bound call actually reached.
[[nodiscard]] std::shared_ptr<mcp::UpstreamConnection> connecting_upstream(
        std::string server_id, const std::shared_ptr<tests::ScriptedMcpTransport>& transport) {
    transport->answer("server/discover", ScriptedMcpAnswer{.result = tests::discover_result()});
    transport->answer("tools/list", ScriptedMcpAnswer{.result = tests::tool_list_result({})});
    answer_tool_call(*transport, support::JsonValue{support::JsonValue::array_t{}});
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

[[nodiscard]] std::vector<std::string> tool_names_in_request(const tests::RecordedProviderRequest& request) {
    std::vector<std::string> names;
    names.reserve(request.context.tools.size());
    for (const auto& tool : request.context.tools) {
        names.push_back(tool.name);
    }
    return names;
}

[[nodiscard]] bool contains(const std::vector<std::string>& values, std::string_view value) {
    return std::ranges::find(values, value) != values.end();
}

/// A scripted chat client that serves queued assistant messages in order and
/// records every request, so a case can read exactly which tools the model was
/// offered on which request.
class PublicationProvider final : public tests::ScriptedProvider {
public:
    PublicationProvider() : ScriptedProvider("sdk-host") {}

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext context, coding_agent::ModelRuntimeTestStreamOptions options) override {
        return ai::detail::make_model_stream(
                [this, model = std::move(model), context = std::move(context), options = std::move(options)](
                        ai::AssistantEventSink sink)
                        -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
                    requests.push_back(tests::RecordedProviderRequest{model, context, options});
                    if (on_request) {
                        // Runs on the session's Runtime domain at the top of a
                        // turn, which is how a case releases a withheld catalog
                        // *inside* a run: after the pre-prompt drain and before the
                        // between-turn one.
                        on_request(static_cast<int>(requests.size()) - 1);
                    }
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

    [[nodiscard]] const std::vector<tests::RecordedProviderRequest>& recorded_requests() const { return requests; }

    std::vector<ai::AssistantMessage> responses;
    std::vector<tests::RecordedProviderRequest> requests;
    std::function<void(int request_index)> on_request;
};

class PublicationProvider;

/// The tool result the model saw, in tool-call order, beside its error flag.
struct SeenToolResult {
    std::string text{};
    bool is_error{false};
};

[[nodiscard]] std::vector<SeenToolResult> tool_results(const PublicationProvider& client) {
    std::vector<SeenToolResult> results;
    for (const auto& request : client.recorded_requests()) {
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

/// A session assembled against a temp Agent Config Directory, with the MCP
/// Host's transport and timer injected and no ambient provider key resolving
/// as configured. The fixture's loop is driven by `RuntimeLoopDriver`, because
/// the MCP Host's connections run on the session's Runtime target — the same
/// loop a prompt runs on.
struct PublicationFixture {
    tests::TempWorkspace workspace;
    std::filesystem::path agent_dir;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard kimi_guard{"KIMI_API_KEY"};
    tests::EnvVarGuard deepseek_guard{"DEEPSEEK_API_KEY"};
    tests::RuntimeFixture runtime;
    tests::RuntimeLoopDriver driver;
    std::shared_ptr<tests::ScriptedMcpTransport> transport{std::make_shared<tests::ScriptedMcpTransport>()};
    tests::ScriptedMcpDelay delay;
    std::shared_ptr<PublicationProvider> client{std::make_shared<PublicationProvider>()};

    PublicationFixture() : driver(runtime) {
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

    /// The same assembly against an explicit target, so a case can open, close,
    /// and resume one persisted session.
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

    /// Run one prompt on a local loop, so the fixture's driver keeps servicing
    /// the Runtime loop the whole run (and every MCP Host operation) needs.
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

constexpr std::string_view kEagerServerSettings =
        R"({"mcpServers": {"executor": {"url": "https://mcp.example/mcp", "activation": "eager"}}})";
constexpr std::string_view kTrustedStore = R"({"executor": true, "other": true})";

} // namespace

TEST_CASE("a Qualified Tool Name namespaces the Server Id and the upstream tool name", "[mcp][issue842][spec]") {
    CHECK(coding_agent::mcp_qualified_tool_name("executor", "search") == "mcp__executor__search");
    // Sanitizing is lossy and deliberate: a tool name is a third party's
    // string and the registered name has to be safe for every provider's
    // tool-name grammar.
    CHECK(coding_agent::mcp_qualified_tool_name("executor", "search/files v2") == "mcp__executor__search_files_v2");
    CHECK(coding_agent::mcp_qualified_tool_name("srv-1", "a-b_c") == "mcp__srv-1__a-b_c");
}

TEST_CASE("an over-long Upstream tool name is truncated with a deterministic hash suffix", "[mcp][issue842][spec]") {
    const std::string long_name(200, 'x');
    const auto qualified = coding_agent::mcp_qualified_tool_name("executor", long_name);
    CHECK(qualified.size() == coding_agent::kMcpQualifiedToolNameMaxLength);
    CHECK(qualified.starts_with("mcp__executor__xxx"));
    // Deterministic across calls, so a re-discovered tool keeps its name and a
    // reconnect does not rename a live tool.
    CHECK(coding_agent::mcp_qualified_tool_name("executor", long_name) == qualified);
    // And distinct: a truncation that collided would silently retarget a tool.
    const std::string other(200, 'y');
    CHECK(coding_agent::mcp_qualified_tool_name("executor", other) != qualified);
}

TEST_CASE("the tool binding keeps one reverse-mapping row per published tool", "[mcp][issue842][spec]") {
    runtime_ns::McpToolBinding binding;
    binding.set_servers(
            {coding_agent::UserMcpServerSettings{.server_id = "executor", .url = "https://mcp.example/mcp"}});
    const auto connection = std::make_shared<mcp::UpstreamConnection>("executor",
            std::make_shared<tests::ScriptedMcpTransport>(),
            mcp::UpstreamConnectionOptions{
                    .url = "https://mcp.example/mcp",
                    .delay = [](std::chrono::milliseconds,
                                     std::stop_token) { return support::AsyncResult<void>(support::Expected<void>{}); },
            });

    REQUIRE(binding.publish(connection, mcp::UpstreamToolDescriptor{.name = "search"}).has_value());
    const auto rows = binding.published();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().qualified_name == "mcp__executor__search");
    CHECK(rows.front().server_id == "executor");
    CHECK(rows.front().tool_name == "search");
    // The reverse mapping is what the approval policy and the display surface
    // read; a built-in name has no row and is therefore never an MCP call.
    CHECK(binding.server_id_for("mcp__executor__search") == std::optional<std::string>{"executor"});
    CHECK(!binding.server_id_for("read").has_value());
    CHECK(binding.approval_for("executor") == coding_agent::McpServerApproval::Allow);
    CHECK(binding.approval_for("never-configured") == coding_agent::McpServerApproval::Allow);

    // Republishing the same tool replaces the staged value instead of stacking
    // a second one: a reconnect must not produce a second `toolsAdded` entry.
    REQUIRE(binding.publish(connection, mcp::UpstreamToolDescriptor{.name = "search"}).has_value());
    CHECK(binding.published().size() == 1);
    CHECK(binding.take_pending().size() == 1);
    CHECK(binding.take_pending().empty());
}

TEST_CASE("a closed tool binding refuses a late publication and drains nothing", "[mcp][issue842][spec]") {
    runtime_ns::McpToolBinding binding;
    const auto connection = std::make_shared<mcp::UpstreamConnection>("executor",
            std::make_shared<tests::ScriptedMcpTransport>(),
            mcp::UpstreamConnectionOptions{
                    .url = "https://mcp.example/mcp",
                    .delay = [](std::chrono::milliseconds,
                                     std::stop_token) { return support::AsyncResult<void>(support::Expected<void>{}); },
            });
    binding.close();
    CHECK(binding.closed());
    const auto published = binding.publish(connection, mcp::UpstreamToolDescriptor{.name = "search"});
    REQUIRE(!published.has_value());
    CHECK(published.error().code == support::ErrorCode::Cancelled);
    CHECK(binding.take_pending().empty());
    CHECK(binding.published().empty());
}

TEST_CASE("the call-approval policy allows everything that is not an ask call", "[mcp][issue842][spec]") {
    auto binding = std::make_shared<runtime_ns::McpToolBinding>();
    binding->set_servers({
            coding_agent::UserMcpServerSettings{.server_id = "executor",
                    .url = "https://mcp.example/mcp",
                    .approval = coding_agent::McpServerApproval::Ask},
            coding_agent::UserMcpServerSettings{.server_id = "open",
                    .url = "https://mcp.example/mcp",
                    .approval = coding_agent::McpServerApproval::Allow},
    });
    const auto connection = std::make_shared<mcp::UpstreamConnection>("executor",
            std::make_shared<tests::ScriptedMcpTransport>(),
            mcp::UpstreamConnectionOptions{
                    .url = "https://mcp.example/mcp",
                    .delay = [](std::chrono::milliseconds,
                                     std::stop_token) { return support::AsyncResult<void>(support::Expected<void>{}); },
            });
    REQUIRE(binding->publish(connection, mcp::UpstreamToolDescriptor{.name = "search"}).has_value());

    auto hook = runtime_ns::McpToolApprovalPolicy{binding}.make_hook();
    const auto decide = [&hook](std::string name) {
        agent::BeforeToolCallContext context;
        context.tool_call = ai::tool_call_content("call-1", std::move(name), "{}");
        const auto outcome = drive_locally(hook(std::move(context), std::stop_token{}));
        REQUIRE(outcome.has_value());
        return *outcome;
    };

    // A built-in tool name and an `allow` server are both untouched: the
    // built-in set must not change behavior because a third-party namespace
    // exists.
    CHECK_FALSE(decide("read").block);
    CHECK_FALSE(decide("mcp__open__search").block);
    // An `ask` call is fail-closed: refused with a reason, never silently
    // treated as `allow` while the prompt ticket has not landed.
    const auto refused = decide("mcp__executor__search");
    CHECK(refused.block);
    REQUIRE(refused.reason.has_value());
    CHECK(refused.reason->find("mcp__executor__search") != std::string::npos);
}

TEST_CASE("a withheld catalog leaves the session usable and the next model request can call the tool",
        "[mcp][issue842][spec]") {
    PublicationFixture fixture;
    fixture.write_settings(kEagerServerSettings);
    fixture.write_trust_store(kTrustedStore);
    // The Upstream takes the era probe and then withholds its catalog.
    fixture.transport->answer("server/discover", ScriptedMcpAnswer{.result = tests::discover_result()});
    fixture.transport->hold("tools/list");

    auto owned = fixture.create();
    auto& session = fixture.runtime.adopt_session(std::move(owned));
    REQUIRE(session.is_open());
    REQUIRE(PublicationFixture::wait_until([&fixture] { return fixture.transport->request_count("tools/list") == 1; }));

    // The session is usable with the catalog still outstanding, and no tool
    // exists yet.
    CHECK(session.mcp_published_tools().empty());
    fixture.client->responses.push_back(ai::assistant_text_message("hello"));
    REQUIRE(fixture.prompt(session, "hi").has_value());

    // The withheld catalog answers now: the tool is published, and the *next*
    // model request is the one that carries it.
    fixture.transport->answer("tools/list",
            ScriptedMcpAnswer{.result = tests::tool_list_result({tests::tool_entry("search", {"query"})})});
    fixture.transport->release("tools/list");
    answer_tool_call(*fixture.transport,
            support::JsonValue{support::JsonValue::array_t{support::JsonValue{support::JsonValue::object_t{
                    {"type", support::JsonValue("text")}, {"text", support::JsonValue("found it")}}}}});
    fixture.client->responses.push_back(tool_call_response("call-1", "mcp__executor__search", R"({"query":"x"})"));
    fixture.client->responses.push_back(ai::assistant_text_message("done"));
    REQUIRE(PublicationFixture::wait_until([&session] { return session.mcp_published_tools().size() == 1; }));

    REQUIRE(fixture.prompt(session, "search please").has_value());

    const auto rows = session.mcp_published_tools();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().qualified_name == "mcp__executor__search");
    CHECK(rows.front().server_id == "executor");
    CHECK(rows.front().tool_name == "search");

    // Request 0 ran while the catalog was withheld; request 1 is the one that
    // could call the tool; request 2 is the follow-up turn after the call.
    const auto& requests = fixture.client->recorded_requests();
    REQUIRE(requests.size() == 3);
    CHECK_FALSE(contains(tool_names_in_request(requests[0]), "mcp__executor__search"));
    CHECK(contains(tool_names_in_request(requests[1]), "mcp__executor__search"));
    CHECK(contains(tool_names_in_request(requests[2]), "mcp__executor__search"));

    REQUIRE(called_tool_names(*fixture.transport) == std::vector<std::string>{"search"});
    const auto results = tool_results(*fixture.client);
    REQUIRE_FALSE(results.empty());
    CHECK_FALSE(results.back().is_error);
    CHECK(results.back().text.find("found it") != std::string::npos);
}

TEST_CASE(
        "late discovery after session close registers nothing and resurrects no connection", "[mcp][issue842][spec]") {
    PublicationFixture fixture;
    fixture.write_settings(kEagerServerSettings);
    fixture.write_trust_store(kTrustedStore);
    fixture.transport->answer("server/discover", ScriptedMcpAnswer{.result = tests::discover_result()});
    fixture.transport->hold("tools/list");

    auto owned = fixture.create();
    auto& session = fixture.runtime.adopt_session(std::move(owned));
    REQUIRE(PublicationFixture::wait_until([&fixture] { return fixture.transport->request_count("tools/list") == 1; }));

    session.close();
    const auto requests_at_close = fixture.transport->request_count();

    // The catalog answers after the session is gone. Nothing is published, no
    // tool is registered, and the closed host knocks no further.
    fixture.transport->answer("tools/list",
            ScriptedMcpAnswer{.result = tests::tool_list_result({tests::tool_entry("search", {"query"})})});
    fixture.transport->release("tools/list");
    (void)PublicationFixture::wait_until(
            [&fixture, requests_at_close] { return fixture.transport->request_count() > requests_at_close; });
    fixture.transport->release("tools/list");
    std::this_thread::sleep_for(std::chrono::milliseconds{20});

    CHECK(fixture.transport->request_count("tools/call") == 0);
    CHECK(fixture.transport->request_count("server/discover") == 1);
}

TEST_CASE("invalid arguments make no upstream request", "[mcp][issue842][spec]") {
    PublicationFixture fixture;
    fixture.write_settings(kEagerServerSettings);
    fixture.write_trust_store(kTrustedStore);
    answer_upstream(*fixture.transport, {required_tool_entry("search", "query")});
    answer_tool_call(*fixture.transport, support::JsonValue{support::JsonValue::array_t{}});

    auto owned = fixture.create();
    auto& session = fixture.runtime.adopt_session(std::move(owned));
    REQUIRE(PublicationFixture::wait_until([&session] { return session.mcp_published_tools().size() == 1; }));

    // `query` is required, so a call that omits it is invalid against the
    // Upstream's own schema and ADR 0007's validation rejects it before the
    // request exists.
    fixture.client->responses.push_back(tool_call_response("call-1", "mcp__executor__search", "{}"));
    fixture.client->responses.push_back(ai::assistant_text_message("done"));
    REQUIRE(fixture.prompt(session, "search please").has_value());

    CHECK(fixture.transport->request_count("tools/call") == 0);
    const auto results = tool_results(*fixture.client);
    REQUIRE_FALSE(results.empty());
    CHECK(results.back().is_error);
}

TEST_CASE(
        "same-named tools on two servers keep their own schema, target, and display target", "[mcp][issue842][spec]") {
    // Two Upstreams, one tool name, two different argument contracts. This is
    // the case a name-only binding gets wrong: whichever schema survived last
    // would validate both calls, and whichever connection survived last would
    // receive both calls.
    auto alpha_transport = std::make_shared<tests::ScriptedMcpTransport>();
    auto beta_transport = std::make_shared<tests::ScriptedMcpTransport>();
    auto alpha = connecting_upstream("alpha", alpha_transport);
    auto beta = connecting_upstream("beta", beta_transport);

    auto binding = std::make_shared<runtime_ns::McpToolBinding>();
    // `alpha`'s tool takes a string; `beta`'s takes an integer. One name, two
    // schemas.
    const support::JsonValue alpha_schema{support::JsonValue::object_t{
            {"type", support::JsonValue("object")},
            {"properties",
                    support::JsonValue{support::JsonValue::object_t{{"query",
                            support::JsonValue{
                                    support::JsonValue::object_t{{"type", support::JsonValue("string")}}}}}}},
    }};
    const support::JsonValue beta_schema{support::JsonValue::object_t{
            {"type", support::JsonValue("object")},
            {"properties",
                    support::JsonValue{support::JsonValue::object_t{{"limit",
                            support::JsonValue{
                                    support::JsonValue::object_t{{"type", support::JsonValue("integer")}}}}}}},
    }};
    REQUIRE(binding->publish(alpha,
                           mcp::UpstreamToolDescriptor{
                                   .name = "search",
                                   .description = "alpha's search",
                                   .parameters = alpha_schema,
                           })
                    .has_value());
    REQUIRE(binding->publish(beta,
                           mcp::UpstreamToolDescriptor{
                                   .name = "search",
                                   .description = "beta's search",
                                   .parameters = beta_schema,
                           })
                    .has_value());

    // Display target: two distinct names, each mapping back to its own Server
    // Id and its own description.
    const auto rows = binding->published();
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].qualified_name == "mcp__alpha__search");
    CHECK(rows[0].server_id == "alpha");
    CHECK(rows[0].description == "alpha's search");
    CHECK(rows[1].qualified_name == "mcp__beta__search");
    CHECK(rows[1].server_id == "beta");
    CHECK(binding->server_id_for("mcp__alpha__search") == std::optional<std::string>{"alpha"});
    CHECK(binding->server_id_for("mcp__beta__search") == std::optional<std::string>{"beta"});

    // Schema target and execution target: each bound tool carries its own
    // server's schema and reaches only its own connection.
    auto staged = binding->take_pending();
    REQUIRE(staged.size() == 2);
    const auto alpha_result = execute_bound(std::move(staged[0].binding),
            agent::ToolInvocation{
                    .call_id = "call-1",
                    .name = "mcp__alpha__search",
                    .arguments = *support::read_json(R"({"query":"x"})"),
                    .raw_arguments = R"({"query":"x"})",
            });
    REQUIRE(alpha_result.has_value());
    CHECK_FALSE(alpha_result->is_error);
    CHECK(alpha_transport->request_count("tools/call") == 1);
    CHECK(beta_transport->request_count("tools/call") == 0);
    CHECK(called_tool_names(*alpha_transport) == std::vector<std::string>{"search"});

    const auto beta_result = execute_bound(std::move(staged[1].binding),
            agent::ToolInvocation{
                    .call_id = "call-2",
                    .name = "mcp__beta__search",
                    .arguments = *support::read_json(R"({"limit":2})"),
                    .raw_arguments = R"({"limit":2})",
            });
    REQUIRE(beta_result.has_value());
    CHECK_FALSE(beta_result->is_error);
    CHECK(beta_transport->request_count("tools/call") == 1);
    CHECK(alpha_transport->request_count("tools/call") == 1);
}

TEST_CASE("a flooding tool result is bounded and redacted before it is truncated", "[mcp][issue842][spec]") {
    PublicationFixture fixture;
    fixture.write_settings(kEagerServerSettings);
    fixture.write_trust_store(kTrustedStore);
    answer_upstream(*fixture.transport, {tests::tool_entry("search", {"query"})});
    // A hostile Upstream: a megabyte of content with a secret-shaped key near
    // the end, so a truncate-then-redact order would leave the secret visible.
    std::string flood(1024 * 1024, 'z');
    flood += R"("api_key": "sk-live-must-never-survive")";
    answer_tool_call(*fixture.transport,
            support::JsonValue{support::JsonValue::array_t{support::JsonValue{support::JsonValue::object_t{
                    {"type", support::JsonValue("text")}, {"text", support::JsonValue(std::move(flood))}}}}});

    auto owned = fixture.create();
    auto& session = fixture.runtime.adopt_session(std::move(owned));
    REQUIRE(PublicationFixture::wait_until([&session] { return session.mcp_published_tools().size() == 1; }));

    fixture.client->responses.push_back(tool_call_response("call-1", "mcp__executor__search", R"({"query":"x"})"));
    fixture.client->responses.push_back(ai::assistant_text_message("done"));
    REQUIRE(fixture.prompt(session, "search please").has_value());

    REQUIRE(called_tool_names(*fixture.transport) == std::vector<std::string>{"search"});
    const auto results = tool_results(*fixture.client);
    REQUIRE_FALSE(results.empty());
    CHECK_FALSE(results.back().is_error);
    // The product's ordinary tool-output bound applies: an MCP result is a tool
    // result, so a flooding Upstream is contained by the same limit `read`
    // and `bash` already use.
    CHECK(results.back().text.size() <= 50 * 1024);
    CHECK(results.back().text.find("sk-live-must-never-survive") == std::string::npos);
}

TEST_CASE("an approval: ask call is refused rather than run until the prompt lands", "[mcp][issue842][spec]") {
    PublicationFixture fixture;
    fixture.write_settings(
            R"({"mcpServers": {"executor": {"url": "https://mcp.example/mcp", "activation": "eager", "approval": "ask"}}})");
    fixture.write_trust_store(kTrustedStore);
    answer_upstream(*fixture.transport, {tests::tool_entry("search", {"query"})});
    answer_tool_call(*fixture.transport, support::JsonValue{support::JsonValue::array_t{}});

    auto owned = fixture.create();
    auto& session = fixture.runtime.adopt_session(std::move(owned));
    REQUIRE(PublicationFixture::wait_until([&session] { return session.mcp_published_tools().size() == 1; }));

    fixture.client->responses.push_back(tool_call_response("call-1", "mcp__executor__search", R"({"query":"x"})"));
    fixture.client->responses.push_back(ai::assistant_text_message("done"));
    REQUIRE(fixture.prompt(session, "search please").has_value());

    // Fail-closed, and one failed call — not a session failure.
    CHECK(fixture.transport->request_count("tools/call") == 0);
    const auto results = tool_results(*fixture.client);
    REQUIRE_FALSE(results.empty());
    CHECK(results.back().is_error);
    CHECK(results.back().text.find("mcp__executor__search") != std::string::npos);
}

TEST_CASE("a lazy server publishes no tool", "[mcp][issue842][spec]") {
    PublicationFixture fixture;
    // No `activation` field: the default is `lazy`, whose interim semantics are
    // that a lazy server's tools stay dormant.
    fixture.write_settings(R"({"mcpServers": {"executor": {"url": "https://mcp.example/mcp"}}})");
    fixture.write_trust_store(kTrustedStore);
    answer_upstream(*fixture.transport, {tests::tool_entry("search", {"query"})});

    auto owned = fixture.create();
    auto& session = fixture.runtime.adopt_session(std::move(owned));
    // Give the host the same window every other case gives it, so this is a
    // real "the eager path did not run" and not "the eager path was too slow".
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    // A lazy server is never silently treated as eager, and its catalog is not
    // even walked until Lazy Tool Activation lands.
    CHECK(session.mcp_published_tools().empty());
    CHECK(fixture.transport->request_count("tools/list") == 0);
}

TEST_CASE("a tool catalog that lands between two turns reaches the next request in the same run",
        "[mcp][issue842][spec]") {
    PublicationFixture fixture;
    fixture.write_settings(kEagerServerSettings);
    fixture.write_trust_store(kTrustedStore);
    fixture.transport->answer("server/discover", ScriptedMcpAnswer{.result = tests::discover_result()});
    // The catalog is withheld, so the pre-prompt drain has nothing to register.
    fixture.transport->hold("tools/list");

    // The first model request releases the catalog. The prompt's pre-prompt
    // drain has already run by then, so the tool can only reach the tool
    // surface through the between-turn drain inside the same run.
    auto* raw_transport = fixture.transport.get();
    fixture.client->on_request = [raw_transport](int index) {
        if (index == 0) {
            raw_transport->answer("tools/list",
                    ScriptedMcpAnswer{.result = tests::tool_list_result({required_tool_entry("search", "query")})});
            raw_transport->release("tools/list");
        }
    };
    answer_tool_call(*fixture.transport, support::JsonValue{support::JsonValue::array_t{}});
    // A built-in call is what makes the run continue into a second turn. It
    // also pins the approval hook's fail-open default end to end: the hook is
    // installed for every call and must leave `read` alone.
    const auto notes = fixture.workspace.path() / "notes.txt";
    {
        std::ofstream output(notes, std::ios::binary | std::ios::trunc);
        output << "hello";
    }
    const std::string read_arguments = std::string{"{\"path\":\""} + notes.string() + "\"}";
    fixture.client->responses.push_back(tool_call_response("call-0", "read", read_arguments));
    fixture.client->responses.push_back(ai::assistant_text_message("done"));

    auto owned = fixture.create();
    auto& session = fixture.runtime.adopt_session(std::move(owned));
    REQUIRE(PublicationFixture::wait_until([&fixture] { return fixture.transport->request_count("tools/list") == 1; }));
    CHECK(session.mcp_published_tools().empty());

    REQUIRE(fixture.prompt(session, "hi").has_value());
    REQUIRE(session.mcp_published_tools().size() == 1);
    // The follow-up turn is the request that carries it: the system message the
    // between-turn drain appended is the one the model sees, and the
    // registry read at the top of that turn already held the tool.
    const auto& requests = fixture.client->recorded_requests();
    REQUIRE(requests.size() == 2);
    CHECK_FALSE(contains(tool_names_in_request(requests[0]), "mcp__executor__search"));
    CHECK(contains(tool_names_in_request(requests[1]), "mcp__executor__search"));
}

TEST_CASE("a resumed session re-binds the callable object, not just the name", "[mcp][issue842][spec]") {
    PublicationFixture fixture;
    fixture.write_settings(kEagerServerSettings);
    fixture.write_trust_store(kTrustedStore);
    answer_upstream(*fixture.transport, {required_tool_entry("search", "query")});
    answer_tool_call(*fixture.transport, support::JsonValue{support::JsonValue::array_t{}});
    const auto session_file = fixture.workspace.path() / "resume-session.jsonl";

    {
        auto owned = fixture.create_with(coding_agent::ExplicitOpenOrCreateSessionTarget{session_file});
        auto& session = fixture.runtime.adopt_session(std::move(owned));
        REQUIRE(PublicationFixture::wait_until([&session] { return session.mcp_published_tools().size() == 1; }));
        // The prompt's pre-prompt drain is what persists the `toolsAdded`
        // system message the resumed transcript will replay.
        fixture.client->responses.push_back(ai::assistant_text_message("hello"));
        REQUIRE(fixture.prompt(session, "hi").has_value());
        session.close();
    }

    auto recorded = harness::session::SessionStore::open_existing(session_file);
    REQUIRE(recorded.has_value());
    bool recorded_the_tool = false;
    for (const auto& entry : recorded->entries()) {
        if (entry.kind != harness::session::SessionEntryKind::Message || !entry.message) {
            continue;
        }
        const auto* system = std::get_if<ai::SystemMessage>(&*entry.message);
        if (system == nullptr) {
            continue;
        }
        recorded_the_tool = std::ranges::any_of(
                system->tools_added, [](const ai::Tool& tool) { return tool.name == "mcp__executor__search"; });
    }
    REQUIRE(recorded_the_tool);

    // On resume the transcript names the tool, but the registry does not hold
    // it until its catalog is discovered again. Re-binding is what makes the
    // recorded name callable again.
    auto resumed = fixture.create_with(coding_agent::ExplicitResumeSessionTarget{session_file});
    auto& session = fixture.runtime.adopt_session(std::move(resumed));
    REQUIRE(PublicationFixture::wait_until([&session] { return session.mcp_published_tools().size() == 1; }));
    // The resumed transcript named the tool, but the registry never held it, so
    // the replay narrowed the active set back to the built-ins. Re-binding is
    // the next line's work, not the replay's.
    CHECK(session.snapshot().agent_state.active_tool_names ==
            std::vector<std::string>{"bash", "edit", "read", "write"});

    fixture.client->responses.push_back(tool_call_response("call-1", "mcp__executor__search", R"({"query":"x"})"));
    fixture.client->responses.push_back(ai::assistant_text_message("done"));
    REQUIRE(fixture.prompt(session, "search please").has_value());
    CHECK(session.snapshot().agent_state.active_tool_names ==
            std::vector<std::string>{"bash", "edit", "mcp__executor__search", "read", "write"});
    REQUIRE(called_tool_names(*fixture.transport) == std::vector<std::string>{"search"});
}
