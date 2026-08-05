// T12 turn auto-retry evidence (#361): pi's turn auto-retry as
// session-assembly policy — enabled by default with `maxRetries` 3 and
// `baseDelayMs` 2000, exponential backoff, retryability per
// `isRetryableAssistantError` (transient provider/network patterns, excluding
// quota/billing/provider-limit), context overflow routing to compaction and
// never to retry (T10's boundary), the failed assistant message removed from
// live state but retained in session history, an abort-interruptible backoff
// sleep, `auto_retry_start`/`auto_retry_end` events, and re-entry through the
// agent continuation mechanism. The scripted fake `Models` seam serves every
// request; no live keys or network.

#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/coding_agent/AgentSessionEvent.hpp>
#include <cch/coding_agent/Sdk.hpp>
#include <cch/harness/session/JsonlSessionStore.hpp>
#include <cch/util/Error.hpp>
#include <cch/util/JsonValue.hpp>
#include "support/EnvVarGuard.hpp"
#include "support/ModelsFixture.hpp"
#include "support/TempWorkspace.hpp"
#include "util/ExpectedMacros.hpp"
#include "util/Json.hpp"

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] std::string read_fixture_text(std::string_view name) {
    const std::string path = std::string{CCH_SOURCE_DIR} +
                             "/fixtures/pi-agent-core/" +
                             std::string{name};
    std::ifstream input(path, std::ios::binary);
    return std::string{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
}

void expect_json_equal(
    const util::JsonValue& actual,
    std::string_view fixture_name) {
    auto serialized = util::write_json(actual);
    REQUIRE(serialized);
    const auto expected = read_fixture_text(fixture_name);
    if (*serialized != expected) {
        std::cerr << "\n[TurnAutoRetryTest] fixture mismatch: "
                  << fixture_name << "\n--- expected ---\n"
                  << expected << "\n--- actual ---\n"
                  << *serialized << "\n--- end ---\n";
    }
    CHECK(*serialized == expected);
}

struct TestPaths {
    cch::tests::TempWorkspace workspace;
    std::filesystem::path session_file;

    TestPaths() {
        session_file = workspace.path() / "test-session.jsonl";
    }

    void write_settings(std::string json) const {
        workspace.write("agent/settings.json", std::move(json));
    }
};

[[nodiscard]] const tests::EnvVarGuard& agent_dir_guard(const TestPaths& paths) {
    static thread_local tests::EnvVarGuard guard{"PI_CODING_AGENT_DIR"};
    guard.set((paths.workspace.path() / "agent").string());
    return guard;
}

/// FIFO scripted client for the retry lifecycle tests. Records every request,
/// serves queued responses in order, and stamps every assistant message with
/// the same deterministic identity/timestamp the other SDK fake clients use.
class RetryScriptedProvider final : public tests::ScriptedProvider {
public:
    RetryScriptedProvider() : ScriptedProvider("sdk-host") {}

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink sink) override {
        ++request_count;
        requests.push_back(tests::RecordedProviderRequest{model, context, options});
        if (options.stop_token.stop_requested()) {
            auto terminal = ai::assistant_text_message("");
            terminal.stop_reason = ai::AssistantStopReason::Aborted;
            terminal.error_message = "Request was aborted";
            terminal.provider = "sdk-host";
            terminal.api = "fake";
            terminal.model = model.id;
            terminal.timestamp = 1718000000123;
            if (sink) {
                CCH_TRY_VOID(sink(ai::AssistantErrorEvent{
                    .reason = terminal.stop_reason,
                    .error = terminal,
                    .failure = util::make_error(
                        util::ErrorCode::Cancelled, "Request was aborted"),
                }));
            }
            co_return terminal;
        }
        if (responses.empty()) {
            co_return ai::assistant_text_message("default fake response");
        }
        auto response = std::move(responses.front());
        responses.pop_front();
        response.provider = "sdk-host";
        response.api = "fake";
        response.model = model.id;
        response.timestamp = 1718000000123;
        if (sink) {
            if (response.stop_reason == ai::AssistantStopReason::Error ||
                response.stop_reason == ai::AssistantStopReason::Aborted) {
                // Terminal-before-start: no assistant start event; the loop
                // synthesizes one from the authoritative final message.
                CCH_TRY_VOID(sink(ai::AssistantErrorEvent{
                    .reason = response.stop_reason,
                    .error = response,
                    .failure = util::make_error(
                        util::ErrorCode::Stream,
                        response.error_message.value_or("terminal error")),
                }));
            } else {
                CCH_TRY_VOID(sink(ai::AssistantStartEvent{response}));
            }
        }
        co_return response;
    }

    int request_count{0};
    std::vector<tests::RecordedProviderRequest> requests;
    std::deque<ai::AssistantMessage> responses;
};

/// An `error` terminal with the given provider message (the classification
/// input for `isRetryableAssistantError`).
[[nodiscard]] ai::AssistantMessage error_terminal(std::string message) {
    auto terminal = ai::assistant_text_message("");
    terminal.stop_reason = ai::AssistantStopReason::Error;
    terminal.error_message = std::move(message);
    return terminal;
}

[[nodiscard]] ai::AssistantMessage success_message(std::string text) {
    return ai::assistant_text_message(std::move(text));
}

/// The context-overflow error terminal providers return when input exceeds
/// the context window (a pattern from pi's OVERFLOW_PATTERNS).
[[nodiscard]] ai::AssistantMessage overflow_terminal() {
    return error_terminal(
        "Your input exceeds the context window of this model");
}

/// A minimal fake tool for retry-continuation tool-loop evidence (same shape
/// as SdkSessionTest's FakeEchoTool).
class FakeEchoTool final : public agent::AsyncAgentTool {
public:
    explicit FakeEchoTool(std::shared_ptr<std::size_t> execution_count)
        : execution_count_(std::move(execution_count)) {
        def_.name = "echo";
        def_.description = "Echo back the input";
        def_.parameters = util::JsonValue::object_t{
            {"type", "object"},
            {"additionalProperties", false}};
    }

    [[nodiscard]] const ai::Tool& definition() const override { return def_; }

    [[nodiscard]] boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> execute(
        agent::ToolInvocation /*invocation*/,
        std::stop_token) override {
        if (execution_count_) {
            ++*execution_count_;
        }
        agent::AsyncToolExecutionResult result;
        result.content.push_back(ai::text_content("echo: ok"));
        co_return result;
    }

private:
    ai::Tool def_;
    std::shared_ptr<std::size_t> execution_count_;
};

struct RetrySessionUnderTest {
    std::unique_ptr<coding_agent::AgentSession> session;
    RetryScriptedProvider* client{nullptr};
};

[[nodiscard]] RetrySessionUnderTest make_retry_session(
    const TestPaths& paths,
    std::deque<ai::AssistantMessage> responses,
    std::string settings_json = {},
    std::vector<std::unique_ptr<agent::AsyncAgentTool>> custom_tools = {}) {
    // Every retry test isolates its settings scope under a fresh agent
    // directory: an empty dir keeps pi's defaults, a test-provided
    // settings.json drives the knobs. The guard lives through session
    // creation, when the SettingsManager snapshot is read.
    const tests::EnvVarGuard agent_dir{
        "PI_CODING_AGENT_DIR",
        (paths.workspace.path() / "agent").string()};
    if (!settings_json.empty()) {
        paths.write_settings(settings_json);
    }
    auto client = std::make_shared<RetryScriptedProvider>();
    auto* client_ptr = client.get();
    client_ptr->responses = std::move(responses);

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    options.workspace = paths.workspace.path();
    options.model = tests::sdk_request_model("sdk-host", "gpt-test");
    options.custom_tools = std::move(custom_tools);
    options.models = cch::tests::models_from_provider(std::move(client));

    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created.has_value());
    return RetrySessionUnderTest{
        std::move(created->session),
        client_ptr,
    };
}

/// Session events observed by a `subscribe_session` sink, reduced to pi's
/// `start:ATTEMPT` / `end:success=BOOL` shape (mirroring pi's retry tests).
struct RecordedSessionEvents {
    std::vector<coding_agent::AutoRetryStartEvent> starts;
    std::vector<coding_agent::AutoRetryEndEvent> ends;

    [[nodiscard]] std::vector<std::string> reduced() const {
        std::vector<std::string> out;
        for (const auto& start : starts) {
            out.push_back("start:" + std::to_string(start.attempt));
        }
        for (const auto& end : ends) {
            out.push_back("end:success=" + std::string{end.success ? "true" : "false"});
        }
        return out;
    }
};

[[nodiscard]] coding_agent::SessionEventSubscription subscribe_events(
    coding_agent::AgentSession& session,
    RecordedSessionEvents& recorded) {
    auto subscription = session.subscribe_session(
        [&recorded](const coding_agent::AgentSessionEvent& event)
            -> util::ExpectedVoid {
            if (const auto* start =
                    std::get_if<coding_agent::AutoRetryStartEvent>(&event)) {
                recorded.starts.push_back(*start);
            } else if (const auto* end =
                           std::get_if<coding_agent::AutoRetryEndEvent>(&event)) {
                recorded.ends.push_back(*end);
            }
            return {};
        });
    REQUIRE(subscription.has_value());
    return std::move(*subscription);
}

} // namespace

TEST_CASE(
    "turn auto-retry retries a transient error and succeeds, emitting start and end events",
    "[coding_agent][retry][issue361]") {
    TestPaths paths;
    // baseDelayMs 1 keeps the test deterministic and fast; pi's own retry
    // tests use the same override.
    auto under_test = make_retry_session(
        paths,
        {
            error_terminal("overloaded_error"),
            success_message("Success after retry"),
        },
        R"({"retry": {"enabled": true, "maxRetries": 3, "baseDelayMs": 1}})");
    auto* session = under_test.session.get();
    auto* client = under_test.client;

    RecordedSessionEvents events;
    auto subscription = subscribe_events(*session, events);

    REQUIRE(session->prompt_blocking("Test").has_value());

    // pi agent-session-retry.test.ts "retries after a transient error and
    // succeeds": exactly one retry, events ["start:1", "end:success=true"].
    CHECK(client->request_count == 2);
    CHECK(events.reduced() ==
          (std::vector<std::string>{"start:1", "end:success=true"}));
    CHECK(events.starts[0].max_attempts == 3);

    // The retried run completed with the successful message.
    const auto snapshot = session->snapshot();
    const auto* last =
        std::get_if<ai::AssistantMessage>(&snapshot.agent_state.messages.back());
    REQUIRE(last != nullptr);
    CHECK(last->stop_reason == ai::AssistantStopReason::Stop);
    CHECK(ai::text_from_assistant_content(last->content) == "Success after retry");

    session->close();
}

TEST_CASE(
    "turn auto-retry exhausts max retries and emits the final failure event",
    "[coding_agent][retry][issue361]") {
    TestPaths paths;
    auto under_test = make_retry_session(
        paths,
        {
            error_terminal("overloaded_error"),
            error_terminal("overloaded_error"),
            error_terminal("overloaded_error"),
        },
        R"({"retry": {"enabled": true, "maxRetries": 2, "baseDelayMs": 1}})");
    auto* session = under_test.session.get();
    auto* client = under_test.client;

    RecordedSessionEvents events;
    auto subscription = subscribe_events(*session, events);

    // Exhaustion completes normally: the final error message is the run's
    // outcome (pi's `_handlePostAgentRun` failure branch emits the end event).
    REQUIRE(session->prompt_blocking("Test").has_value());

    // pi agent-session-retry.test.ts "exhausts max retries and emits
    // failure": initial call + 2 retries, start:1, start:2, end:success=false.
    CHECK(client->request_count == 3);
    const auto reduced = events.reduced();
    REQUIRE(reduced.size() == 3);
    CHECK(reduced[0] == "start:1");
    CHECK(reduced[1] == "start:2");
    CHECK(reduced[2] == "end:success=false");
    CHECK(events.ends[0].attempt == 2);
    CHECK(events.ends[0].final_error.has_value());
    CHECK(*events.ends[0].final_error == "overloaded_error");

    // The final error message remains the last assistant message in live
    // state after exhaustion (no continuation starts).
    const auto snapshot = session->snapshot();
    const auto* last =
        std::get_if<ai::AssistantMessage>(&snapshot.agent_state.messages.back());
    REQUIRE(last != nullptr);
    CHECK(last->stop_reason == ai::AssistantStopReason::Error);

    session->close();
}

TEST_CASE(
    "turn auto-retry uses pi defaults: enabled, maxRetries 3, baseDelayMs 2000, exponential backoff",
    "[coding_agent][retry][issue361]") {
    TestPaths paths;
    // One retry on the default settings: the first `auto_retry_start` must
    // carry pi's `maxAttempts` 3 and `delayMs = 2000 * 2^0`.
    auto under_test = make_retry_session(
        paths,
        {
            error_terminal("overloaded_error"),
            success_message("recovered"),
        });
    auto* session = under_test.session.get();

    RecordedSessionEvents events;
    auto subscription = subscribe_events(*session, events);

    REQUIRE(session->prompt_blocking("Test").has_value());
    REQUIRE(events.starts.size() == 1);
    CHECK(events.starts[0].max_attempts == 3);
    CHECK(events.starts[0].delay_ms == 2000);
    CHECK(events.starts[0].error_message == "overloaded_error");
    CHECK(events.ends.size() == 1);
    CHECK(events.ends[0].success);

    // Exponential backoff pinned deterministically through a second scenario
    // (baseDelayMs 2 → delays 2, 4, 8 on attempts 1..3).
    TestPaths exp_paths;
    auto exp_under_test = make_retry_session(
        exp_paths,
        {
            error_terminal("overloaded_error"),
            error_terminal("overloaded_error"),
            error_terminal("overloaded_error"),
            error_terminal("overloaded_error"),
        },
        R"({"retry": {"enabled": true, "maxRetries": 3, "baseDelayMs": 2}})");
    auto* exp_session = exp_under_test.session.get();
    RecordedSessionEvents exp_events;
    auto exp_subscription = subscribe_events(*exp_session, exp_events);
    REQUIRE(exp_session->prompt_blocking("Test").has_value());
    REQUIRE(exp_events.starts.size() == 3);
    CHECK(exp_events.starts[0].delay_ms == 2);
    CHECK(exp_events.starts[1].delay_ms == 4);
    CHECK(exp_events.starts[2].delay_ms == 8);

    session->close();
    exp_session->close();
}

TEST_CASE(
    "turn auto-retry retryability follows isRetryableAssistantError: transient patterns retry, quota never",
    "[coding_agent][retry][issue361]") {
    // A network/transport pattern retries (pi "retries provider network_error
    // failures").
    TestPaths network_paths;
    auto network_under_test = make_retry_session(
        network_paths,
        {
            error_terminal("Provider finish_reason: network_error"),
            success_message("Recovered after retry"),
        },
        R"({"retry": {"enabled": true, "maxRetries": 3, "baseDelayMs": 1}})");
    auto* network_session = network_under_test.session.get();
    auto* network_client = network_under_test.client;
    REQUIRE(network_session->prompt_blocking("Test").has_value());
    CHECK(network_client->request_count == 2);
    network_session->close();

    // Quota/billing/provider-limit patterns never retry: the error terminal
    // is the run's outcome with no session events.
    for (const std::string& message :
         {"insufficient_quota", "quota exceeded", "Monthly usage limit reached",
          "You have exceeded your available balance", "billing_error",
          "FreeUsageLimitError"}) {
        TestPaths paths;
        auto under_test = make_retry_session(
            paths,
            {error_terminal(message)},
            R"({"retry": {"enabled": true, "maxRetries": 3, "baseDelayMs": 1}})");
        auto* session = under_test.session.get();
        auto* client = under_test.client;
        RecordedSessionEvents events;
        auto subscription = subscribe_events(*session, events);
        REQUIRE(session->prompt_blocking("Test").has_value());
        CHECK(client->request_count == 1);
        CHECK(events.reduced().empty());
        CHECK(session->message_count() == 2);
        session->close();
    }
}

TEST_CASE(
    "context overflow routes to compaction and never enters the retry path",
    "[coding_agent][retry][issue361]") {
    TestPaths paths;
    auto under_test = make_retry_session(
        paths,
        {
            overflow_terminal(),
            // A retry-capable transient failure must not fire after the
            // overflow: the overflow error completes the run (the compaction
            // auto-trigger needs a summarizable history and silently skips a
            // too-small session, exactly like pi's `prepareCompaction`
            // returning undefined).
            error_terminal("overloaded_error"),
        },
        R"({"retry": {"enabled": true, "maxRetries": 3, "baseDelayMs": 1}})");
    auto* session = under_test.session.get();
    auto* client = under_test.client;

    RecordedSessionEvents events;
    auto subscription = subscribe_events(*session, events);

    REQUIRE(session->prompt_blocking("Test").has_value());

    // Exactly one model call: the overflow error was never retried and no
    // auto_retry events fired — the retry path excludes context overflow
    // (pi `_isRetryableError`), so the two recovery paths never interfere.
    CHECK(client->request_count == 1);
    CHECK(events.reduced().empty());

    // The overflow error is removed from live state for the compact-and-retry
    // (pi pops it before `_runAutoCompaction`); a session too small to
    // summarize skips compaction silently and the run completes with the
    // error only in session history.
    CHECK(session->message_count() == 1);
    const auto snapshot = session->snapshot();
    REQUIRE(snapshot.agent_state.messages.size() == 1);
    CHECK(std::holds_alternative<ai::UserMessage>(
        snapshot.agent_state.messages[0]));

    // The overflow error is retained in session history.
    auto loaded = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(loaded.has_value());
    bool found_overflow = false;
    for (const auto& entry : loaded->entries) {
        if (entry.kind != harness::session::SessionEntryKind::Message ||
            !entry.message) {
            continue;
        }
        const auto* assistant =
            std::get_if<ai::AssistantMessage>(&*entry.message);
        if (assistant != nullptr &&
            assistant->stop_reason == ai::AssistantStopReason::Error &&
            assistant->error_message &&
            assistant->error_message->find("exceeds the context window") !=
                std::string::npos) {
            found_overflow = true;
        }
    }
    CHECK(found_overflow);

    session->close();
}

TEST_CASE(
    "the failed assistant message is removed from live state but retained in session history",
    "[coding_agent][retry][issue361]") {
    TestPaths paths;
    auto under_test = make_retry_session(
        paths,
        {
            error_terminal("overloaded_error"),
            success_message("recovered"),
        },
        R"({"retry": {"enabled": true, "maxRetries": 3, "baseDelayMs": 1}})");
    auto* session = under_test.session.get();
    auto* client = under_test.client;

    REQUIRE(session->prompt_blocking("Test").has_value());

    // Live state: [user, assistant(recovered)] — the failed error message is
    // gone so the continuation's last message is the user prompt.
    const auto snapshot = session->snapshot();
    REQUIRE(snapshot.agent_state.messages.size() == 2);
    CHECK(std::holds_alternative<ai::UserMessage>(
        snapshot.agent_state.messages[0]));
    const auto* last = std::get_if<ai::AssistantMessage>(
        &snapshot.agent_state.messages[1]);
    REQUIRE(last != nullptr);
    CHECK(ai::text_from_assistant_content(last->content) == "recovered");

    // The retry request at the stream seam re-enters through the agent
    // continuation: its model context is [user, ...] with no error message,
    // exactly what pi's model sees after `_prepareRetry` popped it.
    REQUIRE(client->requests.size() == 2);
    const auto& retry_context = client->requests[1].context.messages;
    for (const auto& message : retry_context) {
        const auto* assistant =
            std::get_if<ai::AssistantMessage>(&message);
        if (assistant != nullptr) {
            CHECK(assistant->stop_reason != ai::AssistantStopReason::Error);
        }
    }
    CHECK(std::holds_alternative<ai::UserMessage>(retry_context.front()));

    // Session history: user + error + recovered — the failed message was
    // persisted before the retry (pi `_prepareRetry` keeps it in session).
    auto loaded = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(loaded.has_value());
    std::size_t message_entries = 0;
    std::size_t error_entries = 0;
    for (const auto& entry : loaded->entries) {
        if (entry.kind != harness::session::SessionEntryKind::Message ||
            !entry.message) {
            continue;
        }
        ++message_entries;
        const auto* assistant =
            std::get_if<ai::AssistantMessage>(&*entry.message);
        if (assistant != nullptr &&
            assistant->stop_reason == ai::AssistantStopReason::Error &&
            assistant->error_message &&
            *assistant->error_message == "overloaded_error") {
            ++error_entries;
        }
    }
    CHECK(message_entries == 3);
    CHECK(error_entries == 1);

    session->close();
}

TEST_CASE(
    "abort during the backoff sleep cancels the retry with exactly one auto_retry_end",
    "[coding_agent][retry][issue361]") {
    TestPaths paths;
    // A long backoff so the abort deterministically lands inside the sleep.
    auto under_test = make_retry_session(
        paths,
        {
            error_terminal("overloaded_error"),
            success_message("never reached"),
        },
        R"({"retry": {"enabled": true, "maxRetries": 3, "baseDelayMs": 60000}})");
    auto* session = under_test.session.get();
    auto* client = under_test.client;

    RecordedSessionEvents events;
    auto subscription = subscribe_events(*session, events);
    std::atomic<bool> retry_started{false};
    auto retry_observer = session->subscribe_session(
        [&retry_started](const coding_agent::AgentSessionEvent& event)
            -> util::ExpectedVoid {
            if (std::holds_alternative<coding_agent::AutoRetryStartEvent>(
                    event)) {
                retry_started.store(true);
            }
            return {};
        });
    REQUIRE(retry_observer.has_value());

    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> prompt_result;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            prompt_result = co_await session->prompt("Test");
            co_return;
        },
        boost::asio::detached);
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            // Wait until the backoff sleep is running, then abort it.
            boost::asio::steady_timer tick(
                co_await boost::asio::this_coro::executor);
            while (!retry_started.load()) {
                tick.expires_after(std::chrono::milliseconds(1));
                co_await tick.async_wait(boost::asio::use_awaitable);
            }
            session->abort();
            co_return;
        },
        boost::asio::detached);
    io.run();

    // The prompt completes normally with the error terminal; the retry never
    // started (exactly one model call) and the backoff produced exactly one
    // `auto_retry_end` failure ("Retry cancelled").
    REQUIRE(prompt_result.has_value());
    REQUIRE(prompt_result->has_value());
    CHECK(client->request_count == 1);
    CHECK(events.starts.size() == 1);
    REQUIRE(events.ends.size() == 1);
    CHECK_FALSE(events.ends[0].success);
    CHECK(events.ends[0].attempt == 1);
    REQUIRE(events.ends[0].final_error.has_value());
    CHECK(*events.ends[0].final_error == "Retry cancelled");

    // The failed message was removed from live state for the retry (pi pops
    // it before the backoff and does not restore it on cancellation); it is
    // retained in session history.
    const auto snapshot = session->snapshot();
    REQUIRE(snapshot.agent_state.messages.size() == 1);
    CHECK(std::holds_alternative<ai::UserMessage>(
        snapshot.agent_state.messages[0]));
    auto loaded = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(loaded.has_value());
    bool found_error = false;
    for (const auto& entry : loaded->entries) {
        if (entry.kind != harness::session::SessionEntryKind::Message ||
            !entry.message) {
            continue;
        }
        const auto* assistant =
            std::get_if<ai::AssistantMessage>(&*entry.message);
        if (assistant != nullptr &&
            assistant->stop_reason == ai::AssistantStopReason::Error &&
            assistant->error_message &&
            *assistant->error_message == "overloaded_error") {
            found_error = true;
        }
    }
    CHECK(found_error);

    // The session stays reusable for the next prompt.
    CHECK_FALSE(session->is_busy());
    REQUIRE(session->prompt_blocking("Follow-up").has_value());
    CHECK(client->request_count == 2);

    session->close();
}

TEST_CASE(
    "disabled retry settings suppress turn auto-retry entirely",
    "[coding_agent][retry][issue361]") {
    TestPaths paths;
    auto under_test = make_retry_session(
        paths,
        {
            error_terminal("overloaded_error"),
            success_message("never reached"),
        },
        R"({"retry": {"enabled": false, "maxRetries": 3, "baseDelayMs": 1}})");
    auto* session = under_test.session.get();
    auto* client = under_test.client;

    RecordedSessionEvents events;
    auto subscription = subscribe_events(*session, events);

    REQUIRE(session->prompt_blocking("Test").has_value());
    CHECK(client->request_count == 1);
    CHECK(events.reduced().empty());
    session->close();
}

TEST_CASE(
    "turn auto-retry lifecycle golden matches pi's retry event sequence",
    "[coding_agent][retry][issue361][golden]") {
    TestPaths paths;
    auto under_test = make_retry_session(
        paths,
        {
            error_terminal("overloaded_error"),
            success_message("Success after retry"),
        },
        R"({"retry": {"enabled": true, "maxRetries": 3, "baseDelayMs": 1}})");
    auto* session = under_test.session.get();
    auto* client = under_test.client;

    RecordedSessionEvents events;
    auto subscription = subscribe_events(*session, events);
    REQUIRE(session->prompt_blocking("Test").has_value());

    util::JsonValue::object_t record;
    util::JsonValue::array_t event_records;
    for (const auto& start : events.starts) {
        util::JsonValue::object_t event;
        event.emplace("type", util::JsonValue{"auto_retry_start"});
        event.emplace("attempt", util::JsonValue{start.attempt});
        event.emplace("maxAttempts", util::JsonValue{start.max_attempts});
        event.emplace("delayMs", util::JsonValue{static_cast<int>(start.delay_ms)});
        event.emplace("errorMessage", util::JsonValue{start.error_message});
        event_records.push_back(util::JsonValue{std::move(event)});
    }
    for (const auto& end : events.ends) {
        util::JsonValue::object_t event;
        event.emplace("type", util::JsonValue{"auto_retry_end"});
        event.emplace("success", util::JsonValue{end.success});
        event.emplace("attempt", util::JsonValue{end.attempt});
        if (end.final_error) {
            event.emplace(
                "finalError", util::JsonValue{*end.final_error});
        }
        event_records.push_back(util::JsonValue{std::move(event)});
    }
    record.emplace("events", util::JsonValue{std::move(event_records)});
    record.emplace("modelRequests", util::JsonValue{client->request_count});

    expect_json_equal(util::JsonValue{std::move(record)}, "auto-retry-lifecycle.json");

    session->close();
}

TEST_CASE(
    "the retry continuation runs the full agent loop when it produces tool calls",
    "[coding_agent][retry][issue361]") {
    // pi agent-session-retry.test.ts "prompt waits for full agent loop when
    // retry produces tool calls": after the retry response includes a tool
    // call, session.prompt() must wait for the entire tool loop and the
    // session must accept a follow-up prompt afterwards.
    TestPaths paths;
    auto tool_execution_count = std::make_shared<std::size_t>(0);
    auto tool_use = ai::assistant_text_message("Looking that up now.");
    tool_use.stop_reason = ai::AssistantStopReason::ToolUse;
    tool_use.content.emplace_back(ai::ToolCallContent{
        .id = "call_1",
        .name = "echo",
        .arguments = util::JsonValue{util::JsonValue::object_t{}},
        .raw_arguments = {},
        .thought_signature = std::nullopt,
        .arguments_valid = true,
        .argument_error = std::nullopt,
    });
    std::vector<std::unique_ptr<agent::AsyncAgentTool>> tools;
    tools.push_back(std::make_unique<FakeEchoTool>(tool_execution_count));
    auto under_test = make_retry_session(
        paths,
        {
            error_terminal("overloaded_error"),
            std::move(tool_use),
            success_message("Final answer."),
        },
        R"({"retry": {"enabled": true, "maxRetries": 3, "baseDelayMs": 1}})",
        std::move(tools));
    auto* session = under_test.session.get();
    auto* client = under_test.client;

    RecordedSessionEvents events;
    auto subscription = subscribe_events(*session, events);

    REQUIRE(session->prompt_blocking("Test").has_value());

    // All three LLM calls completed (failed turn + retry tool turn + final
    // answer), the tool executed, and the session is not busy afterwards.
    CHECK(client->request_count == 3);
    CHECK(*tool_execution_count == 1);
    CHECK_FALSE(session->is_busy());
    CHECK(events.reduced() ==
          (std::vector<std::string>{"start:1", "end:success=true"}));

    // A follow-up prompt works (no "already processing" rejection).
    REQUIRE(session->prompt_blocking("Follow-up").has_value());
    CHECK(client->request_count == 4);

    session->close();
}
