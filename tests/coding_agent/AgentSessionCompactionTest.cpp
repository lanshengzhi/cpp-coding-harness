// Manual compaction trigger evidence for #358 (T09): the session-assembly
// trigger (pi `AgentSession.compact`) driving the harness compaction machinery
// through the scripted fake `Models` seam — abort-in-flight and idle cases,
// `CompactionEntry` persistence, and context rebuild as compactionSummary +
// retained tail. No live keys or network: every request is served by the
// scripted chat client below.

#include "ai/ModelStreamBridge.hpp"
#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include "coding_agent/AgentSession.hpp"
#include <cch/agent/harness/session/SessionStore.hpp>
#include <cch/support/Error.hpp>
#include "support/EnvVarGuard.hpp"
#include "support/ModelsFixture.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"
#include "support/ExpectedMacros.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
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
using tests::run_awaitable;

namespace {

struct TestPaths {
    cch::tests::TempWorkspace workspace;
    std::filesystem::path session_file;

    TestPaths() {
        session_file = workspace.path() / "test-session.jsonl";
    }
};

[[nodiscard]] ai::Usage big_usage() {
    ai::Usage usage;
    usage.input = 5000;
    usage.output = 1000;
    usage.cache_read = 0;
    usage.cache_write = 0;
    usage.total_tokens = 6000;
    return usage;
}

[[nodiscard]] ai::AssistantMessage big_assistant(std::string text) {
    auto message = ai::assistant_text_message(std::move(text));
    message.usage = big_usage();
    return message;
}

[[nodiscard]] ai::AssistantMessage summarization_response() {
    auto summary = ai::assistant_text_message("## Goal\nCompacted history summary");
    summary.usage = big_usage();
    return summary;
}

/// FIFO scripted chat client for the compaction trigger tests. Records every
/// request (for context-rebuild assertions), serves queued responses in order,
/// and optionally gates one request until `release_gate()` — a stopped request
/// completes with exactly one aborted terminal event plus the agreeing final
/// AssistantMessage (the #326 terminal contract).
class CompactionScriptedProvider final : public tests::ScriptedProvider {
public:
    CompactionScriptedProvider() : ScriptedProvider("sdk-host") {}

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext context, coding_agent::ModelRuntimeTestStreamOptions options) override {
        return ai::detail::make_model_stream(
            [this, model = std::move(model), context = std::move(context), options = std::move(options)](
                ai::AssistantEventSink sink) mutable
                -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
        ++request_count;
        requests.push_back(tests::RecordedProviderRequest{model, context, options});
        if (gate_request_number &&
            request_count == *gate_request_number) {
            gate_.emplace(co_await boost::asio::this_coro::executor);
            gate_->expires_at(std::chrono::steady_clock::time_point::max());
            boost::system::error_code error;
            co_await gate_->async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, error));
        }
        if (options.stop_token.stop_requested()) {
            auto terminal = ai::assistant_text_message(aborted_content);
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
                    .failure = support::make_error(
                        support::ErrorCode::Cancelled, "Request was aborted"),
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
        // Session files require real epoch timestamps on assistant messages;
        // stamp the same deterministic value the other SDK fake clients use.
        response.timestamp = 1718000000123;
        if (sink) {
            CCH_TRY_VOID(sink(ai::AssistantStartEvent{response}));
        }
        co_return response;
                });
    }

    void release_gate() {
        if (gate_) {
            gate_->cancel();
        }
    }

    int request_count{0};
    /// When set, request number N blocks until released.
    std::optional<int> gate_request_number;
    /// Content for an aborted terminal response (deterministic partial text).
    std::string aborted_content;
    std::vector<tests::RecordedProviderRequest> requests;
    std::deque<ai::AssistantMessage> responses;
    std::optional<boost::asio::steady_timer> gate_;
};

/// A persisted session whose live history reaches the 20000-token
/// keepRecentTokens budget with a deterministic cut: three 20K-char
/// user/assistant pairs (each ~5001 estimated tokens). The cut keeps
/// [u2, a2, u3, a3] and summarizes [u1, a1]. Returns the created session.
struct SessionUnderTest {
    std::unique_ptr<coding_agent::AgentSession> session;
    CompactionScriptedProvider* client{nullptr};
};

[[nodiscard]] SessionUnderTest make_big_session(const TestPaths& paths, tests::RuntimeFixture& runtime) {
    const std::string big(20000, 'x');
    auto client = std::make_shared<CompactionScriptedProvider>();
    auto* client_ptr = client.get();
    client_ptr->responses.push_back(big_assistant("a1 " + big));
    client_ptr->responses.push_back(big_assistant("a2 " + big));
    client_ptr->responses.push_back(big_assistant("a3 " + big));

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::ExplicitOpenOrCreateSessionTarget{paths.session_file};
    options.workspace = paths.workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));

    auto models = std::move(options.models);
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    request.execution_runtime_target = runtime.make_target();
    auto created = runtime.run(coding_agent::create_agent_session_async(std::move(request),
            std::nullopt,
            coding_agent::runtime::AssemblyOverrides{
                    .model_runtime = nullptr, .models = std::move(models), .user_shell = nullptr}));
    REQUIRE(created.has_value());
    auto* session = created->session.get();
    REQUIRE(run_awaitable(runtime, session->prompt(big + " u1")).has_value());
    REQUIRE(run_awaitable(runtime, session->prompt(big + " u2")).has_value());
    REQUIRE(run_awaitable(runtime, session->prompt(big + " u3")).has_value());
    CHECK(session->message_count() == 6);
    return SessionUnderTest{
        std::move(created->session),
        client_ptr,
    };
}

[[nodiscard]] std::optional<harness::session::CompactionEntryValue> find_compaction_entry(
    const TestPaths& paths) {
    auto loaded = harness::session::SessionStore::load(paths.session_file);
    REQUIRE(loaded.has_value());
    for (const auto& entry : loaded->entries) {
        if (entry.kind == harness::session::SessionEntryKind::Compaction) {
            return std::get<harness::session::CompactionEntryValue>(entry.value);
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE(
    "manual compaction on an idle session persists a CompactionEntry and rebuilds context",
    "[coding_agent][compaction][issue358]") {
    TestPaths paths;
    tests::RuntimeFixture runtime;
    auto under_test = make_big_session(paths, runtime);
    auto* session = under_test.session.get();
    auto* client = under_test.client;
    client->responses.push_back(summarization_response());
    client->responses.push_back(ai::assistant_text_message("after compaction"));

    auto result = run_awaitable(runtime, session->compact());
    REQUIRE(result.has_value());
    CHECK(result->summary.find("## Goal\nCompacted history summary") != std::string::npos);
    CHECK_FALSE(result->first_kept_entry_id.empty());
    CHECK(result->tokens_before > 0);
    REQUIRE(result->estimated_tokens_after.has_value());

    // A compaction entry persisted with pi's field set (summary,
    // firstKeptEntryId, tokensBefore, retainedTail, details, usage;
    // fromHook absent/omitted for machinery-generated compactions).
    const auto value = find_compaction_entry(paths);
    REQUIRE(value.has_value());
    CHECK(value->summary == result->summary);
    CHECK(value->first_kept_entry_id == result->first_kept_entry_id);
    CHECK(value->tokens_before == result->tokens_before);
    REQUIRE(value->retained_tail.has_value());
    CHECK(value->retained_tail->size() == 4);
    REQUIRE(value->details.has_value());
    REQUIRE(value->usage.has_value());
    CHECK_FALSE(value->from_hook.value_or(false));

    // Live context rebuilt as compactionSummary + retained tail.
    const auto snapshot = session->snapshot();
    REQUIRE(snapshot.agent_state.messages.size() == 5);
    CHECK(std::holds_alternative<ai::CompactionSummaryMessage>(
        snapshot.agent_state.messages[0]));
    CHECK(std::holds_alternative<ai::UserMessage>(snapshot.agent_state.messages[1]));
    CHECK(std::holds_alternative<ai::AssistantMessage>(snapshot.agent_state.messages[2]));
    CHECK(std::holds_alternative<ai::UserMessage>(snapshot.agent_state.messages[3]));
    CHECK(std::holds_alternative<ai::AssistantMessage>(snapshot.agent_state.messages[4]));
    const auto* summary_message =
        std::get_if<ai::CompactionSummaryMessage>(&snapshot.agent_state.messages[0]);
    REQUIRE(summary_message != nullptr);
    CHECK(summary_message->summary == result->summary);
    CHECK(summary_message->tokens_before == static_cast<std::int64_t>(result->tokens_before));

    // The summarization request hit the stream seam: recorded with the
    // summarization system prompt and a single user prompt message.
    REQUIRE(client->requests.size() == 4);
    const auto& summarization = client->requests[3];
    REQUIRE(summarization.context.system_prompt.has_value());
    CHECK(summarization.context.system_prompt->find(
              "context summarization assistant") != std::string::npos);
    CHECK(summarization.context.messages.size() == 1);

    // The next prompt's model context is exactly compactionSummary + retained
    // tail (pi: `agent.state.messages = sessionContext.messages`).
    REQUIRE(run_awaitable(runtime, session->prompt("after compaction")).has_value());
    REQUIRE(client->requests.size() == 5);
    const auto& next_request = client->requests[4];
    REQUIRE(next_request.context.messages.size() == 6);
    CHECK(std::holds_alternative<ai::CompactionSummaryMessage>(
        next_request.context.messages[0]));
    CHECK(std::holds_alternative<ai::AssistantMessage>(
        next_request.context.messages[4]));
    CHECK(std::holds_alternative<ai::UserMessage>(
        next_request.context.messages[5]));
    const auto* seen_summary =
        std::get_if<ai::CompactionSummaryMessage>(&next_request.context.messages[0]);
    REQUIRE(seen_summary != nullptr);
    CHECK(seen_summary->summary == result->summary);

    session->close();
}

TEST_CASE(
    "manual compaction aborts an in-flight run before compacting",
    "[coding_agent][compaction][issue358]") {
    TestPaths paths;
    tests::RuntimeFixture runtime;
    const std::string big(20000, 'x');
    auto client = std::make_shared<CompactionScriptedProvider>();
    auto* client_ptr = client.get();
    client_ptr->responses.push_back(big_assistant("a1 " + big));
    client_ptr->responses.push_back(big_assistant("a2 " + big));
    client_ptr->responses.push_back(summarization_response());
    client_ptr->responses.push_back(ai::assistant_text_message("after compaction"));
    // The third prompt (in flight when compact() is called) is gated; its
    // aborted terminal carries deterministic large partial content so the
    // compaction cut matches the idle case (keeps [u2, a2, u3, a3]).
    client_ptr->gate_request_number = 3;
    client_ptr->aborted_content = "a3 " + big;

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::ExplicitOpenOrCreateSessionTarget{paths.session_file};
    options.workspace = paths.workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));

    auto models = std::move(options.models);
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    request.execution_runtime_target = runtime.make_target();
    auto created = runtime.run(coding_agent::create_agent_session_async(std::move(request),
            std::nullopt,
            coding_agent::runtime::AssemblyOverrides{
                    .model_runtime = nullptr, .models = std::move(models), .user_shell = nullptr}));
    REQUIRE(created.has_value());
    auto* session = created->session.get();
    REQUIRE(run_awaitable(runtime, session->prompt(big + " u1")).has_value());
    REQUIRE(run_awaitable(runtime, session->prompt(big + " u2")).has_value());

    std::optional<support::ExpectedVoid> prompt_result;
    std::optional<support::Expected<coding_agent::CompactionResult>> compact_result;
    auto combined =
            runtime.run(support::detail::make_async_result([&]() -> boost::asio::awaitable<support::ExpectedVoid> {
                const auto executor = co_await boost::asio::this_coro::executor;
                boost::asio::co_spawn(
                        executor,
                        [&]() -> boost::asio::awaitable<void> {
                            prompt_result = co_await session->prompt(big + " u3");
                            co_return;
                        },
                        boost::asio::detached);
                while (client_ptr->request_count < 3) {
                    boost::asio::steady_timer yield_timer{executor};
                    yield_timer.expires_after(std::chrono::milliseconds{1});
                    boost::system::error_code error;
                    co_await yield_timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
                }

                boost::asio::co_spawn(
                        executor,
                        [&]() -> boost::asio::awaitable<void> {
                            compact_result = co_await session->compact();
                            co_return;
                        },
                        boost::asio::detached);

                // Release the in-flight prompt: it observes the cancellation
                // requested by compact() and settles with the ordinary aborted
                // terminal.
                client_ptr->release_gate();
                while (!prompt_result || !compact_result) {
                    boost::asio::steady_timer yield_timer{executor};
                    yield_timer.expires_after(std::chrono::milliseconds{1});
                    boost::system::error_code error;
                    co_await yield_timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
                }
                co_return support::ExpectedVoid{};
            }));
    REQUIRE(combined.has_value());

    // The aborted prompt completes normally; compaction succeeded and used
    // the aborted run's committed history.
    REQUIRE(prompt_result.has_value());
    REQUIRE(prompt_result->has_value());
    REQUIRE(compact_result.has_value());
    REQUIRE(compact_result->has_value());
    CHECK((*compact_result)->summary.find("## Goal\nCompacted history summary") !=
          std::string::npos);

    // The in-flight prompt request observed the abort.
    REQUIRE(client_ptr->requests.size() >= 3);
    CHECK(client_ptr->requests[2].options.stop_token.stop_requested());
    CHECK_FALSE(session->is_busy());
    CHECK(session->message_count() == 5);

    // The aborted run's assistant message was committed, and compaction
    // persisted over it (six entries before compaction → entry after).
    const auto value = find_compaction_entry(paths);
    REQUIRE(value.has_value());
    REQUIRE(value->retained_tail.has_value());
    CHECK(value->retained_tail->size() == 4);

    session->close();
}

TEST_CASE(
    "manual compaction rejects sessions too small to compact and in-memory sessions",
    "[coding_agent][compaction][issue358]") {
    // A small session fits the keepRecentTokens budget: nothing to summarize.
    TestPaths paths;
    tests::RuntimeFixture runtime;
    auto client = std::make_shared<CompactionScriptedProvider>();
    client->responses.push_back(ai::assistant_text_message("small reply"));
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::ExplicitOpenOrCreateSessionTarget{paths.session_file};
    options.workspace = paths.workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
    auto models = std::move(options.models);
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    request.execution_runtime_target = runtime.make_target();
    auto created = runtime.run(coding_agent::create_agent_session_async(std::move(request),
            std::nullopt,
            coding_agent::runtime::AssemblyOverrides{
                    .model_runtime = nullptr, .models = std::move(models), .user_shell = nullptr}));
    REQUIRE(created.has_value());
    REQUIRE(run_awaitable(runtime, created->session->prompt("small prompt")).has_value());
    auto rejected = run_awaitable(runtime, created->session->compact());
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == support::ErrorCode::Validation);
    CHECK(rejected.error().message == "Nothing to compact (session too small)");
    created->session->close();

    // In-memory sessions have no session file to persist a CompactionEntry
    // into or to rebuild context from.
    TestPaths in_memory_paths;
    auto memory_client = std::make_shared<CompactionScriptedProvider>();
    memory_client->responses.push_back(ai::assistant_text_message("memory reply"));
    tests::ModelsSessionOptions memory_options;
    memory_options.session_target = coding_agent::InMemorySessionTarget{};
    memory_options.workspace = in_memory_paths.workspace.path();
    memory_options.models = cch::tests::models_from_provider(std::move(memory_client));
    auto memory_models = std::move(memory_options.models);
    coding_agent::runtime::AgentSessionCreationRequest memory_request = std::move(memory_options);
    memory_request.execution_runtime_target = runtime.make_target();
    auto memory_created = runtime.run(coding_agent::create_agent_session_async(std::move(memory_request),
            std::nullopt,
            coding_agent::runtime::AssemblyOverrides{
                    .model_runtime = nullptr, .models = std::move(memory_models), .user_shell = nullptr}));
    REQUIRE(memory_created.has_value());
    auto memory_rejected = run_awaitable(runtime, memory_created->session->compact());
    REQUIRE_FALSE(memory_rejected.has_value());
    CHECK(memory_rejected.error().message ==
          "compaction requires a persisted session file");
    memory_created->session->close();
}

// ── T10 automatic trigger policy (#359) ─────────────────────────────────────

/// Milliseconds since the epoch, matching session-entry timestamps.
[[nodiscard]] ai::TimestampMs wall_clock_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// FIFO scripted client for the automatic-trigger tests. Stamps every
/// assistant response with an increasing wall-clock timestamp (`now + seq`)
/// so a message served after a compaction is strictly newer than the
/// compaction entry (satisfying pi's `assistantIsFromBeforeCompaction`
/// guard), serves overflow error terminals with a configurable message, and
/// records every request for context-rebuild assertions.
class TriggerPolicyScriptedProvider final : public tests::ScriptedProvider {
public:
    TriggerPolicyScriptedProvider() : ScriptedProvider("sdk-host") {}

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext context, coding_agent::ModelRuntimeTestStreamOptions options) override {
        return ai::detail::make_model_stream(
            [this, model = std::move(model), context = std::move(context), options = std::move(options)](
                ai::AssistantEventSink sink) mutable
                -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
        ++request_count;
        requests.push_back(tests::RecordedProviderRequest{model, context, options});
        if (options.stop_token.stop_requested()) {
            auto terminal = ai::assistant_text_message(aborted_content);
            terminal.stop_reason = ai::AssistantStopReason::Aborted;
            terminal.error_message = "Request was aborted";
            terminal.provider = "sdk-host";
            terminal.api = "fake";
            terminal.model = model.id;
            terminal.timestamp = wall_clock_ms() + request_count;
            if (sink) {
                CCH_TRY_VOID(sink(ai::AssistantErrorEvent{
                    .reason = terminal.stop_reason,
                    .error = terminal,
                    .failure = support::make_error(
                        support::ErrorCode::Cancelled, "Request was aborted"),
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
        response.timestamp = wall_clock_ms() + request_count;
        if (sink) {
            if (response.stop_reason == ai::AssistantStopReason::Error ||
                response.stop_reason == ai::AssistantStopReason::Aborted) {
                // Terminal-before-start: no assistant start event; the loop
                // synthesizes one from the authoritative final message.
                CCH_TRY_VOID(sink(ai::AssistantErrorEvent{
                    .reason = response.stop_reason,
                    .error = response,
                    .failure = support::make_error(
                        support::ErrorCode::Stream,
                        response.error_message.value_or("terminal error")),
                }));
            } else {
                CCH_TRY_VOID(sink(ai::AssistantStartEvent{response}));
            }
        }
        co_return response;
                });
    }

    int request_count{0};
    /// Content for an aborted terminal response (deterministic partial text).
    std::string aborted_content;
    std::vector<tests::RecordedProviderRequest> requests;
    std::deque<ai::AssistantMessage> responses;
};

/// The context-overflow error terminal providers return when input exceeds
/// the context window (a pattern from pi's OVERFLOW_PATTERNS).
[[nodiscard]] ai::AssistantMessage overflow_terminal() {
    auto terminal = ai::assistant_text_message("");
    terminal.stop_reason = ai::AssistantStopReason::Error;
    terminal.error_message =
        "Your input exceeds the context window of this model";
    return terminal;
}

/// An assistant message whose provider usage reports `total_tokens`.
[[nodiscard]] ai::AssistantMessage usage_assistant(
    std::string text,
    std::int64_t total_tokens) {
    auto message = ai::assistant_text_message(std::move(text));
    message.usage = ai::Usage{};
    message.usage.input = total_tokens;
    message.usage.total_tokens = total_tokens;
    return message;
}

/// Session request model with a configurable context window.
[[nodiscard]] ai::Model trigger_model(std::uint64_t context_window) {
    auto model = tests::scripted_request_model("sdk-host", "gpt-test");
    model.context_window = context_window;
    return model;
}

struct TriggerSessionUnderTest {
    std::unique_ptr<coding_agent::AgentSession> session;
    TriggerPolicyScriptedProvider* client{nullptr};
};

[[nodiscard]] TriggerSessionUnderTest make_trigger_session(const TestPaths& paths,
        tests::RuntimeFixture& runtime,
        std::deque<ai::AssistantMessage> responses,
        std::uint64_t context_window = 128000) {
    auto client = std::make_shared<TriggerPolicyScriptedProvider>();
    auto* client_ptr = client.get();
    client_ptr->responses = std::move(responses);

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::ExplicitOpenOrCreateSessionTarget{paths.session_file};
    options.workspace = paths.workspace.path();
    options.request_model = trigger_model(context_window);
    options.models = cch::tests::models_from_provider(std::move(client));

    auto models = std::move(options.models);
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    request.execution_runtime_target = runtime.make_target();
    auto created = runtime.run(coding_agent::create_agent_session_async(std::move(request),
            std::nullopt,
            coding_agent::runtime::AssemblyOverrides{
                    .model_runtime = nullptr, .models = std::move(models), .user_shell = nullptr}));
    REQUIRE(created.has_value());
    return TriggerSessionUnderTest{
        std::move(created->session),
        client_ptr,
    };
}

[[nodiscard]] std::string read_golden_text(std::string_view name) {
    const std::string path = std::string{CCH_SOURCE_DIR} +
                             "/fixtures/pi-agent-core/" +
                             std::string{name};
    std::ifstream input(path, std::ios::binary);
    return std::string{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
}

TEST_CASE(
    "overflow terminal compacts and retries the turn exactly once; success continues normally",
    "[coding_agent][compaction][issue359]") {
    TestPaths paths;
    tests::RuntimeFixture runtime;
    const std::string big(19600, 'x');
    auto under_test = make_trigger_session(paths,
            runtime,
            {
                    big_assistant("a1 " + big),
                    big_assistant("a2 " + big),
                    big_assistant("a3 " + big),
                    overflow_terminal(),
                    summarization_response(),
                    big_assistant("recovered"),
            });
    auto* session = under_test.session.get();
    auto* client = under_test.client;

    REQUIRE(run_awaitable(runtime, session->prompt(big + " u1")).has_value());
    REQUIRE(run_awaitable(runtime, session->prompt(big + " u2")).has_value());
    REQUIRE(run_awaitable(runtime, session->prompt(big + " u3")).has_value());
    CHECK(session->message_count() == 6);

    // The overflowing prompt succeeds after one compact-and-retry.
    REQUIRE(run_awaitable(runtime, session->prompt(big + " u4")).has_value());

    // Requests: 4 agent turns + 1 summarization + 1 retry turn — exactly one
    // retry, and the summarization ran once.
    REQUIRE(client->request_count == 6);
    const auto& retry_request = client->requests[5];
    // The retry sees compactionSummary + retained tail; the overflow error
    // terminal is not re-sent (it stays in session history only).
    CHECK(std::holds_alternative<ai::CompactionSummaryMessage>(
        retry_request.context.messages[0]));
    const auto* last_user =
        std::get_if<ai::UserMessage>(&retry_request.context.messages.back());
    REQUIRE(last_user != nullptr);
    CHECK(ai::text_from_user_message(*last_user) == big + " u4");
    for (const auto& message : retry_request.context.messages) {
        const auto* assistant = std::get_if<ai::AssistantMessage>(&message);
        if (assistant != nullptr) {
            CHECK(assistant->stop_reason != ai::AssistantStopReason::Error);
        }
    }

    // A compaction entry was persisted over the overflow.
    const auto value = find_compaction_entry(paths);
    REQUIRE(value.has_value());

    // The retried success continues the session normally: the next prompt
    // streams over the compacted context.
    REQUIRE(run_awaitable(runtime, session->prompt(big + " u5")).has_value());
    REQUIRE(client->request_count == 7);
    CHECK(std::holds_alternative<ai::CompactionSummaryMessage>(
        client->requests[6].context.messages[0]));

    session->close();
}

TEST_CASE(
    "a second overflow after the compact-and-retry fails with pi's verbatim recovery message",
    "[coding_agent][compaction][issue359]") {
    TestPaths paths;
    tests::RuntimeFixture runtime;
    const std::string big(19600, 'x');
    auto under_test = make_trigger_session(paths,
            runtime,
            {
                    big_assistant("a1 " + big),
                    big_assistant("a2 " + big),
                    big_assistant("a3 " + big),
                    overflow_terminal(),
                    summarization_response(),
                    overflow_terminal(),
            });
    auto* session = under_test.session.get();
    auto* client = under_test.client;

    REQUIRE(run_awaitable(runtime, session->prompt(big + " u1")).has_value());
    REQUIRE(run_awaitable(runtime, session->prompt(big + " u2")).has_value());
    REQUIRE(run_awaitable(runtime, session->prompt(big + " u3")).has_value());

    const auto failed = run_awaitable(runtime, session->prompt(big + " u4"));
    REQUIRE_FALSE(failed.has_value());
    // The verbatim overflow-recovery message, pinned by the committed golden.
    CHECK(failed.error().message == read_golden_text("overflow-recovery-message.txt"));
    CHECK(failed.error().message ==
          "Context overflow recovery failed after one compact-and-retry attempt. "
          "Try reducing context or switching to a larger-context model.");

    // One compaction ran (4 turns + 1 summarization + 1 retry that overflowed
    // again); no third attempt.
    REQUIRE(client->request_count == 6);
    // The second overflow error message stays in live state (pi keeps it; the
    // failure is reported instead of retrying again).
    CHECK(session->message_count() == 7);
    const auto value = find_compaction_entry(paths);
    REQUIRE(value.has_value());

    session->close();
}

TEST_CASE(
    "threshold compaction fires over contextWindow - reserveTokens and never retries",
    "[coding_agent][compaction][issue359]") {
    TestPaths paths;
    tests::RuntimeFixture runtime;
    const std::string big(19600, 'x');
    // 30000 window: threshold boundary = 30000 - 16384 = 13616.
    auto under_test = make_trigger_session(paths,
            runtime,
            {
                    big_assistant("a1 " + big),
                    big_assistant("a2 " + big),
                    big_assistant("a3 " + big),
                    usage_assistant("huge", 20000),
                    summarization_response(),
            },
            /*context_window=*/30000);
    auto* session = under_test.session.get();
    auto* client = under_test.client;

    REQUIRE(run_awaitable(runtime, session->prompt(big + " u1")).has_value());
    REQUIRE(run_awaitable(runtime, session->prompt(big + " u2")).has_value());
    REQUIRE(run_awaitable(runtime, session->prompt(big + " u3")).has_value());

    // The fourth prompt's response crosses the threshold: the session compacts
    // and the turn is NOT retried (the completed answer stays).
    REQUIRE(run_awaitable(runtime, session->prompt(big + " u4")).has_value());
    REQUIRE(client->request_count == 5);
    const auto value = find_compaction_entry(paths);
    REQUIRE(value.has_value());

    // No retry: the run completed on the response that crossed the threshold,
    // and the rebuilt context retains that exact answer (a branch read racing
    // the terminal commitment's persistence hop used to drop it from live
    // state, issue #526).
    const auto final_snapshot = session->snapshot();
    const auto& retained = final_snapshot.agent_state.messages.back();
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(retained));
    CHECK(ai::text_from_assistant_content(std::get<ai::AssistantMessage>(retained).content) == "huge");
    session->close();
}

TEST_CASE(
    "disabled compaction settings suppress both automatic triggers",
    "[coding_agent][compaction][issue359]") {
    TestPaths paths;
    tests::RuntimeFixture runtime;
    paths.workspace.write(
        "agent/settings.json",
        R"({"compaction": {"enabled": false}})");
    const tests::EnvVarGuard agent_dir{
        "PI_CODING_AGENT_DIR",
        (paths.workspace.path() / "agent").string()};

    auto under_test = make_trigger_session(paths, runtime, {overflow_terminal()});
    auto* session = under_test.session.get();
    auto* client = under_test.client;

    // The overflow error terminal completes the run normally: no compaction,
    // no retry (pi's settings.enabled gate returns before any decision).
    REQUIRE(run_awaitable(runtime, session->prompt("u1")).has_value());
    REQUIRE(client->request_count == 1);
    CHECK(session->message_count() == 2);
    CHECK_FALSE(find_compaction_entry(paths).has_value());
    session->close();
}

TEST_CASE(
    "pre-prompt compaction check catches an aborted response over the threshold",
    "[coding_agent][compaction][issue359]") {
    TestPaths paths;
    tests::RuntimeFixture runtime;
    const std::string big(19600, 'x');
    auto aborted = usage_assistant("", 20000);
    aborted.stop_reason = ai::AssistantStopReason::Aborted;
    aborted.error_message = "Request was aborted";
    // 30000 window: threshold boundary = 13616.
    auto under_test = make_trigger_session(paths,
            runtime,
            {
                    big_assistant("a1 " + big),
                    big_assistant("a2 " + big),
                    big_assistant("a3 " + big),
                    aborted,
                    summarization_response(),
                    big_assistant("after pre-prompt compaction"),
            },
            /*context_window=*/30000);
    auto* session = under_test.session.get();
    auto* client = under_test.client;

    REQUIRE(run_awaitable(runtime, session->prompt(big + " u1")).has_value());
    REQUIRE(run_awaitable(runtime, session->prompt(big + " u2")).has_value());
    REQUIRE(run_awaitable(runtime, session->prompt(big + " u3")).has_value());

    // The fourth run is aborted (user cancellation path); the post-run check
    // skips aborted messages.
    REQUIRE(run_awaitable(runtime, session->prompt(big + " u4")).has_value());
    REQUIRE(client->request_count == 4);

    // The next prompt's pre-send check (skipAbortedCheck=false) compacts the
    // aborted response's over-threshold usage before the new prompt streams.
    REQUIRE(run_awaitable(runtime, session->prompt(big + " u5")).has_value());
    REQUIRE(client->request_count == 6);
    REQUIRE(find_compaction_entry(paths).has_value());
    // The u5 request runs on the compacted context.
    CHECK(std::holds_alternative<ai::CompactionSummaryMessage>(
        client->requests[5].context.messages[0]));

    session->close();
}
