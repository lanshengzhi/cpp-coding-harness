// Compaction evidence for #358 (T09) and #541: the harness Compaction
// capability mirroring pi `packages/agent/src/harness/compaction/` at
// baseline 83114817, tested through the one `compact` door over a SessionStore
// — cut-point selection with keepRecentTokens and split-turn handling, token
// estimation, the typed skip reasons, and the summarization request surface
// (`cacheRetention: "none"` + a fresh session id) captured through the
// recording fake stream seam. Goldens under fixtures/pi-agent-core/ follow
// the #330 sanitization rules (dummy values only, no live credentials).
//
// Pre-#541 this file tested the module's private mechanics (findCutPoint,
// prepareCompaction, serializeConversation) directly. The interface
// contraction moved those behind the door; their edge cases are re-expressed
// here as door scenarios (replace, don't layer).

#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/ai/Model.hpp>
#include "agent/harness/session/JsonlSessionStore.hpp"
#include <cch/agent/harness/session/SessionEntry.hpp>
#include <cch/agent/harness/session/SessionStore.hpp>
#include <cch/agent/harness/session/SessionTree.hpp>
#include <cch/support/Error.hpp>
#include "ai/glaze/AiJson.hpp"
#include "agent/harness/compaction/Compaction.hpp"
#include <cch/ai/Models.hpp>
#include "support/AsyncResultBridge.hpp"
#include "support/FakeModelStream.hpp"
#include "support/ModelFixture.hpp"
#include "support/TempWorkspace.hpp"
#include "support/Json.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
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
    const support::JsonValue& actual,
    std::string_view fixture_name) {
    auto serialized = support::write_json(actual);
    REQUIRE(serialized);
    const auto expected = read_fixture_text(fixture_name);
    if (*serialized != expected) {
        std::cerr << "\n[CompactionTest] fixture mismatch: "
                  << fixture_name << "\n--- expected ---\n"
                  << expected << "\n--- actual ---\n"
                  << *serialized << "\n--- end ---\n";
    }
    CHECK(*serialized == expected);
}

[[nodiscard]] ai::Usage mock_usage(
    std::int64_t input,
    std::int64_t output,
    std::int64_t cache_read = 0,
    std::int64_t cache_write = 0) {
    ai::Usage usage;
    usage.input = input;
    usage.output = output;
    usage.cache_read = cache_read;
    usage.cache_write = cache_write;
    usage.total_tokens = input + output + cache_read + cache_write;
    usage.cost.total = 0;
    return usage;
}

// ── Door drivers ────────────────────────────────────────────────────────────

/// Append one user text message and return its generated entry id.
std::string append_user(harness::session::SessionStore& store, std::string text, ai::TimestampMs timestamp = 0) {
    REQUIRE(store.append(ai::user_text_message(std::move(text), timestamp)).has_value());
    return store.leaf_id();
}

/// Append one assistant text message with usage and return its entry id.
std::string append_assistant(
        harness::session::SessionStore& store, std::string text, ai::Usage usage, ai::TimestampMs timestamp = 0) {
    auto message = ai::assistant_text_message(std::move(text), timestamp);
    message.usage = usage;
    REQUIRE(store.append(std::move(message)).has_value());
    return store.leaf_id();
}

/// Append a prior compaction entry (pi `appendCompaction`): the iterative
/// summarization boundary.
void append_prior_compaction(harness::session::SessionStore& store,
        std::string summary,
        std::string first_kept_entry_id,
        std::vector<ai::MessageVariant> retained_tail = {}) {
    auto appended = store.append_compaction(std::nullopt,
            harness::session::CompactionEntryValue{
                    .summary = std::move(summary),
                    .first_kept_entry_id = std::move(first_kept_entry_id),
                    .tokens_before = 1234,
                    .retained_tail = std::move(retained_tail),
                    .details = std::nullopt,
                    .usage = std::nullopt,
                    .from_hook = false,
            });
    REQUIRE(appended.has_value());
}

/// A no-op event sink for standalone summarization streams: the recording
/// fake ModelRuntime invokes the sink for non-terminal responses, so the
/// machinery tests supply a passive sink instead of an empty one.
[[nodiscard]] ai::AssistantEventSink noop_sink() {
    return [](const ai::AssistantStreamEvent&) { return support::ExpectedVoid{}; };
}

/// A scripted summarization stream over the recording fake: each call pops
/// the next scripted response and records the request.
[[nodiscard]] harness::session::SummarizationStreamFn scripted_stream(
        std::shared_ptr<tests::FakeModelStream> runtime, ai::Model model) {
    return [runtime = std::move(runtime), model = std::move(model)](
                   ai::AiContext context, ai::SimpleStreamOptions options) mutable
                   -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
        auto stream = runtime->factory()(model, std::move(context), std::move(options));
        co_return co_await support::detail::await_async_result(std::move(stream).run(noop_sink()));
    };
}

template <typename T> [[nodiscard]] T run_awaitable(boost::asio::awaitable<T> awaitable) {
    boost::asio::io_context io;
    std::optional<T> result;
    // Run the lazy coroutine on the temporary executor through the private
    // completion bridge so the awaitable's terminal outcome stays on the
    // typed `Expected` channel (ADR 0042; no exception path in test code).
    auto bridged = support::detail::make_async_result_on(
            io.get_executor(), [awaitable = std::move(awaitable)]() mutable -> boost::asio::awaitable<T> {
                co_return co_await std::move(awaitable);
            });
    std::move(bridged).start([&result](T completion) noexcept { result.emplace(std::move(completion)); });
    io.run();
    REQUIRE(result.has_value());
    return std::move(*result);
}

/// Run the one compaction door to its terminal outcome.
[[nodiscard]] support::Expected<harness::session::CompactionOutcomeVariant> run_door(
        harness::session::SessionStore& store, const ai::Model& model, harness::session::CompactionRunOptions options) {
    return run_awaitable(harness::session::compact(store, model, std::move(options)));
}

/// The text of the message an entry id resolves to: cut-point assertions read
/// the kept boundary through the store rather than pinning generated ids.
[[nodiscard]] std::string entry_message_text(harness::session::SessionStore& store, const std::string& entry_id) {
    const auto entry = store.get_entry(entry_id);
    REQUIRE(entry.has_value());
    REQUIRE(entry->message.has_value());
    const auto& message = *entry->message;
    if (const auto* user = std::get_if<ai::UserMessage>(&message)) {
        return ai::text_from_user_message(*user);
    }
    if (const auto* assistant = std::get_if<ai::AssistantMessage>(&message)) {
        return ai::text_from_assistant_content(assistant->content);
    }
    return {};
}

/// The model-visible text of one recorded summarization request.
[[nodiscard]] std::string request_text(const tests::RecordedStreamSimpleCall& call) {
    REQUIRE(call.context.messages.size() == 1);
    const auto* user = std::get_if<ai::UserMessage>(&call.context.messages.front());
    REQUIRE(user != nullptr);
    return ai::text_from_user_message(*user);
}

/// The deterministic six-entry budget geometry: 100-char users and 4-char
/// assistants, where keepRecentTokens 30 crosses at the third user message —
/// the cut keeps the last two pairs and summarizes the first without
/// splitting a turn.
[[nodiscard]] harness::session::SessionStore budget_geometry_store() {
    auto store = harness::session::SessionStore::in_memory();
    append_user(store, std::string(100, 'X'));
    append_assistant(store, std::string(4, 'Y'), mock_usage(100, 50));
    append_user(store, std::string(100, 'A'));
    append_assistant(store, std::string(4, 'B'), mock_usage(100, 50));
    append_user(store, std::string(100, 'C'));
    append_assistant(store, std::string(4, 'D'), mock_usage(100, 50, 0, 0));
    return store;
}

[[nodiscard]] harness::session::CompactionSettings budget_geometry_settings() {
    return harness::session::CompactionSettings{
            .enabled = true,
            .reserve_tokens = 2000,
            .keep_recent_tokens = 30,
    };
}

/// The model-visible side of one recorded summarization request at the fake
/// ModelRuntime seam: the options prove `cacheRetention: "none"` plus the
/// fresh session id, and the context proves the prompt construction.
[[nodiscard]] support::JsonValue summarization_request_to_json(
    const tests::RecordedStreamSimpleCall& call) {
    support::JsonValue object{support::JsonValue::object_t{}};
    auto& o = object.get_object();
    support::JsonValue model_object{support::JsonValue::object_t{}};
    model_object.get_object().emplace("id", support::JsonValue(call.model.id));
    model_object.get_object().emplace(
        "provider", support::JsonValue(call.model.provider));
    o.emplace("model", std::move(model_object));

    if (call.context.system_prompt) {
        o.emplace("systemPrompt", support::JsonValue(*call.context.system_prompt));
    }
    support::JsonValue messages{support::JsonValue::array_t{}};
    for (const auto& message : call.context.messages) {
        auto serialized = support::write_json(ai::glaze::to_message_dto(message));
        REQUIRE(serialized);
        auto parsed = support::read_json(*serialized);
        REQUIRE(parsed);
        messages.get_array().push_back(std::move(*parsed));
    }
    o.emplace("messages", std::move(messages));

    support::JsonValue options{support::JsonValue::object_t{}};
    if (call.options.max_tokens) {
        options.get_object().emplace(
            "maxTokens", support::JsonValue(static_cast<int>(*call.options.max_tokens)));
    }
    if (call.options.cache_retention) {
        std::string retention;
        switch (*call.options.cache_retention) {
        case ai::CacheRetention::None:
            retention = "none";
            break;
        case ai::CacheRetention::Short:
            retention = "short";
            break;
        case ai::CacheRetention::Long:
            retention = "long";
            break;
        }
        options.get_object().emplace("cacheRetention", support::JsonValue(retention));
    }
    if (call.options.session_id) {
        options.get_object().emplace(
            "sessionId", support::JsonValue(*call.options.session_id));
    }
    if (call.options.reasoning) {
        std::string reasoning;
        switch (*call.options.reasoning) {
        case ai::ThinkingLevel::Minimal:
            reasoning = "minimal";
            break;
        case ai::ThinkingLevel::Low:
            reasoning = "low";
            break;
        case ai::ThinkingLevel::Medium:
            reasoning = "medium";
            break;
        case ai::ThinkingLevel::High:
            reasoning = "high";
            break;
        case ai::ThinkingLevel::XHigh:
            reasoning = "xhigh";
            break;
        case ai::ThinkingLevel::Max:
            reasoning = "max";
            break;
        }
        options.get_object().emplace("reasoning", support::JsonValue(reasoning));
    }
    o.emplace("options", std::move(options));
    return object;
}

/// A deterministic fresh-session-id factory for goldens: sequential
/// distinguishable dummy values, proving each summarization request carries
/// its own id (pi `uuidv7()` is nondeterministic by design).
[[nodiscard]] harness::session::SummarizationSessionIdFactory
sequential_session_ids() {
    auto counter = std::make_shared<int>(0);
    return [counter]() mutable {
        return "summarization-session-" + std::to_string(++(*counter));
    };
}

} // namespace

TEST_CASE(
    "compaction token estimation matches pi's conservative char/4 heuristic",
    "[harness][compaction][issue358]") {
    using harness::session::calculate_context_tokens;
    using harness::session::estimate_tokens;

    CHECK(calculate_context_tokens(mock_usage(1000, 500, 200, 100)) == 1800);
    CHECK(calculate_context_tokens(mock_usage(0, 0, 0, 0)) == 0);

    // user string content: chars/4.
    CHECK(estimate_tokens(ai::user_text_message("plain user")) == 3);
    // assistant: text + thinking + toolCall name/args.
    ai::AssistantMessage assistant;
    assistant.content.push_back(ai::text_content("assistant"));
    assistant.content.push_back(ai::thinking_content("thinking"));
    assistant.content.push_back(ai::tool_call_content(
        "call-1", "read", R"({"path":"file.ts"})"));
    // 9 + 8 + 4 + 18 = 39 chars → ceil(39/4) = 10.
    CHECK(estimate_tokens(ai::MessageVariant{assistant}) == 10);
    // custom content (blocks).
    ai::CustomMessage custom;
    custom.custom_type = "note";
    custom.content.push_back(ai::text_content("custom text"));
    CHECK(estimate_tokens(ai::MessageVariant{custom}) == 3);
    // tool result with an image counts the fixed 4800-char image estimate.
    ai::ToolResultMessage tool_result;
    tool_result.content.push_back(ai::text_content("tool text"));
    tool_result.content.push_back(ai::image_content("abc", "image/png"));
    CHECK(estimate_tokens(ai::MessageVariant{tool_result}) > 1000);
    // bashExecution: command + output.
    ai::BashExecutionMessage bash;
    bash.command = "npm run check";
    bash.output = "ok";
    CHECK(estimate_tokens(ai::MessageVariant{bash}) > 0);
    // branchSummary / compactionSummary: summary length.
    ai::BranchSummaryMessage branch;
    branch.summary = "branch";
    CHECK(estimate_tokens(ai::MessageVariant{branch}) > 0);
    ai::CompactionSummaryMessage compaction_summary;
    compaction_summary.summary = "compact";
    CHECK(estimate_tokens(ai::MessageVariant{compaction_summary}) > 0);
    // system (pi has no system AgentMessage role) estimates 0.
    ai::SystemMessage system;
    system.content = "system";
    CHECK(estimate_tokens(ai::MessageVariant{system}) == 0);
}

TEST_CASE("estimateContextTokens uses the last assistant usage plus trailing estimate",
        "[harness][compaction][issue358][issue541]") {
    using harness::session::estimate_context_tokens;

    auto no_usage = estimate_context_tokens(
        {ai::MessageVariant{ai::user_text_message("no usage")}});
    CHECK(no_usage.last_usage_index == std::nullopt);
    CHECK(no_usage.usage_tokens == 0);
    CHECK(no_usage.tokens == no_usage.trailing_tokens);

    ai::AssistantMessage assistant = ai::assistant_text_message("assistant");
    assistant.usage = mock_usage(10, 5, 3, 2);
    auto with_usage = estimate_context_tokens(
        {ai::MessageVariant{ai::user_text_message("Hello")},
         ai::MessageVariant{assistant},
         ai::MessageVariant{ai::user_text_message("tail")}});
    CHECK(with_usage.usage_tokens == 20);
    CHECK(with_usage.last_usage_index == 1);
    CHECK(with_usage.trailing_tokens > 0);
    CHECK(with_usage.tokens == 20 + with_usage.trailing_tokens);

    // Aborted, error, and all-zero assistant usages never anchor the
    // estimate (pi `getLastAssistantUsage` skip semantics, exercised through
    // the public estimation surface after the helper was absorbed).
    auto aborted = ai::assistant_text_message("aborted");
    aborted.stop_reason = ai::AssistantStopReason::Aborted;
    aborted.usage = mock_usage(10, 5);
    auto errored = ai::assistant_text_message("errored");
    errored.stop_reason = ai::AssistantStopReason::Error;
    errored.usage = mock_usage(10, 5);
    auto zero = ai::assistant_text_message("zero");
    zero.usage = mock_usage(0, 0);
    const auto none_valid = estimate_context_tokens(
            {ai::MessageVariant{aborted}, ai::MessageVariant{errored}, ai::MessageVariant{zero}});
    CHECK(none_valid.last_usage_index == std::nullopt);
    // A later invalid assistant does not hide an earlier valid usage.
    const auto anchored = estimate_context_tokens({ai::MessageVariant{assistant}, ai::MessageVariant{errored}});
    CHECK(anchored.last_usage_index == 0);
    CHECK(anchored.usage_tokens == 20);
}

TEST_CASE("the compaction door skips inapplicable branches with typed reasons", "[harness][compaction][issue541]") {
    const auto model = tests::make_model("gpt-test");

    auto run_with_probes = [&](harness::session::SessionStore& store) {
        auto runtime = std::make_shared<tests::FakeModelStream>();
        runtime->responses.push_back(ai::assistant_text_message("unused"));
        bool start_fired = false;
        harness::session::CompactionRunOptions options;
        options.summarization_stream = scripted_stream(runtime, model);
        options.on_compaction_start = [&start_fired] { start_fired = true; };
        auto outcome = run_door(store, model, std::move(options));
        REQUIRE(outcome.has_value());
        // A skip never issues a summarization request and never reaches the
        // auto trigger's `compaction_start` point (pi `_runAutoCompaction`
        // emits nothing for a not-applicable preparation).
        CHECK(runtime->calls.empty());
        CHECK_FALSE(start_fired);
        return std::move(*outcome);
    };

    // An empty branch has its own typed reason.
    {
        auto store = harness::session::SessionStore::in_memory();
        const auto outcome = run_with_probes(store);
        REQUIRE(std::holds_alternative<harness::session::CompactionSkipped>(outcome));
        CHECK(std::get<harness::session::CompactionSkipped>(outcome).reason ==
                harness::session::CompactionSkipped::Reason::Empty);
    }

    // A branch already ending in a compaction.
    {
        auto store = harness::session::SessionStore::in_memory();
        append_user(store, "user");
        append_assistant(store, "assistant", mock_usage(100, 50));
        append_prior_compaction(store, "already compacted", "unused");
        const auto outcome = run_with_probes(store);
        REQUIRE(std::holds_alternative<harness::session::CompactionSkipped>(outcome));
        CHECK(std::get<harness::session::CompactionSkipped>(outcome).reason ==
                harness::session::CompactionSkipped::Reason::AlreadyCompacted);
    }

    // A session that fits the keepRecentTokens budget: nothing falls before
    // the cut point.
    {
        auto store = harness::session::SessionStore::in_memory();
        append_user(store, "small");
        append_assistant(store, "session", mock_usage(100, 50));
        const auto outcome = run_with_probes(store);
        REQUIRE(std::holds_alternative<harness::session::CompactionSkipped>(outcome));
        CHECK(std::get<harness::session::CompactionSkipped>(outcome).reason ==
                harness::session::CompactionSkipped::Reason::NothingToSummarize);
    }
}

TEST_CASE("the compaction door keeps the recent-token budget and never cuts before the boundary",
        "[harness][compaction][issue358][issue541]") {
    const auto model = tests::make_model("gpt-test");

    // Ten user/assistant pairs of 100 characters (25 estimated tokens each);
    // keepRecentTokens 200 crosses eight entries back, so the cut keeps the
    // last four pairs and summarizes [u0..a5] without splitting a turn.
    const auto padded_user = [](int index) { return "User " + std::to_string(index) + std::string(94, 'u'); };
    const auto padded_assistant = [](int index) { return "Assistant " + std::to_string(index) + std::string(89, 'a'); };
    auto store = harness::session::SessionStore::in_memory();
    for (int i = 0; i < 10; ++i) {
        append_user(store, padded_user(i));
        append_assistant(store, padded_assistant(i), mock_usage(0, 100));
    }
    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("## Summary"));
    harness::session::CompactionRunOptions options;
    options.settings = harness::session::CompactionSettings{
            .enabled = true,
            .reserve_tokens = 2000,
            .keep_recent_tokens = 200,
    };
    options.summarization_stream = scripted_stream(runtime, model);

    auto outcome = run_door(store, model, std::move(options));
    REQUIRE(outcome.has_value());
    const auto* result = std::get_if<harness::session::CompactionResult>(&*outcome);
    REQUIRE(result != nullptr);
    REQUIRE(runtime->calls.size() == 1);
    // The summarized range covers the old history; the retained tail does
    // not leak into the request.
    const auto prompt = request_text(runtime->calls.front());
    CHECK(prompt.find(padded_user(0)) != std::string::npos);
    CHECK(prompt.find(padded_user(6)) == std::string::npos);
    REQUIRE(result->retained_tail.size() == 8);
    // The kept boundary is the sixth user message, resolved through the
    // store rather than a pinned generated id.
    CHECK(entry_message_text(store, result->first_kept_entry_id) == padded_user(6));

    // A lone tool result is not a valid cut point: nothing falls before the
    // kept range, so the door reports nothing to compact.
    {
        auto lone = harness::session::SessionStore::in_memory();
        REQUIRE(lone.append(ai::tool_result_message("call-1", "read", "tool output", false)).has_value());
        auto lone_runtime = std::make_shared<tests::FakeModelStream>();
        lone_runtime->responses.push_back(ai::assistant_text_message("x"));
        harness::session::CompactionRunOptions lone_options;
        lone_options.settings.keep_recent_tokens = 1;
        lone_options.summarization_stream = scripted_stream(lone_runtime, model);
        auto lone_outcome = run_door(lone, model, std::move(lone_options));
        REQUIRE(lone_outcome.has_value());
        REQUIRE(std::holds_alternative<harness::session::CompactionSkipped>(*lone_outcome));
        CHECK(std::get<harness::session::CompactionSkipped>(*lone_outcome).reason ==
                harness::session::CompactionSkipped::Reason::NothingToSummarize);
        CHECK(lone_runtime->calls.empty());
    }

    // The cut never crosses the previous compaction's kept boundary: with
    // the boundary at the assistant entry, nothing summarizable remains
    // before it and the door skips instead of reaching across.
    {
        auto bounded = harness::session::SessionStore::in_memory();
        append_user(bounded, "ancient history");
        const auto kept = append_assistant(bounded, "post-compaction", mock_usage(100, 50));
        append_prior_compaction(bounded, "earlier summary", kept);
        REQUIRE(bounded.append_session_info(std::nullopt, "boundary marker").has_value());
        auto bounded_runtime = std::make_shared<tests::FakeModelStream>();
        bounded_runtime->responses.push_back(ai::assistant_text_message("x"));
        harness::session::CompactionRunOptions bounded_options;
        bounded_options.settings.keep_recent_tokens = 1;
        bounded_options.summarization_stream = scripted_stream(bounded_runtime, model);
        auto bounded_outcome = run_door(bounded, model, std::move(bounded_options));
        REQUIRE(bounded_outcome.has_value());
        REQUIRE(std::holds_alternative<harness::session::CompactionSkipped>(*bounded_outcome));
        CHECK(std::get<harness::session::CompactionSkipped>(*bounded_outcome).reason ==
                harness::session::CompactionSkipped::Reason::NothingToSummarize);
        CHECK(bounded_runtime->calls.empty());
    }
}

TEST_CASE("the compaction door never splits a turn when the cut lands on a user message",
        "[harness][compaction][issue358][issue541]") {
    auto store = harness::session::SessionStore::in_memory();
    append_user(store, "large turn");
    append_assistant(store, "large assistant message", mock_usage(300, 100));
    append_user(store, "keep me");
    append_assistant(store, "recent", mock_usage(100, 50));

    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("## Summary"));
    const auto model = tests::make_model("gpt-test");
    harness::session::CompactionRunOptions options;
    // Budget 3 crosses at the second user message: the whole first turn is
    // summarized and the second turn is retained whole.
    options.settings.keep_recent_tokens = 3;
    options.summarization_stream = scripted_stream(runtime, model);

    auto outcome = run_door(store, model, std::move(options));
    REQUIRE(outcome.has_value());
    const auto* result = std::get_if<harness::session::CompactionResult>(&*outcome);
    REQUIRE(result != nullptr);
    REQUIRE(runtime->calls.size() == 1);
    const auto prompt = request_text(runtime->calls.front());
    CHECK(prompt.find("large turn") != std::string::npos);
    CHECK(prompt.find("large assistant message") != std::string::npos);
    // A single request (no turn-prefix summarization) and the retained tail
    // is exactly the second turn.
    CHECK(result->summary.find("**Turn Context (split turn):**") == std::string::npos);
    REQUIRE(result->retained_tail.size() == 2);
    CHECK(entry_message_text(store, result->first_kept_entry_id) == "keep me");
}

TEST_CASE("split-turn compaction issues two requests with distinct fresh session ids",
        "[harness][compaction][issue358][issue541]") {
    auto runtime = std::make_shared<tests::FakeModelStream>();
    auto history_response = ai::assistant_text_message("history summary");
    history_response.usage = mock_usage(1000, 200);
    auto prefix_response = ai::assistant_text_message("prefix summary");
    prefix_response.usage = mock_usage(1000, 200);
    runtime->responses.push_back(std::move(history_response));
    runtime->responses.push_back(std::move(prefix_response));

    // A turn too large to keep sits after summarized history, so both the
    // history and the turn-prefix summarization run: the cut lands on the
    // large assistant, splitting the "large turn" turn.
    auto store = harness::session::SessionStore::in_memory();
    append_user(store, "old history");
    append_assistant(store, "old assistant", mock_usage(300, 100));
    append_user(store, "large turn");
    append_assistant(store, "large assistant message", mock_usage(300, 100));
    append_user(store, "keep me");
    append_assistant(store, "recent", mock_usage(100, 50));

    const auto model = tests::make_model("gpt-test");
    std::vector<std::string> order;
    harness::session::CompactionRunOptions options;
    options.settings = harness::session::CompactionSettings{
            .enabled = true,
            .reserve_tokens = 2000,
            .keep_recent_tokens = 10,
    };
    auto inner = scripted_stream(runtime, model);
    options.summarization_stream = [&order, inner = std::move(inner)](
                                           ai::AiContext context, ai::SimpleStreamOptions stream_options) mutable
            -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
        order.push_back("call");
        co_return co_await inner(std::move(context), std::move(stream_options));
    };
    options.session_id_factory = sequential_session_ids();
    // The auto trigger's `compaction_start` point: after preparation, before
    // the first request.
    options.on_compaction_start = [&order] { order.push_back("start"); };

    auto outcome = run_door(store, model, std::move(options));
    REQUIRE(outcome.has_value());
    const auto* result = std::get_if<harness::session::CompactionResult>(&*outcome);
    REQUIRE(result != nullptr);
    REQUIRE(runtime->calls.size() == 2);
    CHECK(runtime->calls[0].options.cache_retention == ai::CacheRetention::None);
    CHECK(runtime->calls[1].options.cache_retention == ai::CacheRetention::None);
    REQUIRE(runtime->calls[0].options.session_id.has_value());
    REQUIRE(runtime->calls[1].options.session_id.has_value());
    CHECK(*runtime->calls[0].options.session_id == "summarization-session-1");
    CHECK(*runtime->calls[1].options.session_id == "summarization-session-2");
    CHECK(*runtime->calls[0].options.session_id != *runtime->calls[1].options.session_id);
    // The start hook fires exactly once, before the first request.
    CHECK(order == std::vector<std::string>{"start", "call", "call"});
    // The history request summarizes the old turn; the prefix request covers
    // the split turn's beginning and never the retained tail.
    CHECK(request_text(runtime->calls[0]).find("old history") != std::string::npos);
    const auto prefix_prompt = request_text(runtime->calls[1]);
    CHECK(prefix_prompt.find("large turn") != std::string::npos);
    CHECK(prefix_prompt.find("keep me") == std::string::npos);
    // The merged summary carries pi's split-turn separator; usage combines
    // both calls.
    CHECK(result->summary.find("**Turn Context (split turn):**") != std::string::npos);
    REQUIRE(result->usage.has_value());
    CHECK(result->usage->input == 1000 + 1000);
    CHECK(result->usage->output == 200 + 200);
}

TEST_CASE("the compaction door carries the latest compaction summary into the update prompt",
        "[harness][compaction][issue358][issue541]") {
    auto store = harness::session::SessionStore::in_memory();
    append_user(store, "user msg 1");
    append_assistant(store, "assistant msg 1", mock_usage(5000, 1000));
    const auto kept_user = append_user(store, "user msg 2");
    auto kept_assistant_message = ai::assistant_text_message("assistant msg 2");
    kept_assistant_message.usage = mock_usage(8000, 2000);
    append_assistant(store, "assistant msg 2", mock_usage(8000, 2000));
    // The previous compaction kept from the second user message onward.
    append_prior_compaction(store,
            "First summary",
            kept_user,
            {ai::MessageVariant{ai::user_text_message("user msg 2")}, ai::MessageVariant{kept_assistant_message}});
    append_user(store, "user msg 3");
    append_assistant(store, "assistant msg 3", mock_usage(8000, 2000));
    append_user(store, "user msg 4");
    append_assistant(store, "assistant msg 4", mock_usage(8000, 2000));

    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("## Summary"));
    const auto model = tests::make_model("gpt-test");
    harness::session::CompactionRunOptions options;
    // Budget 12 retains [u3, a3, u4, a4] and updates the prior summary with
    // the post-boundary [u2, a2] history.
    options.settings.keep_recent_tokens = 12;
    options.summarization_stream = scripted_stream(runtime, model);

    const auto expected_tokens = harness::session::estimate_context_tokens(store.build_context().messages).tokens;
    auto outcome = run_door(store, model, std::move(options));
    REQUIRE(outcome.has_value());
    const auto* result = std::get_if<harness::session::CompactionResult>(&*outcome);
    REQUIRE(result != nullptr);
    REQUIRE(runtime->calls.size() == 1);
    // The update prompt embeds the previous summary verbatim.
    const auto prompt = request_text(runtime->calls.front());
    CHECK(prompt.find("<previous-summary>\nFirst summary") != std::string::npos);
    CHECK(prompt.find("user msg 2") != std::string::npos);
    CHECK(result->tokens_before == expected_tokens);
    CHECK_FALSE(result->retained_tail.empty());
}

TEST_CASE("the compaction door summarizes custom and branch summary entries",
        "[harness][compaction][fixture][issue358][issue541]") {
    // custom_message entries only enter a session through the interoperable
    // wire format (their producers are extensions, a Deferred Capability), so
    // this scenario opens a fixture session instead of appending.
    cch::tests::TempWorkspace workspace;
    const auto source =
            std::filesystem::path{std::string{CCH_SOURCE_DIR}} / "fixtures/pi-agent-core/compaction-door-session.jsonl";
    const auto session_path = workspace.path() / "door-session.jsonl";
    std::error_code copy_error;
    std::filesystem::copy_file(source, session_path, copy_error);
    REQUIRE(!copy_error);
    std::filesystem::permissions(session_path,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace,
            copy_error);
    REQUIRE(!copy_error);
    auto opened = harness::session::SessionStore::open_existing(session_path);
    REQUIRE(opened.has_value());

    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("history"));
    runtime->responses.push_back(ai::assistant_text_message("prefix"));
    const auto model = tests::make_model("gpt-test");
    harness::session::CompactionRunOptions options;
    options.settings = harness::session::CompactionSettings{
            .enabled = true,
            .reserve_tokens = 100,
            .keep_recent_tokens = 1,
    };
    options.summarization_stream = scripted_stream(runtime, model);

    auto outcome = run_door(*opened, model, std::move(options));
    REQUIRE(outcome.has_value());
    const auto* result = std::get_if<harness::session::CompactionResult>(&*outcome);
    REQUIRE(result != nullptr);
    // The branch summary and the custom message land in the summarized
    // history request as converted user-visible text.
    REQUIRE(runtime->calls.size() == 2);
    const auto history_prompt = request_text(runtime->calls.front());
    CHECK(history_prompt.find("branch summary text") != std::string::npos);
    CHECK(history_prompt.find("injected custom content") != std::string::npos);
    CHECK(request_text(runtime->calls[1]).find("recent question") != std::string::npos);
    CHECK(entry_message_text(*opened, result->first_kept_entry_id) == "recent answer");
}

TEST_CASE("summarization requests carry cacheRetention none and a fresh session id",
        "[harness][compaction][fixture][issue358][issue541]") {
    auto runtime = std::make_shared<tests::FakeModelStream>();
    auto summary_response = ai::assistant_text_message("## Goal\nTest summary");
    summary_response.usage = mock_usage(1000, 200);
    runtime->responses.push_back(summary_response);

    auto store = budget_geometry_store();
    const auto model = tests::make_full_thinking_model("gpt-test");
    harness::session::CompactionRunOptions options;
    options.settings = budget_geometry_settings();
    options.thinking_level = "medium";
    options.summarization_stream = scripted_stream(runtime, model);
    options.session_id_factory = sequential_session_ids();

    auto outcome = run_door(store, model, std::move(options));
    REQUIRE(outcome.has_value());
    const auto* result = std::get_if<harness::session::CompactionResult>(&*outcome);
    REQUIRE(result != nullptr);
    CHECK(result->summary.find("## Goal\nTest summary") != std::string::npos);
    REQUIRE(runtime->calls.size() == 1);
    const auto& call = runtime->calls[0];
    CHECK(call.options.cache_retention == ai::CacheRetention::None);
    REQUIRE(call.options.session_id.has_value());
    CHECK(*call.options.session_id == "summarization-session-1");
    CHECK(*call.options.session_id != "session-golden");
    // 0.8 * 2000 = 1600 maxTokens for the history summarization.
    CHECK(call.options.max_tokens == 1600);
    CHECK(call.options.reasoning == ai::ThinkingLevel::Medium);
    // The cut keeps the third user message (100 'A' characters).
    CHECK(entry_message_text(store, result->first_kept_entry_id) == std::string(100, 'A'));

    expect_json_equal(summarization_request_to_json(call), "summarization-request.json");
}

TEST_CASE("the compaction door surfaces summarization-failed, aborted, and missing-stream errors",
        "[harness][compaction][issue358][issue541]") {
    const auto model = tests::make_model("gpt-test");

    // Error terminal → summarization failure with pi's message.
    {
        auto store = budget_geometry_store();
        auto runtime = std::make_shared<tests::FakeModelStream>();
        auto terminal = ai::assistant_text_message("");
        terminal.stop_reason = ai::AssistantStopReason::Error;
        terminal.error_message = "boom";
        runtime->responses.push_back(std::move(terminal));
        harness::session::CompactionRunOptions options;
        options.settings = budget_geometry_settings();
        options.summarization_stream = scripted_stream(runtime, model);
        auto outcome = run_door(store, model, std::move(options));
        REQUIRE_FALSE(outcome.has_value());
        CHECK(outcome.error().code == support::ErrorCode::Stream);
        CHECK(outcome.error().message == "Summarization failed: boom");
    }

    // Aborted terminal → cancelled with pi's message.
    {
        auto store = budget_geometry_store();
        auto runtime = std::make_shared<tests::FakeModelStream>();
        auto terminal = ai::assistant_text_message("");
        terminal.stop_reason = ai::AssistantStopReason::Aborted;
        terminal.error_message = "stopped";
        runtime->responses.push_back(std::move(terminal));
        harness::session::CompactionRunOptions options;
        options.settings = budget_geometry_settings();
        options.summarization_stream = scripted_stream(runtime, model);
        auto outcome = run_door(store, model, std::move(options));
        REQUIRE_FALSE(outcome.has_value());
        CHECK(outcome.error().code == support::ErrorCode::Cancelled);
        CHECK(outcome.error().message == "stopped");
    }

    // A door run without the injected stream seam → validation error.
    {
        auto store = budget_geometry_store();
        harness::session::CompactionRunOptions options;
        options.settings = budget_geometry_settings();
        auto outcome = run_door(store, model, std::move(options));
        REQUIRE_FALSE(outcome.has_value());
        CHECK(outcome.error().code == support::ErrorCode::Validation);
        CHECK(outcome.error().message == "summarization stream is unavailable");
    }
}

TEST_CASE("the compaction door appends pi's file-operation tags and details to the summary",
        "[harness][compaction][issue358][issue541]") {
    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("## Goal\nTest summary"));

    auto store = harness::session::SessionStore::in_memory();
    append_user(store, "read a file");
    ai::AssistantMessage tool_message;
    tool_message.content.push_back(ai::tool_call_content(
        "tool-1", "read", R"({"path":"src/index.ts"})",
        *support::read_json(R"({"path":"src/index.ts"})")));
    tool_message.usage = mock_usage(1000, 200);
    REQUIRE(store.append(std::move(tool_message)).has_value());
    append_user(store, "continue");
    append_assistant(store, "done", mock_usage(4000, 500));

    const auto model = tests::make_model("gpt-test");
    harness::session::CompactionRunOptions options;
    // Budget 2 crosses at the second user message: [u1, a1] is summarized,
    // and the tool call in a1 feeds the file-operation details.
    options.settings = harness::session::CompactionSettings{
            .enabled = true,
            .reserve_tokens = 2000,
            .keep_recent_tokens = 2,
    };
    options.summarization_stream = scripted_stream(runtime, model);

    auto outcome = run_door(store, model, std::move(options));
    REQUIRE(outcome.has_value());
    const auto* result = std::get_if<harness::session::CompactionResult>(&*outcome);
    REQUIRE(result != nullptr);
    CHECK(result->summary.find("<read-files>\nsrc/index.ts\n</read-files>") !=
          std::string::npos);
    REQUIRE(result->details.has_value());
    const auto* details = result->details->get_if<support::JsonValue::object_t>();
    REQUIRE(details != nullptr);
    const auto read_it = details->find("readFiles");
    REQUIRE(read_it != details->end());
    const auto* read_files = read_it->second.get_if<support::JsonValue::array_t>();
    REQUIRE(read_files != nullptr);
    REQUIRE(read_files->size() == 1);
    const auto* name = (*read_files)[0].get_if<std::string>();
    REQUIRE(name != nullptr);
    CHECK(*name == "src/index.ts");
}

TEST_CASE("compaction persistence and rebuild goldens match pi's CompactionEntry wire fields",
        "[harness][compaction][fixture][issue358][issue541]") {
    // Deterministic session history with fixed message timestamps: the cut
    // keeps the last two pairs and summarizes the first (keepRecentTokens
    // 30). Entry ids/timestamps are store-generated; the goldens normalize
    // them like the thinking-persistence.json precedent.
    auto store = harness::session::SessionStore::in_memory();
    const auto append_message = [&store](ai::MessageVariant message) {
        REQUIRE(store.append(std::move(message)).has_value());
    };
    const auto assistant_wire = [](std::string text, ai::Usage usage, ai::TimestampMs timestamp) {
        auto message = ai::assistant_text_message(std::move(text), timestamp);
        message.usage = usage;
        // Assistant message wire DTOs require non-empty api/provider/model.
        message.api = "anthropic-messages";
        message.provider = "anthropic";
        message.model = "claude-sonnet-4-5";
        return ai::MessageVariant{std::move(message)};
    };
    append_message(ai::user_text_message(std::string(100, 'X'), 1718000000000));
    append_message(assistant_wire(std::string(4, 'Y'), mock_usage(100, 50), 1718000001000));
    append_message(ai::user_text_message(std::string(100, 'A'), 1718000002000));
    append_message(assistant_wire(std::string(4, 'B'), mock_usage(100, 50), 1718000003000));
    append_message(ai::user_text_message(std::string(100, 'C'), 1718000004000));
    append_message(assistant_wire(std::string(4, 'D'), mock_usage(100, 50, 0, 0), 1718000005000));

    auto runtime = std::make_shared<tests::FakeModelStream>();
    auto summary_response = ai::assistant_text_message("## Goal\nTest summary");
    summary_response.usage = mock_usage(1000, 200);
    runtime->responses.push_back(std::move(summary_response));

    const auto model = tests::make_full_thinking_model("gpt-test");
    harness::session::CompactionRunOptions options;
    options.settings = budget_geometry_settings();
    options.thinking_level = "medium";
    options.summarization_stream = scripted_stream(runtime, model);
    options.session_id_factory = sequential_session_ids();

    auto outcome = run_door(store, model, std::move(options));
    REQUIRE(outcome.has_value());
    const auto* result = std::get_if<harness::session::CompactionResult>(&*outcome);
    REQUIRE(result != nullptr);

    // Persist into a real JsonlSessionStore file (pi appendCompaction).
    cch::tests::TempWorkspace workspace;
    const auto session_path = workspace.path() / "golden-session.jsonl";
    auto jsonl = harness::session::JsonlSessionStore::create_new(session_path,
            harness::session::SessionMetadata{
                    .session_id = "compaction-golden",
                    .created_at = "2026-08-05T00:00:00Z",
                    .workspace = workspace.path(),
                    .provider = "fake",
                    .model = "gpt-test",
            });
    REQUIRE(jsonl.has_value());
    REQUIRE(jsonl->append_compaction(std::nullopt,
                         harness::session::CompactionEntryValue{
                                 .summary = std::move(result->summary),
                                 .first_kept_entry_id = std::move(result->first_kept_entry_id),
                                 .tokens_before = result->tokens_before,
                                 .retained_tail = std::move(result->retained_tail),
                                 .details = std::move(result->details),
                                 .usage = std::move(result->usage),
                                 .from_hook = false,
                         })
                    .has_value());

    // Persistence golden: the appended compaction line with pi's field set,
    // entry id/timestamp/first-kept id normalized (store-generated values).
    auto loaded = harness::session::JsonlSessionStore::load(session_path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->entries.size() == 2);  // header + compaction
    const auto& persisted_entry = loaded->entries[1];
    CHECK(persisted_entry.kind == harness::session::SessionEntryKind::Compaction);
    auto line_json = support::read_json(persisted_entry.raw_line);
    REQUIRE(line_json.has_value());
    auto& line_object = line_json->get_object();
    line_object.at("id") = support::JsonValue("<compaction-entry-id>");
    line_object.at("timestamp") = support::JsonValue("<compaction-entry-timestamp>");
    line_object.at("firstKeptEntryId") = support::JsonValue("<first-kept-entry-id>");
    expect_json_equal(*line_json, "compaction-persistence.jsonl");
    // Rebuild golden: the rebuilt context is compactionSummary + retained
    // tail (the compactionSummary timestamp follows the normalized entry).
    harness::session::SessionTree tree(std::move(*loaded));
    auto context = tree.buildSessionContext();
    REQUIRE(context.messages.size() == 5);
    CHECK(std::holds_alternative<ai::CompactionSummaryMessage>(context.messages[0]));
    support::JsonValue rebuild{support::JsonValue::array_t{}};
    for (const auto& message : context.messages) {
        auto serialized = support::write_json(ai::glaze::to_message_dto(message));
        REQUIRE(serialized);
        auto parsed = support::read_json(*serialized);
        REQUIRE(parsed);
        if (std::holds_alternative<ai::CompactionSummaryMessage>(message)) {
            parsed->get_object().at("timestamp") =
                support::JsonValue("<compaction-entry-timestamp>");
        }
        rebuild.get_array().push_back(std::move(*parsed));
    }
    expect_json_equal(rebuild, "compaction-rebuild.json");
}

TEST_CASE(
        "the compaction door serializes pi's verbatim conversation text", "[harness][compaction][issue358][issue541]") {
    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("## Summary"));

    auto store = harness::session::SessionStore::in_memory();
    append_user(store, "hello");
    ai::AssistantMessage call_message;
    call_message.content.push_back(ai::tool_call_content("call-1", "read", R"({"path":"big.log"})"));
    call_message.usage = mock_usage(100, 50);
    REQUIRE(store.append(std::move(call_message)).has_value());
    const std::string long_content(5000, 'x');
    REQUIRE(store.append(ai::tool_result_message("call-1", "read", long_content, false)).has_value());
    append_user(store, "recent");
    append_assistant(store, "tail", mock_usage(100, 50));

    const auto model = tests::make_model("gpt-test");
    harness::session::CompactionRunOptions options;
    // Budget 5 crosses at the second user message: [hello, call, result] is
    // summarized, including the over-long tool result.
    options.settings.keep_recent_tokens = 5;
    options.summarization_stream = scripted_stream(runtime, model);

    auto outcome = run_door(store, model, std::move(options));
    REQUIRE(outcome.has_value());
    REQUIRE(std::holds_alternative<harness::session::CompactionResult>(*outcome));
    REQUIRE(runtime->calls.size() == 1);
    const auto prompt = request_text(runtime->calls.front());
    CHECK(prompt.find("[User]: hello") != std::string::npos);
    CHECK(prompt.find("[Tool result]:") != std::string::npos);
    CHECK(prompt.find("[... 3000 more characters truncated]") != std::string::npos);
}

// ── T10 trigger-policy machinery (#359): shouldCompact + isContextOverflow ──

TEST_CASE(
    "shouldCompact mirrors pi's threshold arithmetic",
    "[harness][compaction][issue359]") {
    using harness::session::CompactionSettings;
    using harness::session::should_compact;

    const CompactionSettings defaults;
    // Disabled settings never compact.
    CompactionSettings disabled = defaults;
    disabled.enabled = false;
    CHECK_FALSE(should_compact(100000, 128000, disabled));
    // At the boundary: `contextTokens > contextWindow - reserveTokens` is a
    // strict greater-than (111616 is not over 128000 - 16384 = 111616).
    CHECK_FALSE(should_compact(111616, 128000, defaults));
    CHECK(should_compact(111617, 128000, defaults));
    // Custom reserveTokens shifts the boundary.
    CompactionSettings tight = defaults;
    tight.reserve_tokens = 1024;
    CHECK_FALSE(should_compact(126976, 128000, tight));
    CHECK(should_compact(126977, 128000, tight));
    // An unknown (zero) context window follows pi's JS arithmetic exactly:
    // `contextTokens > 0 - reserveTokens` is true for any context value
    // (pi `contextWindow ?? 0`; catalog models always carry a window, so this
    // only ever bites the C++ placeholders, which the session policy gates on).
    CHECK(should_compact(0, 0, defaults));
    CHECK(should_compact(1, 0, defaults));
    // A window smaller than the reserve still leaves the threshold negative,
    // so any context triggers (pi: `contextWindow - reserveTokens < 0`).
    CHECK(should_compact(1, 4096, defaults));
}

TEST_CASE(
    "isContextOverflow detects provider overflow errors, silent usage overflow, and length-stop overflow",
    "[harness][compaction][issue359]") {
    using harness::session::is_context_overflow;

    constexpr std::size_t kWindow = 128000;

    // Case 1: error terminals matching provider overflow patterns.
    const auto overflow_error = [](std::string message) {
        auto terminal = ai::assistant_text_message("");
        terminal.stop_reason = ai::AssistantStopReason::Error;
        terminal.error_message = std::move(message);
        return terminal;
    };
    CHECK(is_context_overflow(overflow_error("prompt is too long: 213462 tokens > 200000 maximum"), kWindow));
    CHECK(is_context_overflow(overflow_error("413 {\"error\":{\"type\":\"request_too_large\"}}"), kWindow));
    CHECK(is_context_overflow(overflow_error("Your input exceeds the context window of this model"), kWindow));
    CHECK(is_context_overflow(overflow_error("Requested token count exceeds the model's maximum context length of 131072 tokens"), kWindow));
    CHECK(is_context_overflow(overflow_error("Input length (265330) exceeds model's maximum context length (262144)."), kWindow));
    CHECK(is_context_overflow(overflow_error("The input token count (1196265) exceeds the maximum number of tokens allowed (1048575)"), kWindow));
    CHECK(is_context_overflow(overflow_error("This model's maximum prompt length is 131072 but the request contains 537812 tokens"), kWindow));
    CHECK(is_context_overflow(overflow_error("Please reduce the length of the messages or completion"), kWindow));
    CHECK(is_context_overflow(overflow_error("This endpoint's maximum context length is 200000 tokens"), kWindow));
    CHECK(is_context_overflow(overflow_error("Input length 265330 exceeds the maximum allowed input length of 262144 tokens."), kWindow));
    CHECK(is_context_overflow(overflow_error("The input (265330 tokens) is longer than the model's context length (262144 tokens)."), kWindow));
    CHECK(is_context_overflow(overflow_error("prompt token count of 1000 exceeds the limit of 500"), kWindow));
    CHECK(is_context_overflow(overflow_error("the request exceeds the available context size, try increasing it"), kWindow));
    CHECK(is_context_overflow(overflow_error("tokens to keep from the initial prompt is greater than the context length"), kWindow));
    CHECK(is_context_overflow(overflow_error("invalid params, context window exceeds limit"), kWindow));
    CHECK(is_context_overflow(overflow_error("Your request exceeded model token limit: 1000 (requested: 500)"), kWindow));
    CHECK(is_context_overflow(overflow_error("Prompt contains 1000 tokens ... too large for model with 500 maximum context length"), kWindow));
    CHECK(is_context_overflow(overflow_error("Prompt has 1,000 tokens, but the configured context size is 500 tokens"), kWindow));
    CHECK(is_context_overflow(overflow_error("model_context_window_exceeded"), kWindow));
    CHECK(is_context_overflow(overflow_error("prompt too long; exceeded max context length by 100 tokens"), kWindow));
    CHECK(is_context_overflow(overflow_error("Range of input length should be [1, 1000]"), kWindow));
    CHECK(is_context_overflow(overflow_error("request failed: context length exceeded"), kWindow));
    CHECK(is_context_overflow(overflow_error("too many tokens"), kWindow));
    CHECK(is_context_overflow(overflow_error("token limit exceeded"), kWindow));
    CHECK(is_context_overflow(overflow_error("400 (no body)"), kWindow));
    CHECK(is_context_overflow(overflow_error("413 status code (no body)"), kWindow));

    // Non-overflow errors must not match, even when an overflow pattern would
    // (e.g. Bedrock throttling carrying "Too many tokens").
    CHECK_FALSE(is_context_overflow(overflow_error("Throttling error: Too many tokens, please wait before trying again."), kWindow));
    CHECK_FALSE(is_context_overflow(overflow_error("Service unavailable: too many requests"), kWindow));
    CHECK_FALSE(is_context_overflow(overflow_error("Rate limit exceeded: too many tokens"), kWindow));
    CHECK_FALSE(is_context_overflow(overflow_error("a generic stream failure"), kWindow));
    // An overflow-pattern error message without the error terminal is not an
    // overflow error.
    auto plain = ai::assistant_text_message("prompt is too long");
    CHECK_FALSE(is_context_overflow(plain, kWindow));

    // Case 2: silent overflow — a successful response whose input usage
    // exceeds the window (z.ai style).
    auto silent = ai::assistant_text_message("ok");
    silent.stop_reason = ai::AssistantStopReason::Stop;
    silent.usage.input = 129000;
    silent.usage.cache_read = 0;
    silent.usage.total_tokens = 129000;
    CHECK(is_context_overflow(silent, kWindow));
    silent.usage.input = 1000;
    silent.usage.cache_read = 128000;
    CHECK(is_context_overflow(silent, kWindow));
    silent.usage.cache_read = 0;
    CHECK_FALSE(is_context_overflow(silent, kWindow));

    // Case 3: length-stop overflow — the server truncated the input to fill
    // the window, leaving zero room for output (Xiaomi MiMo style).
    auto length = ai::assistant_text_message("");
    length.stop_reason = ai::AssistantStopReason::Length;
    length.usage.input = 127000;
    length.usage.output = 0;
    CHECK(is_context_overflow(length, kWindow));
    length.usage.input = 126000; // >= 99% of 128000 = 126720
    CHECK_FALSE(is_context_overflow(length, kWindow));
    length.usage.input = 127000;
    length.usage.output = 1;
    CHECK_FALSE(is_context_overflow(length, kWindow));

    // An unknown (zero) window disables the usage-based cases but keeps the
    // error-pattern detection (pi's truthiness gate on `contextWindow`).
    CHECK(is_context_overflow(overflow_error("prompt is too long"), 0));
    CHECK_FALSE(is_context_overflow(silent, 0));
    CHECK_FALSE(is_context_overflow(length, 0));
}
