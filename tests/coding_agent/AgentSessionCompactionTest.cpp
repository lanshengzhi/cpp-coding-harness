// Manual compaction trigger evidence for #358 (T09): the session-assembly
// trigger (pi `AgentSession.compact`) driving the harness compaction machinery
// through the scripted fake `Models` seam — abort-in-flight and idle cases,
// `CompactionEntry` persistence, and context rebuild as compactionSummary +
// retained tail. No live keys or network: every request is served by the
// scripted chat client below.

#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/coding_agent/Sdk.hpp>
#include <cch/harness/session/JsonlSessionStore.hpp>
#include <cch/util/Error.hpp>
#include "support/ModelsFixture.hpp"
#include "support/TempWorkspace.hpp"
#include "util/ExpectedMacros.hpp"

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

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
class CompactionScriptedClient final : public ai::StreamingChatClient {
public:
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& request,
        ai::AssistantEventSink sink) override {
        ++request_count;
        requests.push_back(request);
        if (gate_request_number &&
            request_count == *gate_request_number) {
            gate_.emplace(co_await boost::asio::this_coro::executor);
            gate_->expires_at(std::chrono::steady_clock::time_point::max());
            boost::system::error_code error;
            co_await gate_->async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, error));
        }
        if (request.stop_token.stop_requested()) {
            auto terminal = ai::assistant_text_message(aborted_content);
            terminal.stop_reason = ai::AssistantStopReason::Aborted;
            terminal.error_message = "Request was aborted";
            terminal.provider = "sdk-host";
            terminal.api = "fake";
            terminal.model = request.model.id;
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
        response.model = request.model.id;
        // Session files require real epoch timestamps on assistant messages;
        // stamp the same deterministic value the other SDK fake clients use.
        response.timestamp = 1718000000123;
        if (sink) {
            CCH_TRY_VOID(sink(ai::AssistantStartEvent{response}));
        }
        co_return response;
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
    std::vector<ai::StreamChatRequest> requests;
    std::deque<ai::AssistantMessage> responses;
    std::optional<boost::asio::steady_timer> gate_;
};

template <typename T>
[[nodiscard]] T run_awaitable(boost::asio::awaitable<T> awaitable) {
    boost::asio::io_context io;
    std::optional<T> result;
    std::exception_ptr exception;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            try {
                result = co_await std::move(awaitable);
            } catch (...) {
                exception = std::current_exception();
            }
            co_return;
        },
        boost::asio::detached);
    io.run();
    if (exception) {
        std::rethrow_exception(exception);
    }
    REQUIRE(result.has_value());
    return std::move(*result);
}

/// A persisted session whose live history reaches the 20000-token
/// keepRecentTokens budget with a deterministic cut: three 20K-char
/// user/assistant pairs (each ~5001 estimated tokens). The cut keeps
/// [u2, a2, u3, a3] and summarizes [u1, a1]. Returns the created session.
struct SessionUnderTest {
    std::unique_ptr<coding_agent::AgentSession> session;
    CompactionScriptedClient* client{nullptr};
};

[[nodiscard]] SessionUnderTest make_big_session(const TestPaths& paths) {
    const std::string big(20000, 'x');
    auto client = std::make_unique<CompactionScriptedClient>();
    auto* client_ptr = client.get();
    client_ptr->responses.push_back(big_assistant("a1 " + big));
    client_ptr->responses.push_back(big_assistant("a2 " + big));
    client_ptr->responses.push_back(big_assistant("a3 " + big));

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    options.workspace = paths.workspace.path();
    options.models = cch::tests::models_from_stream(std::move(client));

    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created.has_value());
    auto* session = created->session.get();
    REQUIRE(session->prompt_blocking(big + " u1").has_value());
    REQUIRE(session->prompt_blocking(big + " u2").has_value());
    REQUIRE(session->prompt_blocking(big + " u3").has_value());
    CHECK(session->message_count() == 6);
    return SessionUnderTest{
        std::move(created->session),
        client_ptr,
    };
}

[[nodiscard]] std::optional<harness::session::CompactionEntryValue> find_compaction_entry(
    const TestPaths& paths) {
    auto loaded = harness::session::JsonlSessionStore::load(paths.session_file);
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
    auto under_test = make_big_session(paths);
    auto* session = under_test.session.get();
    auto* client = under_test.client;
    client->responses.push_back(summarization_response());
    client->responses.push_back(ai::assistant_text_message("after compaction"));

    auto result = run_awaitable(session->compact());
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
    REQUIRE(session->prompt_blocking("after compaction").has_value());
    REQUIRE(client->requests.size() == 5);
    const auto& next_request = client->requests[4];
    CHECK(next_request.context.messages.size() == 6);
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
    const std::string big(20000, 'x');
    auto client = std::make_unique<CompactionScriptedClient>();
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
    options.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    options.workspace = paths.workspace.path();
    options.models = cch::tests::models_from_stream(std::move(client));

    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created.has_value());
    auto* session = created->session.get();
    REQUIRE(session->prompt_blocking(big + " u1").has_value());
    REQUIRE(session->prompt_blocking(big + " u2").has_value());

    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> prompt_result;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            prompt_result = co_await session->prompt(big + " u3");
            co_return;
        },
        boost::asio::detached);
    while (client_ptr->request_count < 3) {
        REQUIRE(io.poll_one() == 1);
    }

    std::optional<util::Expected<coding_agent::CompactionResult>> compact_result;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            compact_result = co_await session->compact();
            co_return;
        },
        boost::asio::detached);

    // Release the in-flight prompt: it observes the cancellation requested by
    // compact() and settles with the ordinary aborted terminal.
    client_ptr->release_gate();
    if (io.stopped()) {
        io.restart();
    }
    io.run();

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
    CHECK(client_ptr->requests[2].stop_token.stop_requested());
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
    auto client = std::make_unique<CompactionScriptedClient>();
    client->responses.push_back(ai::assistant_text_message("small reply"));
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    options.workspace = paths.workspace.path();
    options.models = cch::tests::models_from_stream(std::move(client));
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created.has_value());
    REQUIRE(created->session->prompt_blocking("small prompt").has_value());
    auto rejected = run_awaitable(created->session->compact());
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == util::ErrorCode::Validation);
    CHECK(rejected.error().message == "Nothing to compact (session too small)");
    created->session->close();

    // In-memory sessions have no session file to persist a CompactionEntry
    // into or to rebuild context from.
    TestPaths in_memory_paths;
    auto memory_client = std::make_unique<CompactionScriptedClient>();
    memory_client->responses.push_back(ai::assistant_text_message("memory reply"));
    tests::ModelsSessionOptions memory_options;
    memory_options.session_target = coding_agent::InMemorySessionTarget{};
    memory_options.workspace = in_memory_paths.workspace.path();
    memory_options.models = cch::tests::models_from_stream(std::move(memory_client));
    auto memory_created = coding_agent::create_agent_session(std::move(memory_options));
    REQUIRE(memory_created.has_value());
    auto memory_rejected = run_awaitable(memory_created->session->compact());
    REQUIRE_FALSE(memory_rejected.has_value());
    CHECK(memory_rejected.error().message ==
          "compaction requires a persisted session file");
    memory_created->session->close();
}
