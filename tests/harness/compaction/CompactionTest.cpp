// Compaction machinery evidence for #358 (T09): the harness-module compaction
// machinery mirroring pi `packages/agent/src/harness/compaction/compaction.ts`
// at baseline 83114817 — cut-point selection with keepRecentTokens and
// split-turn handling, token estimation, preparation, and the summarization
// request surface (`cacheRetention: "none"` + a fresh session id) captured
// through the recording fake ModelRuntime. Goldens under
// fixtures/pi-agent-core/ follow the #330 sanitization rules (dummy values
// only, no live credentials).

#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/ai/Model.hpp>
#include <cch/harness/session/JsonlSessionStore.hpp>
#include <cch/harness/session/SessionEntry.hpp>
#include <cch/harness/session/SessionTree.hpp>
#include <cch/util/Error.hpp>
#include "ai/glaze/AiJson.hpp"
#include "harness/compaction/Compaction.hpp"
#include <cch/ai/Models.hpp>
#include "support/FakeModelStream.hpp"
#include "support/ModelFixture.hpp"
#include "support/TempWorkspace.hpp"
#include "util/Json.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <cstddef>
#include <fstream>
#include <iostream>
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

[[nodiscard]] harness::session::SessionEntry user_entry(
    std::string id,
    std::string text,
    ai::TimestampMs timestamp = 0) {
    harness::session::SessionEntry entry;
    entry.kind = harness::session::SessionEntryKind::Message;
    entry.entry_id = std::move(id);
    entry.timestamp = timestamp;
    entry.message = ai::user_text_message(std::move(text), timestamp);
    return entry;
}

[[nodiscard]] harness::session::SessionEntry assistant_entry(
    std::string id,
    std::string text,
    ai::Usage usage,
    ai::TimestampMs timestamp = 0) {
    harness::session::SessionEntry entry;
    entry.kind = harness::session::SessionEntryKind::Message;
    entry.entry_id = std::move(id);
    entry.timestamp = timestamp;
    auto message = ai::assistant_text_message(std::move(text), timestamp);
    message.usage = usage;
    entry.message = std::move(message);
    return entry;
}

[[nodiscard]] harness::session::SessionEntry tool_result_entry(
    std::string id,
    std::string text,
    ai::TimestampMs timestamp = 0) {
    harness::session::SessionEntry entry;
    entry.kind = harness::session::SessionEntryKind::Message;
    entry.entry_id = std::move(id);
    entry.timestamp = timestamp;
    entry.message = ai::tool_result_message("call-1", "read", std::move(text), false, timestamp);
    return entry;
}

[[nodiscard]] harness::session::SessionEntry compaction_entry(
    std::string id,
    std::string summary,
    std::string first_kept_entry_id,
    ai::TimestampMs timestamp = 0) {
    harness::session::SessionEntry entry;
    entry.kind = harness::session::SessionEntryKind::Compaction;
    entry.entry_id = std::move(id);
    entry.timestamp = timestamp;
    entry.value = harness::session::CompactionEntryValue{
        .summary = std::move(summary),
        .first_kept_entry_id = std::move(first_kept_entry_id),
        .tokens_before = 1234,
        .retained_tail = std::nullopt,
        .details = std::nullopt,
        .usage = std::nullopt,
        .from_hook = std::nullopt,
    };
    return entry;
}

[[nodiscard]] std::vector<const harness::session::SessionEntry*> path_of(
    const std::vector<harness::session::SessionEntry>& entries) {
    std::vector<const harness::session::SessionEntry*> path;
    path.reserve(entries.size());
    for (const auto& entry : entries) {
        path.push_back(&entry);
    }
    return path;
}

/// The model-visible side of one recorded summarization request at the fake
/// ModelRuntime seam: the options prove `cacheRetention: "none"` plus the
/// fresh session id, and the context proves the prompt construction.
[[nodiscard]] util::JsonValue summarization_request_to_json(
    const tests::RecordedStreamSimpleCall& call) {
    util::JsonValue object{util::JsonValue::object_t{}};
    auto& o = object.get_object();
    util::JsonValue model_object{util::JsonValue::object_t{}};
    model_object.get_object().emplace("id", util::JsonValue(call.model.id));
    model_object.get_object().emplace(
        "provider", util::JsonValue(call.model.provider));
    o.emplace("model", std::move(model_object));

    if (call.context.system_prompt) {
        o.emplace("systemPrompt", util::JsonValue(*call.context.system_prompt));
    }
    util::JsonValue messages{util::JsonValue::array_t{}};
    for (const auto& message : call.context.messages) {
        auto serialized = util::write_json(ai::glaze::to_message_dto(message));
        REQUIRE(serialized);
        auto parsed = util::read_json(*serialized);
        REQUIRE(parsed);
        messages.get_array().push_back(std::move(*parsed));
    }
    o.emplace("messages", std::move(messages));

    util::JsonValue options{util::JsonValue::object_t{}};
    if (call.options.max_tokens) {
        options.get_object().emplace(
            "maxTokens", util::JsonValue(static_cast<int>(*call.options.max_tokens)));
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
        options.get_object().emplace("cacheRetention", util::JsonValue(retention));
    }
    if (call.options.session_id) {
        options.get_object().emplace(
            "sessionId", util::JsonValue(*call.options.session_id));
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
        options.get_object().emplace("reasoning", util::JsonValue(reasoning));
    }
    o.emplace("options", std::move(options));
    return object;
}

/// A no-op event sink for standalone summarization streams: the recording
/// fake ModelRuntime invokes the sink for non-terminal responses, so the
/// machinery tests supply a passive sink instead of an empty one.
[[nodiscard]] ai::AssistantEventSink noop_sink() {
    return [](const ai::AssistantStreamEvent&) { return util::ExpectedVoid{}; };
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

TEST_CASE(
    "getLastAssistantUsage skips aborted/error/all-zero assistant messages",
    "[harness][compaction][issue358]") {
    using harness::session::get_last_assistant_usage;

    auto usage = mock_usage(100, 50);
    auto valid = [&] {
        ai::AssistantMessage message = ai::assistant_text_message("assistant");
        message.usage = usage;
        return message;
    };

    {
        const auto found = get_last_assistant_usage(
            {ai::MessageVariant{ai::user_text_message("user")},
             ai::MessageVariant{valid()}});
        REQUIRE(found.has_value());
        CHECK(found->input == usage.input);
        CHECK(found->output == usage.output);
    }

    auto aborted = valid();
    aborted.stop_reason = ai::AssistantStopReason::Aborted;
    auto errored = valid();
    errored.stop_reason = ai::AssistantStopReason::Error;
    CHECK(get_last_assistant_usage(
        {ai::MessageVariant{aborted}, ai::MessageVariant{errored}}) == std::nullopt);

    auto partial = valid();
    partial.usage = mock_usage(0, 0);
    {
        const auto found = get_last_assistant_usage(
            {ai::MessageVariant{ai::user_text_message("user")},
             ai::MessageVariant{valid()},
             ai::MessageVariant{partial}});
        REQUIRE(found.has_value());
        CHECK(found->input == usage.input);
    }
}

TEST_CASE(
    "estimateContextTokens uses the last assistant usage plus trailing estimate",
    "[harness][compaction][issue358]") {
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
}

TEST_CASE(
    "findCutPoint keeps the recent-token budget and never cuts at tool results",
    "[harness][compaction][issue358]") {
    using harness::session::find_cut_point;

    // 10 user/assistant pairs; keepRecentTokens lands on a message entry.
    std::vector<harness::session::SessionEntry> entries;
    for (int i = 0; i < 10; ++i) {
        entries.push_back(user_entry("u" + std::to_string(i), "User " + std::to_string(i)));
        entries.push_back(assistant_entry(
            "a" + std::to_string(i),
            "Assistant " + std::to_string(i),
            mock_usage(0, 100, (i + 1) * 1000, 0)));
    }
    auto path = path_of(entries);
    const auto result = find_cut_point(path, 0, path.size(), 2500);
    CHECK(entries[result.first_kept_entry_index].kind ==
          harness::session::SessionEntryKind::Message);

    // A lone tool result is not a valid cut point: the fallback keeps from the
    // start and never splits.
    std::vector<harness::session::SessionEntry> only_result{
        tool_result_entry("tr", "tool output")};
    auto only_path = path_of(only_result);
    CHECK(find_cut_point(only_path, 0, 1, 1).first_kept_entry_index == 0);
    CHECK(find_cut_point(only_path, 0, 1, 1).is_split_turn == false);

    // Cutting at a compaction boundary: the cut never crosses a compaction.
    std::vector<harness::session::SessionEntry> boundary{
        user_entry("u1", "user"),
        compaction_entry("c1", "summary", "u1"),
        assistant_entry("a1", "assistant", mock_usage(100, 50)),
    };
    auto boundary_path = path_of(boundary);
    CHECK(find_cut_point(boundary_path, 0, 3, 1).first_kept_entry_index == 2);
}

TEST_CASE(
    "findCutPoint split-turn handling covers turn-start edge cases",
    "[harness][compaction][issue358]") {
    using harness::session::find_cut_point;
    using harness::session::find_turn_start_index;

    std::vector<harness::session::SessionEntry> entries{
        user_entry("u1", "large turn"),
        assistant_entry("a1", "large assistant message", mock_usage(300, 100)),
        user_entry("u2", "keep me"),
        assistant_entry("a2", "recent", mock_usage(100, 50)),
    };
    auto path = path_of(entries);
    // Walk back: a2 (2) + u2 (2) + a1 (6) = 10 ≥ 10 → cut at the first valid
    // cut point at/after a1, i.e. a1 itself — an assistant message, so the cut
    // splits the turn that u1 started.
    const auto split = find_cut_point(path, 0, 4, 10);
    CHECK(split.first_kept_entry_index == 1);
    CHECK(split.is_split_turn == true);
    CHECK(split.turn_start_index == 0);

    // Cutting directly at a user message never splits: with a 3-token budget
    // the crossing entry is u2 (2 + 2 = 4 ≥ 3) and the cut lands on u2 itself.
    const auto user_cut = find_cut_point(path, 0, 4, 3);
    CHECK(user_cut.first_kept_entry_index == 2);
    CHECK(user_cut.is_split_turn == false);
    CHECK(user_cut.turn_start_index == -1);

    // turn-start scan: only user/bashExecution messages and
    // branch_summary/custom_message entries start turns.
    CHECK(find_turn_start_index(path, 1, 0) == 0);
    CHECK(find_turn_start_index(path, 0, 0) == 0);
    std::vector<harness::session::SessionEntry> meta{
        user_entry("u1", "user"),
        assistant_entry("a1", "assistant", mock_usage(10, 5)),
    };
    auto meta_path = path_of(meta);
    CHECK(find_turn_start_index(meta_path, 1, 0) == 0);
}

TEST_CASE(
    "prepareCompaction uses the latest compaction summary as previousSummary",
    "[harness][compaction][issue358]") {
    using harness::session::prepare_compaction;

    std::vector<harness::session::SessionEntry> entries{
        user_entry("u1", "user msg 1"),
        assistant_entry("a1", "assistant msg 1", mock_usage(5000, 1000)),
        user_entry("u2", "user msg 2"),
        assistant_entry("a2", "assistant msg 2", mock_usage(8000, 2000)),
        compaction_entry("c1", "First summary", "u2"),
        user_entry("u3", "user msg 3"),
        assistant_entry("a3", "assistant msg 3", mock_usage(8000, 2000)),
    };
    auto path = path_of(entries);
    auto preparation = prepare_compaction(
        path, harness::session::kDefaultCompactionSettings);
    REQUIRE(preparation.has_value());
    REQUIRE(preparation->has_value());
    const auto& prep = **preparation;
    CHECK(prep.previous_summary == "First summary");
    CHECK_FALSE(prep.first_kept_entry_id.empty());
    CHECK_FALSE(prep.retained_tail.empty());
    CHECK(prep.tokens_before == harness::session::estimate_context_tokens(
        harness::session::buildSessionContext(path).messages).tokens);
}

TEST_CASE(
    "prepareCompaction covers split turns, prior details, and nothing-to-compact",
    "[harness][compaction][issue358]") {
    using harness::session::prepare_compaction;

    // Split-turn preparation carries prior compaction details plus tool-call
    // file operations from the summarized history.
    harness::session::SessionEntry u1 = user_entry("u1", "user msg 1");
    harness::session::SessionEntry a1 = assistant_entry(
        "a1", "assistant msg 1", mock_usage(100, 50));
    ai::AssistantMessage tool_message;
    tool_message.content.push_back(ai::tool_call_content(
        "tool-1", "write", R"({"path":"written.ts"})",
        *util::read_json(R"({"path":"written.ts"})")));
    tool_message.usage = mock_usage(100, 50);
    a1.message = std::move(tool_message);
    harness::session::SessionEntry c1 = compaction_entry("c1", "First summary", "u1");
    auto* c1_value = std::get_if<harness::session::CompactionEntryValue>(&c1.value);
    c1_value->details = util::JsonValue::object_t{
        {"readFiles", util::JsonValue::array_t{"old-read.ts"}},
        {"modifiedFiles", util::JsonValue::array_t{"old-edit.ts"}},
    };
    harness::session::SessionEntry u2 = user_entry("u2", "large turn");
    harness::session::SessionEntry a2 = assistant_entry(
        "a2", "large assistant message", mock_usage(300, 100));
    std::vector<harness::session::SessionEntry> entries{u1, a1, c1, u2, a2};
    auto path = path_of(entries);
    harness::session::CompactionSettings tiny{
        .enabled = true,
        .reserve_tokens = 100,
        .keep_recent_tokens = 1,
    };
    auto preparation = prepare_compaction(path, tiny);
    REQUIRE(preparation.has_value());
    REQUIRE(preparation->has_value());
    const auto& prep = **preparation;
    CHECK(prep.previous_summary == "First summary");
    CHECK(prep.is_split_turn == true);
    REQUIRE(prep.turn_prefix_messages.size() == 1);
    CHECK(std::holds_alternative<ai::UserMessage>(prep.turn_prefix_messages[0]));
    CHECK(prep.file_ops.read.count("old-read.ts") == 1);
    CHECK(prep.file_ops.edited.count("old-edit.ts") == 1);
    CHECK(prep.file_ops.written.count("written.ts") == 1);

    // Nothing to compact: an empty path and a path ending in compaction.
    CHECK_FALSE(prepare_compaction({}, tiny)->has_value());
    std::vector<harness::session::SessionEntry> already{
        compaction_entry("c1", "already compacted", "entry-keep"),
    };
    auto already_path = path_of(already);
    CHECK_FALSE(prepare_compaction(already_path, tiny)->has_value());
}

TEST_CASE(
    "prepareCompaction summarizes custom and branch summary entries",
    "[harness][compaction][issue358]") {
    using harness::session::prepare_compaction;

    harness::session::SessionEntry branch_summary;
    branch_summary.kind = harness::session::SessionEntryKind::BranchSummary;
    branch_summary.entry_id = "bs1";
    branch_summary.value = harness::session::BranchSummaryEntryValue{
        .from_id = "branch",
        .summary = "branch summary",
        .details = std::nullopt,
        .usage = std::nullopt,
        .from_hook = std::nullopt,
    };
    harness::session::SessionEntry custom_message;
    custom_message.kind = harness::session::SessionEntryKind::CustomMessage;
    custom_message.entry_id = "cm1";
    custom_message.value = harness::session::CustomMessageEntryValue{
        .custom_type = "note",
        .content = std::string{"custom content"},
        .display = true,
        .details = std::nullopt,
    };
    std::vector<harness::session::SessionEntry> entries{
        branch_summary,
        custom_message,
        user_entry("u1", "keep"),
        assistant_entry("a1", "assistant", mock_usage(100, 50)),
    };
    auto path = path_of(entries);
    harness::session::CompactionSettings tiny{
        .enabled = true,
        .reserve_tokens = 100,
        .keep_recent_tokens = 1,
    };
    auto preparation = prepare_compaction(path, tiny);
    REQUIRE(preparation.has_value());
    REQUIRE(preparation->has_value());
    const auto& prep = **preparation;
    REQUIRE(prep.messages_to_summarize.size() == 2);
    CHECK(std::holds_alternative<ai::BranchSummaryMessage>(prep.messages_to_summarize[0]));
    CHECK(std::holds_alternative<ai::CustomMessage>(prep.messages_to_summarize[1]));
}

TEST_CASE(
    "summarization requests carry cacheRetention none and a fresh session id",
    "[harness][compaction][fixture][issue358]") {
    auto runtime = std::make_shared<tests::FakeModelStream>();
    auto summary_response = ai::assistant_text_message("## Goal\nTest summary");
    summary_response.usage = mock_usage(1000, 200);
    runtime->responses.push_back(summary_response);

    std::vector<harness::session::SessionEntry> entries{
        user_entry("u1", std::string(100, 'X')),
        assistant_entry("a1", std::string(4, 'Y'), mock_usage(100, 50)),
        user_entry("u2", std::string(100, 'A')),
        assistant_entry("a2", std::string(4, 'B'), mock_usage(100, 50)),
        user_entry("u3", std::string(100, 'C')),
        assistant_entry("a3", std::string(4, 'D'), mock_usage(100, 50, 0, 0)),
    };
    // keepRecentTokens 30: walk back crosses at u2 (user), so the cut keeps
    // [u2..a3] and summarizes [u1, a1] without splitting a turn.
    harness::session::CompactionSettings settings{
        .enabled = true,
        .reserve_tokens = 2000,
        .keep_recent_tokens = 30,
    };
    auto path = path_of(entries);
    auto preparation = harness::session::prepare_compaction(path, settings);
    REQUIRE(preparation.has_value());
    REQUIRE(preparation->has_value());
    const auto& prep = **preparation;
    CHECK_FALSE(prep.is_split_turn);
    REQUIRE(prep.messages_to_summarize.size() == 2);
    CHECK(prep.first_kept_entry_id == "u2");

    const auto model = tests::make_full_thinking_model("gpt-test");
    harness::session::SummarizationStreamFn stream_fn =
        [runtime, model](
            ai::AiContext context,
            ai::SimpleStreamOptions options)
            -> boost::asio::awaitable<util::Expected<ai::AssistantMessage>> {
        auto stream = runtime->factory()(model, std::move(context), std::move(options));
        co_return co_await ai::detail::await_async_result(std::move(stream).run(noop_sink()));
    };

    harness::session::CompactionRunOptions run_options;
    run_options.thinking_level = "medium";
    run_options.summarization_stream = std::move(stream_fn);
    run_options.session_id_factory = sequential_session_ids();

    auto result = run_awaitable(harness::session::compact(
        prep, model, std::move(run_options)));

    REQUIRE(result.has_value());
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

    expect_json_equal(
        summarization_request_to_json(call),
        "summarization-request.json");
}

TEST_CASE(
    "split-turn compaction issues two requests with distinct fresh session ids",
    "[harness][compaction][issue358]") {
    auto runtime = std::make_shared<tests::FakeModelStream>();
    auto history_response = ai::assistant_text_message("history summary");
    history_response.usage = mock_usage(1000, 200);
    auto prefix_response = ai::assistant_text_message("prefix summary");
    prefix_response.usage = mock_usage(1000, 200);
    runtime->responses.push_back(std::move(history_response));
    runtime->responses.push_back(std::move(prefix_response));

    // A turn too large to keep sits after summarized history, so both the
    // history and the turn-prefix summarization run: cut lands on a1 (the
    // large assistant), splitting the u1/a1 turn, with [u0, a0] summarized as
    // history and u1 summarized as the turn prefix.
    std::vector<harness::session::SessionEntry> entries{
        user_entry("u0", "old history"),
        assistant_entry("a0", "old assistant", mock_usage(300, 100)),
        user_entry("u1", "large turn"),
        assistant_entry("a1", "large assistant message", mock_usage(300, 100)),
        user_entry("u2", "keep me"),
        assistant_entry("a2", "recent", mock_usage(100, 50)),
    };
    auto path = path_of(entries);
    harness::session::CompactionSettings settings{
        .enabled = true,
        .reserve_tokens = 2000,
        .keep_recent_tokens = 10,
    };
    auto preparation = harness::session::prepare_compaction(path, settings);
    REQUIRE(preparation.has_value());
    REQUIRE(preparation->has_value());
    const auto& prep = **preparation;
    CHECK(prep.is_split_turn == true);
    CHECK_FALSE(prep.messages_to_summarize.empty());
    CHECK_FALSE(prep.turn_prefix_messages.empty());

    const auto model = tests::make_model("gpt-test");
    harness::session::SummarizationStreamFn stream_fn =
        [runtime, model](
            ai::AiContext context,
            ai::SimpleStreamOptions options)
            -> boost::asio::awaitable<util::Expected<ai::AssistantMessage>> {
        auto stream = runtime->factory()(model, std::move(context), std::move(options));
        co_return co_await ai::detail::await_async_result(std::move(stream).run(noop_sink()));
    };
    harness::session::CompactionRunOptions run_options;
    run_options.summarization_stream = std::move(stream_fn);
    run_options.session_id_factory = sequential_session_ids();

    auto result = run_awaitable(harness::session::compact(
        prep, model, std::move(run_options)));

    REQUIRE(result.has_value());
    REQUIRE(runtime->calls.size() == 2);
    CHECK(runtime->calls[0].options.cache_retention == ai::CacheRetention::None);
    CHECK(runtime->calls[1].options.cache_retention == ai::CacheRetention::None);
    REQUIRE(runtime->calls[0].options.session_id.has_value());
    REQUIRE(runtime->calls[1].options.session_id.has_value());
    CHECK(*runtime->calls[0].options.session_id == "summarization-session-1");
    CHECK(*runtime->calls[1].options.session_id == "summarization-session-2");
    CHECK(*runtime->calls[0].options.session_id != *runtime->calls[1].options.session_id);
    // The merged summary carries pi's split-turn separator; usage combines
    // both calls.
    CHECK(result->summary.find("**Turn Context (split turn):**") != std::string::npos);
    REQUIRE(result->usage.has_value());
    CHECK(result->usage->input == 1000 + 1000);
    CHECK(result->usage->output == 200 + 200);
}

TEST_CASE(
    "compact returns summarization-failed, aborted, and invalid-session errors",
    "[harness][compaction][issue358]") {
    using harness::session::compact;

    harness::session::CompactionPreparation preparation;
    preparation.first_kept_entry_id = "entry-keep";
    preparation.messages_to_summarize = {
        ai::MessageVariant{ai::user_text_message("Summarize this.")},
    };
    preparation.settings = harness::session::kDefaultCompactionSettings;
    const auto model = tests::make_model("gpt-test");

    auto stream_fn_for = [](std::shared_ptr<tests::FakeModelStream> runtime)
        -> harness::session::SummarizationStreamFn {
        return [runtime](
                   ai::AiContext context,
                   ai::SimpleStreamOptions options)
            -> boost::asio::awaitable<util::Expected<ai::AssistantMessage>> {
            auto stream = runtime->factory()(tests::make_model("gpt-test"), std::move(context), std::move(options));
            co_return co_await ai::detail::await_async_result(std::move(stream).run(noop_sink()));
        };
    };

    // Error terminal → summarization_failed with pi's message.
    {
        auto runtime = std::make_shared<tests::FakeModelStream>();
        auto terminal = ai::assistant_text_message("");
        terminal.stop_reason = ai::AssistantStopReason::Error;
        terminal.error_message = "boom";
        runtime->responses.push_back(std::move(terminal));
        harness::session::CompactionRunOptions options;
        options.summarization_stream = stream_fn_for(runtime);
        auto result = run_awaitable(compact(
            preparation, model, std::move(options)));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == util::ErrorCode::Stream);
        CHECK(result.error().message == "Summarization failed: boom");
    }

    // Aborted terminal → aborted with pi's message.
    {
        auto runtime = std::make_shared<tests::FakeModelStream>();
        auto terminal = ai::assistant_text_message("");
        terminal.stop_reason = ai::AssistantStopReason::Aborted;
        terminal.error_message = "stopped";
        runtime->responses.push_back(std::move(terminal));
        harness::session::CompactionRunOptions options;
        options.summarization_stream = stream_fn_for(runtime);
        auto result = run_awaitable(compact(
            preparation, model, std::move(options)));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == util::ErrorCode::Cancelled);
        CHECK(result.error().message == "stopped");
    }

    // Missing firstKeptEntryId → invalid_session.
    {
        harness::session::CompactionPreparation empty_preparation;
        empty_preparation.settings = harness::session::kDefaultCompactionSettings;
        harness::session::CompactionRunOptions options;
        options.summarization_stream = stream_fn_for(
            std::make_shared<tests::FakeModelStream>());
        auto result = run_awaitable(compact(
            empty_preparation, model, std::move(options)));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == util::ErrorCode::Session);
        CHECK(result.error().message ==
              "First kept entry has no UUID - session may need migration");
    }
}

TEST_CASE(
    "compact appends pi's file-operation tags and details to the summary",
    "[harness][compaction][issue358]") {
    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("## Goal\nTest summary"));

    harness::session::SessionEntry u1 = user_entry("u1", "read a file");
    harness::session::SessionEntry a1 = assistant_entry(
        "a1", "calling tool", mock_usage(1000, 200));
    ai::AssistantMessage tool_message;
    tool_message.content.push_back(ai::tool_call_content(
        "tool-1", "read", R"({"path":"src/index.ts"})",
        *util::read_json(R"({"path":"src/index.ts"})")));
    tool_message.usage = mock_usage(1000, 200);
    a1.message = std::move(tool_message);
    harness::session::SessionEntry u2 = user_entry("u2", "continue");
    harness::session::SessionEntry a2 = assistant_entry(
        "a2", "done", mock_usage(4000, 500));
    std::vector<harness::session::SessionEntry> entries{u1, a1, u2, a2};
    auto path = path_of(entries);

    auto preparation = harness::session::prepare_compaction(
        path, harness::session::CompactionSettings{
            .enabled = true,
            .reserve_tokens = 2000,
            .keep_recent_tokens = 2,
        });
    REQUIRE(preparation.has_value());
    REQUIRE(preparation->has_value());
    const auto& prep = **preparation;
    // keepRecentTokens 2 crosses at u2 (a user message), so the cut keeps
    // [u2, a2] and summarizes [u1, a1] without splitting a turn — the tool
    // call in a1 feeds the file-operation details.
    REQUIRE(prep.messages_to_summarize.size() == 2);

    const auto model = tests::make_model("gpt-test");
    harness::session::SummarizationStreamFn stream_fn =
        [runtime, model](
            ai::AiContext context,
            ai::SimpleStreamOptions options)
            -> boost::asio::awaitable<util::Expected<ai::AssistantMessage>> {
        auto stream = runtime->factory()(model, std::move(context), std::move(options));
        co_return co_await ai::detail::await_async_result(std::move(stream).run(noop_sink()));
    };
    harness::session::CompactionRunOptions run_options;
    run_options.summarization_stream = std::move(stream_fn);

    auto result = run_awaitable(harness::session::compact(
        prep, model, std::move(run_options)));
    REQUIRE(result.has_value());
    CHECK(result->summary.find("<read-files>\nsrc/index.ts\n</read-files>") !=
          std::string::npos);
    REQUIRE(result->details.has_value());
    const auto* details = result->details->get_if<util::JsonValue::object_t>();
    REQUIRE(details != nullptr);
    const auto read_it = details->find("readFiles");
    REQUIRE(read_it != details->end());
    const auto* read_files = read_it->second.get_if<util::JsonValue::array_t>();
    REQUIRE(read_files != nullptr);
    REQUIRE(read_files->size() == 1);
    const auto* name = (*read_files)[0].get_if<std::string>();
    REQUIRE(name != nullptr);
    CHECK(*name == "src/index.ts");
}

TEST_CASE(
    "compaction persistence and rebuild goldens match pi's CompactionEntry wire fields",
    "[harness][compaction][fixture][issue358]") {
    // Deterministic session history with fixed entry ids and timestamps: the
    // cut keeps [u2..a3] and summarizes [u1, a1] (keepRecentTokens 30).
    std::vector<harness::session::SessionEntry> entries{
        user_entry("u1", std::string(100, 'X'), 1718000000000),
        assistant_entry("a1", std::string(4, 'Y'), mock_usage(100, 50), 1718000001000),
        user_entry("u2", std::string(100, 'A'), 1718000002000),
        assistant_entry("a2", std::string(4, 'B'), mock_usage(100, 50), 1718000003000),
        user_entry("u3", std::string(100, 'C'), 1718000004000),
        assistant_entry("a3", std::string(4, 'D'), mock_usage(100, 50, 0, 0), 1718000005000),
    };
    // Assistant message wire DTOs require non-empty api/provider/model.
    for (auto& entry : entries) {
        if (auto* assistant = std::get_if<ai::AssistantMessage>(&*entry.message)) {
            assistant->api = "anthropic-messages";
            assistant->provider = "anthropic";
            assistant->model = "claude-sonnet-4-5";
        }
    }
    auto path = path_of(entries);
    harness::session::CompactionSettings settings{
        .enabled = true,
        .reserve_tokens = 2000,
        .keep_recent_tokens = 30,
    };
    auto preparation = harness::session::prepare_compaction(path, settings);
    REQUIRE(preparation.has_value());
    REQUIRE(preparation->has_value());
    const auto& prep = **preparation;
    CHECK(prep.first_kept_entry_id == "u2");

    auto runtime = std::make_shared<tests::FakeModelStream>();
    auto summary_response = ai::assistant_text_message("## Goal\nTest summary");
    summary_response.usage = mock_usage(1000, 200);
    runtime->responses.push_back(std::move(summary_response));

    const auto model = tests::make_full_thinking_model("gpt-test");
    harness::session::SummarizationStreamFn stream_fn =
        [runtime, model](
            ai::AiContext context,
            ai::SimpleStreamOptions options)
            -> boost::asio::awaitable<util::Expected<ai::AssistantMessage>> {
        auto stream = runtime->factory()(model, std::move(context), std::move(options));
        co_return co_await ai::detail::await_async_result(std::move(stream).run(noop_sink()));
    };
    harness::session::CompactionRunOptions run_options;
    run_options.thinking_level = "medium";
    run_options.summarization_stream = std::move(stream_fn);
    run_options.session_id_factory = sequential_session_ids();
    auto result = run_awaitable(harness::session::compact(
        prep, model, std::move(run_options)));
    REQUIRE(result.has_value());

    // Persist into a real JsonlSessionStore file (pi appendCompaction).
    cch::tests::TempWorkspace workspace;
    const auto session_path = workspace.path() / "golden-session.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(
        session_path,
        harness::session::SessionMetadata{
            .session_id = "compaction-golden",
            .created_at = "2026-08-05T00:00:00Z",
            .workspace = workspace.path(),
            .provider = "fake",
            .model = "gpt-test",
        });
    REQUIRE(store.has_value());
    REQUIRE(store->append_compaction(
        std::nullopt,
        result->summary,
        result->first_kept_entry_id,
        result->tokens_before,
        result->details,
        /*from_hook=*/false,
        result->retained_tail,
        result->usage).has_value());

    // Persistence golden: the appended compaction line with pi's field set,
    // entry id/timestamp normalized (generated values, like the
    // thinking-persistence.json precedent).
    auto loaded = harness::session::JsonlSessionStore::load(session_path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->entries.size() == 2);  // header + compaction
    const auto& compaction_entry = loaded->entries[1];
    CHECK(compaction_entry.kind == harness::session::SessionEntryKind::Compaction);
    auto line_json = util::read_json(compaction_entry.raw_line);
    REQUIRE(line_json.has_value());
    auto& line_object = line_json->get_object();
    line_object.at("id") = util::JsonValue("<compaction-entry-id>");
    line_object.at("timestamp") = util::JsonValue("<compaction-entry-timestamp>");
    expect_json_equal(*line_json, "compaction-persistence.jsonl");

    // Rebuild golden: the rebuilt context is compactionSummary + retained
    // tail (the compactionSummary timestamp follows the normalized entry).
    auto tree = harness::session::JsonlSessionStore::open_as_tree(session_path);
    REQUIRE(tree.has_value());
    auto context = tree->buildSessionContext();
    REQUIRE(context.messages.size() == 5);
    CHECK(std::holds_alternative<ai::CompactionSummaryMessage>(context.messages[0]));
    util::JsonValue rebuild{util::JsonValue::array_t{}};
    for (const auto& message : context.messages) {
        auto serialized = util::write_json(ai::glaze::to_message_dto(message));
        REQUIRE(serialized);
        auto parsed = util::read_json(*serialized);
        REQUIRE(parsed);
        if (std::holds_alternative<ai::CompactionSummaryMessage>(message)) {
            parsed->get_object().at("timestamp") =
                util::JsonValue("<compaction-entry-timestamp>");
        }
        rebuild.get_array().push_back(std::move(*parsed));
    }
    expect_json_equal(rebuild, "compaction-rebuild.json");
}

TEST_CASE(
    "serializeConversation formats pi's verbatim summarization text",
    "[harness][compaction][issue358]") {
    using harness::session::serialize_conversation;

    const std::string long_content(5000, 'x');
    auto text = serialize_conversation({
        ai::MessageVariant{ai::user_text_message("hello")},
        ai::MessageVariant{ai::tool_result_message("tc1", "read", long_content)},
    });
    CHECK(text.find("[User]: hello") != std::string::npos);
    CHECK(text.find("[Tool result]:") != std::string::npos);
    CHECK(text.find("[... 3000 more characters truncated]") != std::string::npos);
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
