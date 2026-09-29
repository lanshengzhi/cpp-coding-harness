// A Pending Elicitation through the whole session (issue #845; spec #833
// stories 26-29, 32).
//
// Every session-level case crosses the one production door —
// `create_agent_session_async` — so the chain under test is the real one: the
// MCP Host suspends the Upstream tool call, the session publishes the
// question as a `cch_coding_agent` projection, the frontend answers it, and
// the retried request carries the continuation. The frontend is the session's
// own API here; the Native TUI dialog is exercised in
// `tests/coding_agent/tui/McpElicitationDialogTest.cpp`.
//
// The injected seams are the MCP Host's own transport
// (`tests::ScriptedMcpTransport`) and its timers
// (`tests::ScriptedMcpDelay`); no second seam is introduced.

#include <cch/agent/harness/session/SessionStore.hpp>
#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/coding_agent/McpElicitation.hpp>
#include <cch/coding_agent/McpToolApproval.hpp>
#include "ai/ModelStreamBridge.hpp"
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/AgentSessionCreationRequest.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/AgentRootFixture.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/ModelsFixture.hpp"
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
#include <vector>

using namespace cch;
using coding_agent::McpElicitationAction;
using coding_agent::McpElicitationMode;
using coding_agent::McpToolApprovalAnswer;
using coding_agent::McpToolApprovalPrompter;
using coding_agent::McpToolApprovalRequest;
using tests::ScriptedMcpAnswer;

namespace {

namespace runtime_ns = cch::coding_agent::runtime;

/// Every wait in this file is a cap, not a wait: a passing case returns as
/// soon as its condition holds.
constexpr std::chrono::milliseconds kBudget{5000};

/// One `tools/list` tool entry the model may call.
[[nodiscard]] support::JsonValue approvable_tool() {
    using JsonValue = support::JsonValue;
    return JsonValue::object_t{
            {"name", JsonValue("approve")},
            {"description", JsonValue("start an approval flow")},
            {"inputSchema", JsonValue::object_t{{"type", JsonValue("object")}, {"properties", JsonValue::object_t{}}}},
    };
}

/// A `tools/call` result that suspends the call for one URL-mode question.
/// The continuation token is a value-tree string, which is what a
/// well-behaved server sends; the byte-for-byte claim is proven over the wire
/// in `tests/mcp/MrtrElicitationTest.cpp`.
[[nodiscard]] support::JsonValue url_elicitation() {
    using JsonValue = support::JsonValue;
    return JsonValue::object_t{
            {"resultType", JsonValue("input_required")},
            {"requestState", JsonValue::object_t{{"token", JsonValue("suspension-1")}}},
            {"inputRequests",
                    JsonValue::array_t{JsonValue::object_t{
                            {"id", JsonValue("r1")},
                            {"type", JsonValue("url")},
                            {"message", JsonValue("Approve this action")},
                            {"url", JsonValue("https://executor.invalid/mcp/approve/abc")},
                    }}},
    };
}

/// One scripted chat client that serves queued assistant messages in order.
class ElicitationProvider final : public tests::ScriptedProvider {
public:
    ElicitationProvider() : ScriptedProvider("sdk-host") {}

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

/// One assistant message whose only content is a single tool call.
[[nodiscard]] ai::AssistantMessage tool_call_response(
        std::string call_id, std::string tool_name, std::string raw_arguments) {
    auto message = ai::assistant_text_message("");
    message.content.clear();
    message.content.emplace_back(
            ai::tool_call_content(std::move(call_id), std::move(tool_name), std::move(raw_arguments)));
    return message;
}

/// The tool result the model saw, in tool-call order, beside its error flag.
struct SeenToolResult {
    std::string text{};
    bool is_error{false};
};

[[nodiscard]] std::vector<SeenToolResult> tool_results(const ElicitationProvider& client) {
    std::vector<SeenToolResult> results;
    for (const auto& request : client.requests) {
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

/// The call-approval prompt an `approval: "ask"` session is assembled with.
/// The seam #843 owns, counted here so a case can prove the Multi Round-Trip
/// loop never re-enters it.
class CountingApprovalPrompter final {
public:
    std::shared_ptr<std::atomic<std::size_t>> count{std::make_shared<std::atomic<std::size_t>>(0)};

    [[nodiscard]] McpToolApprovalPrompter prompter() const {
        auto count = this->count;
        return [count](McpToolApprovalRequest, std::stop_token) {
            count->fetch_add(1);
            return support::AsyncResult<McpToolApprovalAnswer>{
                    std::expected<McpToolApprovalAnswer, support::Error>{McpToolApprovalAnswer::Allowed}};
        };
    }
};

/// A session assembled against a temp Agent Config Directory, with the MCP
/// Host's transport and both of its timers injected.
struct ElicitationFixture {
    tests::TempWorkspace workspace;
    std::filesystem::path agent_dir;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard kimi_guard{"KIMI_API_KEY"};
    tests::EnvVarGuard deepseek_guard{"DEEPSEEK_API_KEY"};
    tests::RuntimeFixture runtime;
    tests::RuntimeLoopDriver driver;
    std::shared_ptr<tests::ScriptedMcpTransport> transport{std::make_shared<tests::ScriptedMcpTransport>()};
    tests::ScriptedMcpDelay connection_delay;
    tests::ScriptedMcpDelay elicitation_delay;
    std::shared_ptr<ElicitationProvider> client{std::make_shared<ElicitationProvider>()};
    CountingApprovalPrompter approval;

    ElicitationFixture() : driver(runtime) {
        home_guard.set(workspace.path().string());
        agent_dir = tests::agent_root_under_home(workspace.path());
        std::filesystem::create_directories(agent_dir);
        kimi_guard.unset();
        deepseek_guard.unset();
        transport->answer("server/discover", ScriptedMcpAnswer{.result = tests::discover_result()});
    }

    void write_settings(std::string_view content) const {
        std::ofstream output(agent_dir / "settings.json", std::ios::binary | std::ios::trunc);
        output << content;
    }

    void write_trust_store(std::string_view content) const {
        std::ofstream output(agent_dir / "mcp-trust.json", std::ios::binary | std::ios::trunc);
        output << content;
    }

    /// Script `tools/call` from a handler that counts rounds, so a case can
    /// suspend once and then complete.
    void answer_calls(std::function<ScriptedMcpAnswer(std::size_t round)> handler) {
        transport->answer_with("tools/call",
                [handler = std::move(handler), round = std::size_t{0}](
                        const support::JsonValue&) mutable { return handler(round++); });
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
            return connection_delay.request(wait, stop_token);
        };
        request.mcp_elicitation_delay = [this](std::chrono::milliseconds wait, std::stop_token stop_token) {
            return elicitation_delay.request(wait, stop_token);
        };
        // The session's own call-approval prompt, filled whether or not the
        // configured server asks for consent: a case that must prove the
        // prompt is *not* consulted needs the seam to be present to be absent.
        request.mcp_tool_approval_prompter = approval.prompter();
        auto models = tests::models_from_provider(client);
        auto created = runtime.run(coding_agent::create_agent_session_async(
                std::move(request), std::nullopt, tests::cli_fake_overrides(std::move(models))));
        REQUIRE(created.has_value());
        return std::move(created->session);
    }

    [[nodiscard]] static std::expected<void, support::Error> prompt_once(
            coding_agent::AgentSession& session, std::string text) {
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

    [[nodiscard]] std::expected<void, support::Error> prompt(coding_agent::AgentSession& session, std::string text) {
        return prompt_once(session, std::move(text));
    }

    /// Start one prompt on its own thread and hand back the handle that
    /// settles it. A run that a Pending Elicitation suspends cannot be awaited
    /// synchronously: the answer the test is about to give is exactly what
    /// releases the run, so the run has to be in flight while the test
    /// answers.
    [[nodiscard]] std::thread start_prompt(coding_agent::AgentSession& session, std::string text) {
        return std::thread([&session, text = std::move(text)]() { (void)prompt_once(session, text); });
    }

    template <typename Condition> [[nodiscard]] static bool wait_until(Condition condition) {
        const auto deadline = std::chrono::steady_clock::now() + kBudget;
        while (!condition() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        return condition();
    }

    /// The `tools/call` request bodies, in order.
    [[nodiscard]] std::vector<std::string> call_bodies() const {
        std::vector<std::string> bodies;
        for (std::size_t index = 0; index < transport->request_count(); ++index) {
            const auto method = transport->recorded_method(index);
            REQUIRE(method.has_value());
            if (*method == "tools/call") {
                bodies.push_back(transport->requests().at(index).body);
            }
        }
        return bodies;
    }
};

constexpr std::string_view kEagerServerSettings =
        R"({"mcpServers": {"executor": {"url": "https://mcp.example/mcp", "activation": "eager"}}})";
/// The same server with the call-time consent policy #843 added.
constexpr std::string_view kAskServerSettings = R"({"mcpServers": {"executor": )"
                                                R"({"url": "https://mcp.example/mcp", "activation": "eager", )"
                                                R"("approval": "ask"}}})";
constexpr std::string_view kTrustedStore = R"({"executor": true, "other": true})";

} // namespace

TEST_CASE("a URL elicitation is answered through the session and the original call continues",
        "[mcp][coding_agent][issue845][spec]") {
    ElicitationFixture fixture;
    fixture.write_settings(kEagerServerSettings);
    fixture.write_trust_store(kTrustedStore);
    fixture.transport->answer("tools/list", ScriptedMcpAnswer{.result = tests::tool_list_result({approvable_tool()})});
    fixture.answer_calls([](std::size_t round) {
        if (round == 0) {
            return ScriptedMcpAnswer{.result = url_elicitation()};
        }
        return ScriptedMcpAnswer{
                .result = tests::tool_call_result(support::JsonValue::array_t{support::JsonValue::object_t{
                        {"type", support::JsonValue("text")}, {"text", support::JsonValue("approved")}}})};
    });

    auto owned = fixture.create();
    auto& session = fixture.runtime.adopt_session(std::move(owned));
    REQUIRE(fixture.wait_until([&session] { return session.mcp_published_tools().size() == 1; }));
    // Nothing is pending before a call suspends.
    CHECK(session.pending_mcp_elicitations().empty());

    fixture.client->responses.push_back(tool_call_response("call-1", "mcp__executor__approve", "{}"));
    fixture.client->responses.push_back(ai::assistant_text_message("done"));
    // The run is in flight while the user is being asked, so the answer is
    // delivered from another thread than the one that started it.
    auto running = fixture.start_prompt(session, "approve please");
    REQUIRE(fixture.wait_until([&session] { return session.pending_mcp_elicitations().size() == 1; }));
    const auto question = session.pending_mcp_elicitations().front();
    CHECK(question.mode == McpElicitationMode::Url);
    CHECK(question.server_id == "executor");
    CHECK(question.tool_name == "approve");
    CHECK(question.url == "https://executor.invalid/mcp/approve/abc");
    CHECK(question.message == "Approve this action");
    // The opaque token is not in the projection: nothing a dialog renders can
    // read or normalize it.
    CHECK(question.elicitation_id.find("suspension-1") == std::string::npos);
    // The answer comes from a different thread than the one that asked.
    std::thread::id const caller = std::this_thread::get_id();
    std::atomic<bool> answered{false};
    std::thread([&session, &question, &answered, caller] {
        const auto outcome = session.answer_mcp_elicitation(coding_agent::McpElicitationAnswer{
                .elicitation_id = question.elicitation_id, .action = McpElicitationAction::Accept});
        answered.store(outcome.has_value() && std::this_thread::get_id() != caller);
    }).join();
    CHECK(answered.load());
    running.join();
    // The question is settled the moment it is answered.
    CHECK(session.pending_mcp_elicitations().empty());
    // A second answer for the same question is refused rather than completing
    // the settled call a second time.
    const auto repeat = session.answer_mcp_elicitation(coding_agent::McpElicitationAnswer{
            .elicitation_id = question.elicitation_id, .action = McpElicitationAction::Accept});
    REQUIRE(!repeat.has_value());
    CHECK(repeat.error().code == support::ErrorCode::Validation);

    REQUIRE(fixture.wait_until([&fixture] { return fixture.transport->request_count("tools/call") == 2; }));
    REQUIRE(session.pending_mcp_elicitations().empty());

    // The continuation carried the answer, the token, and a new id.
    const auto bodies = fixture.call_bodies();
    REQUIRE(bodies.size() == 2);
    CHECK(bodies[1].find(R"("action":"accept")") != std::string::npos);
    CHECK(bodies[1].find(R"("id":"r1")") != std::string::npos);
    CHECK(bodies[1].find(R"("requestState":{"token":"suspension-1"})") != std::string::npos);
    const auto first_id = support::read_json(bodies[0])->at("id").get<double>();
    const auto second_id = support::read_json(bodies[1])->at("id").get<double>();
    CHECK(second_id != first_id);
    // The original arguments are still the original arguments.
    CHECK(bodies[1].find(R"("name":"approve")") != std::string::npos);

    // The model sees the completed result, not the suspension.
    const auto results = tool_results(*fixture.client);
    REQUIRE_FALSE(results.empty());
    CHECK_FALSE(results.back().is_error);
    CHECK(results.back().text.find("approved") != std::string::npos);
    // Nothing was thrown away: the tool was called once from the model's point
    // of view, and the exchange behind it took two round trips.
    CHECK(fixture.transport->request_count("tools/call") == 2);
}

TEST_CASE("a declined elicitation continues the call with the decline", "[mcp][coding_agent][issue845][spec]") {
    ElicitationFixture fixture;
    fixture.write_settings(kEagerServerSettings);
    fixture.write_trust_store(kTrustedStore);
    fixture.transport->answer("tools/list", ScriptedMcpAnswer{.result = tests::tool_list_result({approvable_tool()})});
    fixture.answer_calls([](std::size_t round) {
        if (round == 0) {
            return ScriptedMcpAnswer{.result = url_elicitation()};
        }
        return ScriptedMcpAnswer{
                .result = tests::tool_call_result(support::JsonValue::array_t{support::JsonValue::object_t{
                        {"type", support::JsonValue("text")}, {"text", support::JsonValue("denied")}}})};
    });
    auto owned = fixture.create();
    auto& session = fixture.runtime.adopt_session(std::move(owned));
    REQUIRE(fixture.wait_until([&session] { return session.mcp_published_tools().size() == 1; }));

    fixture.client->responses.push_back(tool_call_response("call-1", "mcp__executor__approve", "{}"));
    fixture.client->responses.push_back(ai::assistant_text_message("done"));
    auto running = fixture.start_prompt(session, "approve please");
    REQUIRE(fixture.wait_until([&session] { return session.pending_mcp_elicitations().size() == 1; }));
    REQUIRE(session.answer_mcp_elicitation(
                           coding_agent::McpElicitationAnswer{
                                   .elicitation_id = session.pending_mcp_elicitations().front().elicitation_id,
                                   .action = McpElicitationAction::Decline})
                    .has_value());
    REQUIRE(fixture.wait_until([&fixture] { return fixture.transport->request_count("tools/call") == 2; }));
    running.join();

    const auto bodies = fixture.call_bodies();
    REQUIRE(bodies.size() == 2);
    CHECK(bodies[1].find(R"("action":"decline")") != std::string::npos);
}

TEST_CASE("an unanswered elicitation that reaches its bound fails the one call and sends nothing",
        "[mcp][coding_agent][issue845][spec]") {
    ElicitationFixture fixture;
    fixture.write_settings(kEagerServerSettings);
    fixture.write_trust_store(kTrustedStore);
    fixture.transport->answer("tools/list", ScriptedMcpAnswer{.result = tests::tool_list_result({approvable_tool()})});
    // The Upstream suspends and never completes, whatever is asked of it.
    fixture.answer_calls([](std::size_t) { return ScriptedMcpAnswer{.result = url_elicitation()}; });
    auto owned = fixture.create();
    auto& session = fixture.runtime.adopt_session(std::move(owned));
    REQUIRE(fixture.wait_until([&session] { return session.mcp_published_tools().size() == 1; }));

    fixture.client->responses.push_back(tool_call_response("call-1", "mcp__executor__approve", "{}"));
    fixture.client->responses.push_back(ai::assistant_text_message("done"));
    auto running = fixture.start_prompt(session, "approve please");

    // The user was asked, and the wait is bounded on the session's own timer.
    REQUIRE(fixture.wait_until([&session] { return session.pending_mcp_elicitations().size() == 1; }));
    REQUIRE(fixture.wait_until([&fixture] { return fixture.elicitation_delay.waiting() == 1; }));
    // The bound is the elicitation bound, not the connection's own waits.
    CHECK(fixture.elicitation_delay.next_delay() == std::chrono::minutes{5});
    REQUIRE(fixture.elicitation_delay.elapse_oldest());
    REQUIRE(fixture.wait_until([&session] { return session.pending_mcp_elicitations().empty(); }));

    // One failed call, no re-send: the user never answered, so nothing was
    // sent in their name.
    CHECK(fixture.transport->request_count("tools/call") == 1);
    running.join();
    const auto results = tool_results(*fixture.client);
    REQUIRE_FALSE(results.empty());
    CHECK(results.back().is_error);
    CHECK(results.back().text.find("not answered") != std::string::npos);
    // The run itself finished normally: a bounded wait is a failed call, not
    // a session that cannot be used.
    CHECK(session.is_open());
}

TEST_CASE("an undeclared input-request type fails the one call and the session keeps working",
        "[mcp][coding_agent][issue845][spec]") {
    ElicitationFixture fixture;
    fixture.write_settings(kEagerServerSettings);
    fixture.write_trust_store(kTrustedStore);
    fixture.transport->answer("tools/list", ScriptedMcpAnswer{.result = tests::tool_list_result({approvable_tool()})});
    fixture.answer_calls([](std::size_t round) {
        using JsonValue = support::JsonValue;
        if (round == 0) {
            return ScriptedMcpAnswer{.result = JsonValue::object_t{{"resultType", JsonValue("input_required")},
                                             {"requestState", JsonValue("t")},
                                             {"inputRequests",
                                                     JsonValue::array_t{JsonValue::object_t{{"id", JsonValue("r1")},
                                                             {"type", JsonValue("websocket")}}}}}};
        }
        return ScriptedMcpAnswer{
                .result = tests::tool_call_result(support::JsonValue::array_t{support::JsonValue::object_t{
                        {"type", support::JsonValue("text")}, {"text", support::JsonValue("still works")}}})};
    });
    auto owned = fixture.create();
    auto& session = fixture.runtime.adopt_session(std::move(owned));
    REQUIRE(fixture.wait_until([&session] { return session.mcp_published_tools().size() == 1; }));

    fixture.client->responses.push_back(tool_call_response("call-1", "mcp__executor__approve", "{}"));
    fixture.client->responses.push_back(ai::assistant_text_message("done"));
    REQUIRE(fixture.prompt(session, "approve please").has_value());

    // The user was never asked, and the run completed normally: a
    // non-conformant server degrades to one failed call, never to a session
    // that cannot be used (ADR 0008, story 32).
    CHECK(session.pending_mcp_elicitations().empty());
    const auto first = tool_results(*fixture.client);
    REQUIRE_FALSE(first.empty());
    CHECK(first.back().is_error);
    CHECK(first.back().text.find("does not declare") != std::string::npos);
    CHECK(session.is_open());
    // The same connection still serves an ordinary call, with no re-probe and
    // no re-list: the failure cost exactly one call.
    fixture.client->responses.push_back(tool_call_response("call-2", "mcp__executor__approve", "{}"));
    fixture.client->responses.push_back(ai::assistant_text_message("done"));
    REQUIRE(fixture.prompt(session, "try again").has_value());
    const auto second = tool_results(*fixture.client);
    REQUIRE(second.size() > first.size());
    CHECK_FALSE(second.back().is_error);
    CHECK(second.back().text.find("still works") != std::string::npos);
    CHECK(fixture.transport->request_count("server/discover") == 1);
    CHECK(fixture.transport->request_count("tools/call") == 2);
}

TEST_CASE("session Close with a pending elicitation leaves no waiter and no suspended call",
        "[mcp][coding_agent][issue845][spec]") {
    ElicitationFixture fixture;
    fixture.write_settings(kEagerServerSettings);
    fixture.write_trust_store(kTrustedStore);
    fixture.transport->answer("tools/list", ScriptedMcpAnswer{.result = tests::tool_list_result({approvable_tool()})});
    fixture.answer_calls([](std::size_t) { return ScriptedMcpAnswer{.result = url_elicitation()}; });
    auto owned = fixture.create();
    auto& session = fixture.runtime.adopt_session(std::move(owned));
    REQUIRE(fixture.wait_until([&session] { return session.mcp_published_tools().size() == 1; }));

    fixture.client->responses.push_back(tool_call_response("call-1", "mcp__executor__approve", "{}"));
    fixture.client->responses.push_back(ai::assistant_text_message("done"));
    auto running = fixture.start_prompt(session, "approve please");
    REQUIRE(fixture.wait_until([&session] { return session.pending_mcp_elicitations().size() == 1; }));
    const auto question = session.pending_mcp_elicitations().front().elicitation_id;

    session.close();
    running.join();
    // The question ended with the session: no waiter, no dialog that could
    // still answer a call being torn down.
    CHECK(session.pending_mcp_elicitations().empty());
    const auto late = session.answer_mcp_elicitation(
            coding_agent::McpElicitationAnswer{.elicitation_id = question, .action = McpElicitationAction::Accept});
    REQUIRE(!late.has_value());
    // And nothing was re-sent on the way out.
    CHECK(fixture.transport->request_count("tools/call") == 1);
}

TEST_CASE("an approved call is consented to once and the Multi Round-Trip retry is never re-prompted",
        "[mcp][coding_agent][issue845][issue843][spec]") {
    // The interlock between #843's call approval and #845's Multi Round-Trip
    // loop. The consent question is asked by the Agent's before-tool-call
    // hook, which is above the call; the loop and every retried round trip are
    // inside that one call, below the hook. So one consent answers the whole
    // exchange, and no second question can appear between two round trips.
    ElicitationFixture fixture;
    fixture.write_settings(kAskServerSettings);
    fixture.write_trust_store(kTrustedStore);
    fixture.transport->answer("tools/list", ScriptedMcpAnswer{.result = tests::tool_list_result({approvable_tool()})});
    fixture.answer_calls([](std::size_t round) {
        if (round == 0) {
            return ScriptedMcpAnswer{.result = url_elicitation()};
        }
        return ScriptedMcpAnswer{
                .result = tests::tool_call_result(support::JsonValue::array_t{support::JsonValue::object_t{
                        {"type", support::JsonValue("text")}, {"text", support::JsonValue("approved")}}})};
    });
    auto owned = fixture.create();
    auto& session = fixture.runtime.adopt_session(std::move(owned));
    REQUIRE(fixture.wait_until([&session] { return session.mcp_published_tools().size() == 1; }));

    fixture.client->responses.push_back(tool_call_response("call-1", "mcp__executor__approve", "{}"));
    fixture.client->responses.push_back(ai::assistant_text_message("done"));
    auto running = fixture.start_prompt(session, "approve please");
    // Consent is asked once, before the call reaches the Upstream at all.
    REQUIRE(fixture.wait_until([&fixture] { return fixture.approval.count->load() == 1; }));
    // Then the Upstream suspends the already-consented call.
    REQUIRE(fixture.wait_until([&session] { return session.pending_mcp_elicitations().size() == 1; }));
    // The question is on screen and the consent question is not asked again:
    // a Mid-Round-Trip prompt would be a second question about a call the user
    // already authorized.
    CHECK(fixture.approval.count->load() == 1);
    REQUIRE(session.answer_mcp_elicitation(
                           coding_agent::McpElicitationAnswer{
                                   .elicitation_id = session.pending_mcp_elicitations().front().elicitation_id,
                                   .action = McpElicitationAction::Accept})
                    .has_value());
    REQUIRE(fixture.wait_until([&fixture] { return fixture.transport->request_count("tools/call") == 2; }));
    running.join();

    // Two round trips behind one consent, and the retry was never re-prompted.
    CHECK(fixture.approval.count->load() == 1);
    const auto bodies = fixture.call_bodies();
    REQUIRE(bodies.size() == 2);
    CHECK(bodies[1].find(R"("action":"accept")") != std::string::npos);
    const auto results = tool_results(*fixture.client);
    REQUIRE_FALSE(results.empty());
    CHECK_FALSE(results.back().is_error);
    CHECK(results.back().text.find("approved") != std::string::npos);
    CHECK(session.is_open());
}
