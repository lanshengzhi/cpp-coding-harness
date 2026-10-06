// Spec #865 fourth slice (#872): MCP failure isolation and cancellation. The
// fixture is the same real MCP stdio child as #869 (`fixtures/pi-mcp/
// echo_server.py`), extended with `hang`, `slow`, and `debug/exit` fault modes
// and a trace file the tests poll. The scripted fake provider serves the model,
// so no live keys or network are used.
//
// Acceptance pairs each property with a case the naive check does not catch:
//   * a call that fails with a cancellation error is not proof of propagation,
//     so the server's trace must show `notifications/cancelled` for the same
//     request id, and a later call must still complete;
//   * a server that keeps streaming after acknowledging cancellation must still
//     fail the caller with the cancelled error (a client that waited for the
//     request to settle would stall);
//   * a hung server must fail the call with an explicit timeout, and a server
//     that dies between tools/list and tools/call must fail at call time —
//     never a silent hang.

#include "ai/ModelStreamBridge.hpp"
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/mcp/McpStdioClient.hpp"
#include "coding_agent/mcp/McpStdioServerConfig.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/Json.hpp"
#include "support/McpTestKit.hpp"
#include "support/ModelsFixture.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/RuntimeLoopDriver.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

using namespace std::chrono_literals;

[[nodiscard]] std::string fixture_path(std::string_view name) {
    return std::string{CCH_SOURCE_DIR} + "/fixtures/pi-mcp/" + std::string{name};
}

/// The fixture server with a per-test trace file and request deadline.
[[nodiscard]] coding_agent::mcp::McpStdioServerConfig echo_server_config(
        const std::filesystem::path& trace, std::chrono::milliseconds request_timeout) {
    coding_agent::mcp::McpStdioServerConfig config;
    config.name = "echo";
    config.command = "python3";
    config.args = {fixture_path("echo_server.py")};
    config.env.emplace("PIKE_MCP_TRACE", trace.string());
    config.request_timeout = request_timeout;
    return config;
}

// The shared fixture-trace and tool-call helpers live in `tests/support`
// (§11.5), so the stdio and HTTP session tests cannot drift apart.
using cch::tests::first_text_content;
using cch::tests::tools_call_params;
using cch::tests::trace_contains;
using cch::tests::wait_for;

/// Scripted provider whose stream returns an aborted terminal while the
/// prompt's stop token is requested; otherwise it serves the next scripted
/// step (a tool call) or a plain text answer. The aborted stream does not
/// consume a step, so a session that aborts one turn still serves the next
/// prompt's scripted call.
class McpSequenceProvider final : public tests::ScriptedProvider {
public:
    struct Step {
        std::string tool_name;
        support::JsonValue arguments;
        std::string text;
    };

    explicit McpSequenceProvider(std::vector<Step> steps) : ScriptedProvider("fake"), steps_(std::move(steps)) {}

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext, coding_agent::ModelRuntimeTestStreamOptions options) override {
        const bool aborted = options.stop_token.stop_requested();
        const int index = aborted ? -1 : request_count_++;
        const Step step = (index >= 0 && index < static_cast<int>(steps_.size()))
                                  ? steps_[static_cast<std::size_t>(index)]
                                  : Step{.tool_name = {}, .arguments = support::JsonValue{}, .text = "done"};
        return ai::detail::make_model_stream(
                [model = std::move(model), aborted, step](ai::AssistantEventSink sink) mutable
                        -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
                    ai::AssistantMessage round;
                    round.provider = "mcp-fake";
                    round.api = "fake";
                    round.model = model.id;
                    if (aborted) {
                        round.content = {};
                        round.stop_reason = ai::AssistantStopReason::Aborted;
                        round.error_message = "Request was aborted";
                    } else if (!step.tool_name.empty()) {
                        auto serialized = support::write_json(step.arguments);
                        round.content = {ai::text_content("calling the MCP tool")};
                        round.stop_reason = ai::AssistantStopReason::ToolUse;
                        round.content.emplace_back(ai::ToolCallContent{
                                .id = "call_mcp_" + step.tool_name,
                                .name = step.tool_name,
                                .arguments = step.arguments,
                                .raw_arguments = serialized ? std::move(*serialized) : std::string{"{}"},
                                .thought_signature = std::nullopt,
                                .arguments_valid = true,
                                .argument_error = std::nullopt,
                        });
                    } else {
                        round.content = {ai::text_content(step.text)};
                        round.stop_reason = ai::AssistantStopReason::Stop;
                    }
                    if (sink) {
                        if (auto emitted = sink(ai::AssistantStartEvent{.partial = round}); !emitted) {
                            co_return std::unexpected(emitted.error());
                        }
                    }
                    co_return round;
                });
    }

private:
    std::vector<Step> steps_;
    int request_count_{0};
};

[[nodiscard]] std::unique_ptr<coding_agent::AgentSession> make_session(tests::RuntimeFixture& runtime,
        const tests::TempWorkspace& workspace,
        std::shared_ptr<tests::ScriptedProvider> provider,
        std::vector<coding_agent::mcp::McpStdioServerConfig> servers) {
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.request_model = tests::scripted_request_model("fake", "fake-model");
    options.execution_runtime_target = runtime.make_target();
    options.mcp_servers = std::move(servers);
    auto models = tests::models_from_provider(std::move(provider));
    auto created = runtime.run(coding_agent::create_agent_session_async(
            std::move(options), std::nullopt, tests::cli_fake_overrides(std::move(models))));
    REQUIRE(created.has_value());
    return std::move(created->session);
}

[[nodiscard]] std::optional<std::string> tool_result_text(
        const coding_agent::AgentSession& session, std::string_view name) {
    for (const auto& message : session.snapshot().agent_state.messages) {
        const auto* result = std::get_if<ai::ToolResultMessage>(&message);
        if (result != nullptr && result->tool_name == name) {
            return ai::text_from_content(result->content);
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool tool_result_is_error(const coding_agent::AgentSession& session, std::string_view name) {
    for (const auto& message : session.snapshot().agent_state.messages) {
        const auto* result = std::get_if<ai::ToolResultMessage>(&message);
        if (result != nullptr && result->tool_name == name) {
            return result->is_error;
        }
    }
    return false;
}

[[nodiscard]] std::optional<ai::AssistantStopReason> last_assistant_stop_reason(
        const coding_agent::AgentSession& session) {
    const auto& messages = session.snapshot().agent_state.messages;
    for (auto iterator = messages.rbegin(); iterator != messages.rend(); ++iterator) {
        if (const auto* message = std::get_if<ai::AssistantMessage>(&*iterator)) {
            return message->stop_reason;
        }
    }
    return std::nullopt;
}

/// Start one `tools/call` for `tool_name`, wait until the server has it in
/// flight, request cancellation through the caller's stop token, and return the
/// call's terminal outcome. Waiting for the server's `hang-start` trace line
/// guarantees the cancellation hits a running call rather than a queued one.
[[nodiscard]] boost::asio::awaitable<support::Expected<support::JsonValue>> cancel_in_flight_call(
        std::shared_ptr<coding_agent::mcp::McpStdioClient> client,
        std::filesystem::path trace,
        std::stop_source& cancel,
        std::string tool_name) {
    const auto executor = co_await boost::asio::this_coro::executor;
    auto first = std::make_shared<std::optional<support::Expected<support::JsonValue>>>();
    boost::asio::co_spawn(
            executor,
            [client,
                    first,
                    token = cancel.get_token(),
                    params = tools_call_params(std::move(tool_name),
                            support::JsonValue::object_t{})]() mutable -> boost::asio::awaitable<void> {
                first->emplace(co_await support::detail::await_async_result(
                        client->request("tools/call", std::move(params), token)));
                co_return;
            },
            boost::asio::detached);
    co_await wait_for(executor, [&] { return trace_contains(trace, "hang-start"); }, 10s);
    cancel.request_stop();
    co_await wait_for(executor, [&] { return first->has_value(); }, 10s);
    if (!first->has_value()) {
        co_return std::unexpected(
                support::make_error(support::ErrorCode::Timeout, "the cancelled call never completed"));
    }
    co_return std::move(**first);
}

} // namespace

TEST_CASE("a cancelled in-flight MCP call notifies the server, fails the call, and leaves the connection usable",
        "[coding_agent][mcp][issue872][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;
    const auto trace = workspace.path() / "trace.log";

    auto connected = tests::run_awaitable(
            runtime, coding_agent::mcp::McpStdioClient::connect(echo_server_config(trace, 5000ms)));
    REQUIRE(connected.has_value());
    const std::shared_ptr<coding_agent::mcp::McpStdioClient> client = *connected;

    std::stop_source cancel;
    const auto cancelled = tests::run_awaitable(runtime, cancel_in_flight_call(client, trace, cancel, "hang"));

    // The call fails with a cancellation error...
    REQUIRE_FALSE(cancelled.has_value());
    CHECK(cancelled.error().code == support::ErrorCode::Cancelled);
    // ...and the server observed the cancellation for the same request id,
    // which is what separates propagation from a local-only failure.
    CHECK(trace_contains(trace, "cancelled requestId="));

    // The connection stays usable: a subsequent call completes normally.
    const auto after = tests::run_awaitable(runtime,
            support::detail::await_async_result(client->request(
                    "tools/call", tools_call_params("echo", support::JsonValue::object_t{{"text", "after cancel"}}))));
    REQUIRE(after.has_value());
    CHECK(first_text_content(*after) == std::optional<std::string>{"after cancel"});
}

TEST_CASE("a server that keeps streaming after acknowledging cancellation still surfaces the cancelled error",
        "[coding_agent][mcp][issue872][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;
    const auto trace = workspace.path() / "trace.log";

    auto connected = tests::run_awaitable(
            runtime, coding_agent::mcp::McpStdioClient::connect(echo_server_config(trace, 5000ms)));
    REQUIRE(connected.has_value());
    const std::shared_ptr<coding_agent::mcp::McpStdioClient> client = *connected;

    std::stop_source cancel;
    const auto cancelled = tests::run_awaitable(runtime, cancel_in_flight_call(client, trace, cancel, "hang"));

    REQUIRE_FALSE(cancelled.has_value());
    CHECK(cancelled.error().code == support::ErrorCode::Cancelled);
    // The server acknowledged the cancellation and streamed more frames, yet
    // the caller still resolved with the cancelled error rather than waiting
    // for a response that never comes.
    CHECK(trace_contains(trace, "cancelled requestId="));
    CHECK(trace_contains(trace, "stream id="));
}

TEST_CASE("a hung MCP server fails the call with an explicit timeout, never a silent hang",
        "[coding_agent][mcp][issue872][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;
    const auto trace = workspace.path() / "trace.log";

    auto connected = tests::run_awaitable(
            runtime, coding_agent::mcp::McpStdioClient::connect(echo_server_config(trace, 2000ms)));
    REQUIRE(connected.has_value());
    const std::shared_ptr<coding_agent::mcp::McpStdioClient> client = *connected;

    // A slow-but-answering server within the deadline resolves normally, so
    // the timeout is a deadline, not an unconditional failure.
    const auto slow = tests::run_awaitable(runtime,
            support::detail::await_async_result(client->request("tools/call",
                    tools_call_params("slow", support::JsonValue::object_t{{"ms", 20.0}, {"text", "within"}}))));
    REQUIRE(slow.has_value());
    CHECK(first_text_content(*slow) == std::optional<std::string>{"within"});

    // A server that never answers fails explicitly with a timeout.
    const auto hung = tests::run_awaitable(runtime,
            support::detail::await_async_result(
                    client->request("tools/call", tools_call_params("hang", support::JsonValue::object_t{}))));
    REQUIRE_FALSE(hung.has_value());
    CHECK(hung.error().code == support::ErrorCode::Timeout);
    CHECK(hung.error().message.find("timed out") != std::string::npos);
}

TEST_CASE("an MCP call errors explicitly when the server dies between tools/list and tools/call",
        "[coding_agent][mcp][issue872][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;
    const auto trace = workspace.path() / "trace.log";

    auto connected = tests::run_awaitable(
            runtime, coding_agent::mcp::McpStdioClient::connect(echo_server_config(trace, 5000ms)));
    REQUIRE(connected.has_value());
    const std::shared_ptr<coding_agent::mcp::McpStdioClient> client = *connected;

    const auto listed =
            tests::run_awaitable(runtime, support::detail::await_async_result(client->request("tools/list")));
    REQUIRE(listed.has_value());

    // The server responds to `debug/exit` and dies; the process is gone before
    // the next tools/call.
    const auto exited =
            tests::run_awaitable(runtime, support::detail::await_async_result(client->request("debug/exit")));
    REQUIRE(exited.has_value());

    const auto call = tests::run_awaitable(runtime,
            support::detail::await_async_result(client->request(
                    "tools/call", tools_call_params("echo", support::JsonValue::object_t{{"text", "too late"}}))));
    REQUIRE_FALSE(call.has_value());
    // A write to the dead process or a read of its closed stdout both surface
    // as an explicit transport error naming the server.
    CHECK(call.error().code == support::ErrorCode::Process);
    CHECK(call.error().message.find("echo") != std::string::npos);
}

TEST_CASE("aborting an Agent Turn cancels an in-flight MCP call and the session stays usable",
        "[coding_agent][mcp][issue872][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;
    const auto trace = workspace.path() / "trace.log";

    auto provider = std::make_shared<McpSequenceProvider>(std::vector<McpSequenceProvider::Step>{
            McpSequenceProvider::Step{
                    .tool_name = "mcp__echo__hang", .arguments = support::JsonValue::object_t{}, .text = {}},
            McpSequenceProvider::Step{.tool_name = "mcp__echo__echo",
                    .arguments = support::JsonValue::object_t{{"text", "second"}},
                    .text = {}},
    });
    auto created = make_session(runtime, workspace, provider, {echo_server_config(trace, 5000ms)});
    auto& session = runtime.adopt_session(std::move(created));
    tests::RuntimeLoopDriver driver(runtime);

    // Abort through the Session's ordinary cancellation path once the MCP call
    // is genuinely in flight on the server.
    std::thread watcher([&] {
        for (int attempt = 0; attempt < 1000; ++attempt) {
            if (trace_contains(trace, "hang-start")) {
                session.abort();
                return;
            }
            std::this_thread::sleep_for(10ms);
        }
    });

    const auto first = session.prompt_blocking("use the hang tool");
    watcher.join();

    REQUIRE(first.has_value());
    // The in-flight MCP tool call failed with the cancellation error the
    // transport produced, and the server observed the cancellation.
    CHECK(tool_result_is_error(session, "mcp__echo__hang"));
    const auto text = tool_result_text(session, "mcp__echo__hang").value_or("<none>");
    CHECK(text.find("cancel") != std::string::npos);
    CHECK(trace_contains(trace, "cancelled requestId="));
    // The Turn settled through the ordinary aborted lifecycle.
    CHECK(last_assistant_stop_reason(session) ==
            std::optional<ai::AssistantStopReason>{ai::AssistantStopReason::Aborted});

    // The session and the server connection stay usable: a later prompt's MCP
    // call completes normally.
    CHECK(session.is_open());
    const auto second = session.prompt_blocking("use the echo tool");
    REQUIRE(second.has_value());
    CHECK(tool_result_text(session, "mcp__echo__echo") == std::optional<std::string>{"second"});
    CHECK_FALSE(tool_result_is_error(session, "mcp__echo__echo"));

    session.close();
}
