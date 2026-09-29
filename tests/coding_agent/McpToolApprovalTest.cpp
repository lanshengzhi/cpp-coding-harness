// Call approval for `approval: "ask"` Upstream MCP Servers (issue #843; spec
// #833 story 22).
//
// The cases cross the one production door — `create_agent_session_async` — for
// everything the product does with a call that needs consent, and drive the
// policy value directly where the case is about a decision the session
// assembly only ever fills. The injected seams are the MCP Host's own transport
// (`tests::ScriptedMcpTransport`) and timer (`tests::ScriptedMcpDelay`), plus
// the session's own call-approval prompt — the frontend seam the Native TUI
// fills with its prompt slot.

#include <cch/coding_agent/McpToolApproval.hpp>
#include "ai/ModelStreamBridge.hpp"
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/AgentSessionCreationRequest.hpp"
#include "coding_agent/runtime/McpToolApprovalPolicy.hpp"
#include "coding_agent/runtime/McpToolBinding.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/AgentRootFixture.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/Json.hpp"
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
#include <variant>
#include <vector>

using namespace cch;

namespace {

namespace runtime_ns = cch::coding_agent::runtime;
using coding_agent::McpToolApprovalAnswer;
using coding_agent::McpToolApprovalPrompter;
using coding_agent::McpToolApprovalRequest;
using tests::ScriptedMcpAnswer;

constexpr std::chrono::milliseconds kBudget{5000};

/// One `tools/list` entry whose `query` property is required, so the schema the
/// bound tool carries is a real one.
[[nodiscard]] support::JsonValue required_tool_entry() { return tests::tool_entry("search", {"query"}); }

void answer_upstream(tests::ScriptedMcpTransport& transport) {
    transport.answer("server/discover", ScriptedMcpAnswer{.result = tests::discover_result()});
    transport.answer("tools/list", ScriptedMcpAnswer{.result = tests::tool_list_result({required_tool_entry()})});
    transport.answer("tools/call",
            ScriptedMcpAnswer{
                    .result = tests::tool_call_result(support::JsonValue{
                            support::JsonValue::array_t{support::JsonValue{support::JsonValue::object_t{
                                    {"type", support::JsonValue("text")}, {"text", support::JsonValue("ran it")}}}}})});
}

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

/// The prompter a case installs: it records every question it was asked and
/// answers with the answer the case chose. The answers are consumed in order,
/// so a case can answer the first call differently from the second.
class ScriptedApprovalPrompter final {
public:
    struct State {
        std::vector<McpToolApprovalRequest> requests;
        std::vector<McpToolApprovalAnswer> answers;
        std::size_t index{0};
        /// The run's stop token, so a case can prove the policy forwards the
        /// run's cancellation to the prompt rather than inventing one.
        std::vector<bool> stop_requested;
    };

    std::shared_ptr<State> state{std::make_shared<State>()};

    [[nodiscard]] McpToolApprovalPrompter prompter() const {
        auto state = this->state;
        return [state](McpToolApprovalRequest request, std::stop_token stop_token) {
            const auto index = state->index++;
            state->requests.push_back(std::move(request));
            state->stop_requested.push_back(stop_token.stop_requested());
            const auto answer = index < state->answers.size() ? state->answers[index] : McpToolApprovalAnswer::Declined;
            return support::AsyncResult<McpToolApprovalAnswer>{
                    std::expected<McpToolApprovalAnswer, support::Error>{answer}};
        };
    }

    [[nodiscard]] std::size_t prompts() const { return state->requests.size(); }
};

/// A prompter that always fails, for the outcome the policy has to survive.
[[nodiscard]] McpToolApprovalPrompter failing_prompter() {
    return [](McpToolApprovalRequest, std::stop_token) {
        return support::AsyncResult<McpToolApprovalAnswer>{
                std::unexpected(support::make_error(support::ErrorCode::Unknown, "the prompt surface is gone"))};
    };
}

/// A connection for one Server Id that is never dialled here: the binding only
/// needs the identity a publication is registered under.
[[nodiscard]] std::shared_ptr<mcp::UpstreamConnection> connection_for(std::string server_id) {
    return std::make_shared<mcp::UpstreamConnection>(std::move(server_id),
            std::make_shared<tests::ScriptedMcpTransport>(),
            mcp::UpstreamConnectionOptions{
                    .url = "https://mcp.example/mcp",
                    .delay = [](std::chrono::milliseconds,
                                     std::stop_token) { return support::AsyncResult<void>(support::Expected<void>{}); },
            });
}

/// A binding holding one published tool for each of two servers: `executor`
/// configured `ask`, `open` configured `allow`.
[[nodiscard]] std::shared_ptr<runtime_ns::McpToolBinding> ask_and_allow_binding() {
    auto binding = std::make_shared<runtime_ns::McpToolBinding>();
    binding->set_servers({
            coding_agent::UserMcpServerSettings{.server_id = "executor",
                    .url = "https://mcp.example/mcp",
                    .approval = coding_agent::McpServerApproval::Ask},
            coding_agent::UserMcpServerSettings{.server_id = "open",
                    .url = "https://mcp.example/mcp",
                    .approval = coding_agent::McpServerApproval::Allow},
    });
    const auto published = binding->publish(connection_for("executor"), mcp::UpstreamToolDescriptor{.name = "search"});
    REQUIRE(published.has_value());
    const auto allowed = binding->publish(connection_for("open"), mcp::UpstreamToolDescriptor{.name = "search"});
    REQUIRE(allowed.has_value());
    return binding;
}

/// One hook decision for a named call with the given prepared arguments.
[[nodiscard]] agent::BeforeToolCallResult decide(
        agent::BeforeToolCallHook& hook, std::string name, std::string raw_arguments = R"({"query":"weather"})") {
    agent::BeforeToolCallContext context;
    context.tool_call = ai::tool_call_content("call-1", std::move(name), raw_arguments);
    context.args = *support::read_json(raw_arguments);
    const auto outcome = drive_locally(hook(std::move(context), std::stop_token{}));
    REQUIRE(outcome.has_value());
    return *outcome;
}

/// One scripted chat client: it serves queued assistant messages in order and
/// records every request, so a case can read what the model was shown.
class ApprovalProvider final : public tests::ScriptedProvider {
public:
    ApprovalProvider() : ScriptedProvider("sdk-host") {}

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

/// The tool results the model saw, in tool-call order, beside each error flag.
struct SeenToolResult {
    std::string text{};
    bool is_error{false};
};

[[nodiscard]] std::vector<SeenToolResult> tool_results(const ApprovalProvider& client) {
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

[[nodiscard]] ai::AssistantMessage tool_call_response(
        std::string call_id, std::string tool_name, std::string raw_arguments) {
    auto message = ai::assistant_text_message("");
    message.content.clear();
    message.content.emplace_back(
            ai::tool_call_content(std::move(call_id), std::move(tool_name), std::move(raw_arguments)));
    return message;
}

/// A session assembled against a temp Agent Config Directory, with the MCP
/// Host's transport and timer and the session's call-approval prompt injected.
struct ApprovalFixture {
    tests::TempWorkspace workspace;
    std::filesystem::path agent_dir;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard kimi_guard{"KIMI_API_KEY"};
    tests::EnvVarGuard deepseek_guard{"DEEPSEEK_API_KEY"};
    tests::RuntimeFixture runtime;
    tests::RuntimeLoopDriver driver;
    std::shared_ptr<tests::ScriptedMcpTransport> transport{std::make_shared<tests::ScriptedMcpTransport>()};
    tests::ScriptedMcpDelay delay;
    std::shared_ptr<ApprovalProvider> client{std::make_shared<ApprovalProvider>()};
    std::shared_ptr<ScriptedApprovalPrompter> prompter{std::make_shared<ScriptedApprovalPrompter>()};

    ApprovalFixture() : driver(runtime) {
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

    void write_trust_store() const {
        std::ofstream output(agent_dir / "mcp-trust.json", std::ios::binary | std::ios::trunc);
        output << R"({"executor": true})";
    }

    [[nodiscard]] std::unique_ptr<coding_agent::AgentSession> create() { return create_with(/*with_prompter=*/true); }

    /// The same assembly without a call-approval prompt: a non-interactive
    /// session, which is what a headless caller builds.
    [[nodiscard]] std::unique_ptr<coding_agent::AgentSession> create_headless() {
        return create_with(/*with_prompter=*/false);
    }

    [[nodiscard]] std::unique_ptr<coding_agent::AgentSession> create_with(bool with_prompter) {
        runtime_ns::AgentSessionCreationRequest request;
        request.execution_runtime_target = runtime.make_target();
        request.session_facts.no_skills = true;
        request.session_facts.no_prompt_templates = true;
        request.workspace = workspace.path();
        request.session_target = coding_agent::InMemorySessionTarget{};
        request.mcp_transport = transport;
        request.mcp_delay = [this](std::chrono::milliseconds wait, std::stop_token stop_token) {
            return delay.request(wait, stop_token);
        };
        if (with_prompter) {
            request.mcp_tool_approval_prompter = prompter->prompter();
        }
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

    [[nodiscard]] static bool wait_until(std::function<bool()> condition) {
        const auto deadline = std::chrono::steady_clock::now() + kBudget;
        while (!condition() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        return condition();
    }
};

constexpr std::string_view kAskServerSettings = R"({"mcpServers": {"executor": )"
                                                R"({"url": "https://mcp.example/mcp", "activation": "eager", )"
                                                R"("approval": "ask"}}})";

} // namespace

TEST_CASE("a call that is not an ask call is never put to the prompt", "[mcp][issue843][spec]") {
    ScriptedApprovalPrompter prompter;
    auto hook = runtime_ns::McpToolApprovalPolicy{ask_and_allow_binding(), prompter.prompter()}.make_hook();

    // The built-in set and an `allow` server: neither reaches the prompt, and
    // neither is blocked. The default path is the `allow` path, byte-identical
    // in behavior to a build with no MCP host at all.
    CHECK_FALSE(decide(hook, "read").block);
    CHECK_FALSE(decide(hook, "mcp__open__search").block);
    CHECK(prompter.prompts() == 0);
}

TEST_CASE("an ask call is put to the prompt once, with the tool name and its arguments", "[mcp][issue843][spec]") {
    ScriptedApprovalPrompter prompter;
    prompter.state->answers = {McpToolApprovalAnswer::Allowed};
    auto hook = runtime_ns::McpToolApprovalPolicy{ask_and_allow_binding(), prompter.prompter()}.make_hook();

    CHECK_FALSE(decide(hook, "mcp__executor__search").block);
    REQUIRE(prompter.prompts() == 1);
    const auto& asked = prompter.state->requests.front();
    // The prompt is answerable only if it names the call exactly: the
    // Qualified Tool Name the model called, the Upstream's own tool name it
    // targets, and the arguments that would go on the wire.
    CHECK(asked.qualified_tool_name == "mcp__executor__search");
    CHECK(asked.server_id == "executor");
    CHECK(asked.tool_name == "search");
    CHECK(asked.arguments_json == R"({"query":"weather"})");
}

TEST_CASE("a declined ask call and a dismissed one are each refused, and differently", "[mcp][issue843][spec]") {
    ScriptedApprovalPrompter prompter;
    prompter.state->answers = {McpToolApprovalAnswer::Declined, McpToolApprovalAnswer::Cancelled};
    auto hook = runtime_ns::McpToolApprovalPolicy{ask_and_allow_binding(), prompter.prompter()}.make_hook();

    const auto declined = decide(hook, "mcp__executor__search");
    const auto cancelled = decide(hook, "mcp__executor__search");
    REQUIRE(declined.block);
    REQUIRE(cancelled.block);
    REQUIRE(declined.reason.has_value());
    REQUIRE(cancelled.reason.has_value());
    // Both name the call, so the model-visible failure is actionable.
    CHECK(declined.reason->find("mcp__executor__search") != std::string::npos);
    CHECK(cancelled.reason->find("mcp__executor__search") != std::string::npos);
    // A dismissal is not a decision, so the two refusals are not the same
    // sentence: nothing is remembered either way.
    CHECK(declined.reason != cancelled.reason);
    CHECK(cancelled.reason->find("dismissed") != std::string::npos);
    // One question per call: the hook is asked, answers, and is done.
    CHECK(prompter.prompts() == 2);
}

TEST_CASE("a session with no prompt refuses the ask call rather than running it", "[mcp][issue843][spec]") {
    // No prompter is a non-interactive session: there is nobody to ask, and
    // consent cannot be assumed. This is the same fail-closed rule the
    // first-enable trust gate applies to a server it cannot ask about.
    auto hook = runtime_ns::McpToolApprovalPolicy{ask_and_allow_binding()}.make_hook();
    const auto refused = decide(hook, "mcp__executor__search");
    REQUIRE(refused.block);
    REQUIRE(refused.reason.has_value());
    CHECK(refused.reason->find("mcp__executor__search") != std::string::npos);
    // Everything else is untouched in the same session.
    CHECK_FALSE(decide(hook, "read").block);
}

TEST_CASE("a prompt that fails refuses the ask call rather than running it", "[mcp][issue843][spec]") {
    auto hook = runtime_ns::McpToolApprovalPolicy{ask_and_allow_binding(), failing_prompter()}.make_hook();
    const auto refused = decide(hook, "mcp__executor__search");
    REQUIRE(refused.block);
    REQUIRE(refused.reason.has_value());
    CHECK(refused.reason->find("mcp__executor__search") != std::string::npos);
}

TEST_CASE("the policy hands the run's cancellation to the prompt", "[mcp][issue843][spec]") {
    ScriptedApprovalPrompter prompter;
    prompter.state->answers = {McpToolApprovalAnswer::Allowed};
    auto hook = runtime_ns::McpToolApprovalPolicy{ask_and_allow_binding(), prompter.prompter()}.make_hook();

    std::stop_source source;
    agent::BeforeToolCallContext context;
    context.tool_call = ai::tool_call_content("call-1", "mcp__executor__search", R"({"query":"weather"})");
    context.args = *support::read_json(R"({"query":"weather"})");
    const auto outcome = drive_locally(hook(std::move(context), source.get_token()));
    REQUIRE(outcome.has_value());
    REQUIRE(prompter.prompts() == 1);
    // The prompt is cancellable: the frontend decides what a stop means, and
    // the policy never invents a second cancellation channel of its own.
    REQUIRE(prompter.state->stop_requested.size() == 1);
    CHECK_FALSE(prompter.state->stop_requested.front());
}

TEST_CASE("consent runs exactly one upstream call", "[mcp][issue843][spec]") {
    ApprovalFixture fixture;
    fixture.write_settings(kAskServerSettings);
    fixture.write_trust_store();
    answer_upstream(*fixture.transport);
    fixture.prompter->state->answers = {McpToolApprovalAnswer::Allowed};

    auto owned = fixture.create();
    auto& session = fixture.runtime.adopt_session(std::move(owned));
    REQUIRE(ApprovalFixture::wait_until([&session] { return session.mcp_published_tools().size() == 1; }));

    fixture.client->responses.push_back(
            tool_call_response("call-1", "mcp__executor__search", R"({"query":"weather"})"));
    fixture.client->responses.push_back(ai::assistant_text_message("done"));
    REQUIRE(fixture.prompt(session, "search please").has_value());

    // One consent answers exactly one call: one question, one upstream request.
    CHECK(fixture.prompter->prompts() == 1);
    CHECK(fixture.transport->request_count("tools/call") == 1);
    const auto results = tool_results(*fixture.client);
    REQUIRE_FALSE(results.empty());
    CHECK_FALSE(results.back().is_error);
    CHECK(results.back().text.find("ran it") != std::string::npos);
}

TEST_CASE("decline and dismissal run no upstream call and fail only the one call", "[mcp][issue843][spec]") {
    for (const auto answer : {McpToolApprovalAnswer::Declined, McpToolApprovalAnswer::Cancelled}) {
        ApprovalFixture fixture;
        fixture.write_settings(kAskServerSettings);
        fixture.write_trust_store();
        answer_upstream(*fixture.transport);
        fixture.prompter->state->answers = {answer};

        auto owned = fixture.create();
        auto& session = fixture.runtime.adopt_session(std::move(owned));
        REQUIRE(ApprovalFixture::wait_until([&session] { return session.mcp_published_tools().size() == 1; }));

        fixture.client->responses.push_back(
                tool_call_response("call-1", "mcp__executor__search", R"({"query":"weather"})"));
        fixture.client->responses.push_back(ai::assistant_text_message("done"));
        // The run completes normally: a refusal is one failed tool call, never a
        // session failure (ADR 0008).
        REQUIRE(fixture.prompt(session, "search please").has_value());
        CHECK(session.is_open());

        CHECK(fixture.prompter->prompts() == 1);
        CHECK(fixture.transport->request_count("tools/call") == 0);
        const auto results = tool_results(*fixture.client);
        REQUIRE(results.size() == 1);
        CHECK(results.front().is_error);
        CHECK(results.front().text.find("mcp__executor__search") != std::string::npos);
        // No retry storm: the refusal is not re-asked, so the second turn the
        // model takes after it produces no further question.
        CHECK(fixture.prompter->prompts() == 1);

        // The session is still usable, and an `allow`-less but ordinary
        // conversation continues to work after a refusal.
        fixture.client->responses.push_back(ai::assistant_text_message("still here"));
        REQUIRE(fixture.prompt(session, "anything else?").has_value());
    }
}

TEST_CASE("a headless session refuses the ask call and never reaches the Upstream", "[mcp][issue843][spec]") {
    ApprovalFixture fixture;
    fixture.write_settings(kAskServerSettings);
    fixture.write_trust_store();
    answer_upstream(*fixture.transport);

    // No call-approval prompt at all: the non-interactive shape. The call is
    // refused rather than run, so a headless session cannot execute a
    // third-party tool nobody authorized.
    auto owned = fixture.create_headless();
    auto& session = fixture.runtime.adopt_session(std::move(owned));
    REQUIRE(ApprovalFixture::wait_until([&session] { return session.mcp_published_tools().size() == 1; }));

    fixture.client->responses.push_back(
            tool_call_response("call-1", "mcp__executor__search", R"({"query":"weather"})"));
    fixture.client->responses.push_back(ai::assistant_text_message("done"));
    REQUIRE(fixture.prompt(session, "search please").has_value());

    CHECK(fixture.transport->request_count("tools/call") == 0);
    const auto results = tool_results(*fixture.client);
    REQUIRE(results.size() == 1);
    CHECK(results.front().is_error);
    CHECK(results.front().text.find("mcp__executor__search") != std::string::npos);
}
