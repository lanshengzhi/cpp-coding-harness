// T07 (#356): the pi v3 session wire contract — all eleven entry types
#include "ai/AsyncResultBridge.hpp"
// round-trip byte-identically against a golden captured from the frozen pi
// tests (fixtures/pi-agent-core/session-roundtrip.jsonl), and context
// projection per entry type matches pi (custom omitted by default,
// custom_message → CustomMessage, branch_summary → branchSummary message,
// compaction → compactionSummary + retained tail, label → getLabel,
// session_info → session name). Projection is driven into the Agent through
// the fake-ModelRuntime seam so the model sees exactly what pi's model sees.

#include <cch/agent/Agent.hpp>
#include <cch/agent/harness/session/JsonlSessionStore.hpp>
#include <cch/agent/harness/session/SessionTree.hpp>
#include <cch/ai/Message.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include "agent/AgentLoop.hpp"
#include "ai/glaze/AiJson.hpp"
#include "agent/harness/session/EntrySerializer.hpp"
#include "support/FakeModelStream.hpp"
#include "support/ModelFixture.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/Json.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
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

[[nodiscard]] std::vector<std::string> fixture_lines(std::string_view name) {
    std::vector<std::string> lines;
    std::string line;
    std::istringstream stream(read_fixture_text(name));
    while (std::getline(stream, line)) {
        lines.push_back(std::move(line));
    }
    return lines;
}

/// Canonical message projection for comparison: both the pi-captured context
/// JSON and the C++ `MessageDto` serialization are reduced to the same stable
/// object (keys are map-sorted, so input key order never matters). pi custom
/// messages carry `content` as a plain string; the C++ `CustomMessage` carries
/// a text block — both normalize to a `[{type:"text",text}]` block array.
[[nodiscard]] support::JsonValue canonical_message(const support::JsonValue& message) {
    const auto* object = message.get_if<support::JsonValue::object_t>();
    REQUIRE(object != nullptr);
    support::JsonValue::object_t out;
    const auto copy_field = [&](const char* key) {
        if (const auto it = object->find(key); it != object->end()) {
            out.emplace(key, it->second);
        }
    };
    copy_field("role");
    copy_field("api");
    copy_field("provider");
    copy_field("model");
    copy_field("stopReason");
    copy_field("customType");
    copy_field("display");
    copy_field("summary");
    copy_field("fromId");
    copy_field("tokensBefore");
    copy_field("details");
    copy_field("timestamp");

    if (const auto it = object->find("content"); it != object->end()) {
        support::JsonValue::array_t blocks;
        if (const auto* text = it->second.get_if<std::string>()) {
            blocks.push_back(support::JsonValue{support::JsonValue::object_t{
                {"type", support::JsonValue{"text"}},
                {"text", support::JsonValue{*text}},
            }});
        } else if (const auto* array = it->second.get_if<support::JsonValue::array_t>()) {
            for (const auto& block : *array) {
                const auto* block_object = block.get_if<support::JsonValue::object_t>();
                REQUIRE(block_object != nullptr);
                support::JsonValue::object_t normalized;
                for (const char* key : {"type", "text", "data", "mimeType"}) {
                    if (const auto found = block_object->find(key);
                        found != block_object->end()) {
                        normalized.emplace(key, found->second);
                    }
                }
                blocks.push_back(support::JsonValue{std::move(normalized)});
            }
        }
        out.emplace("content", support::JsonValue{std::move(blocks)});
    }
    return support::JsonValue{std::move(out)};
}

[[nodiscard]] support::JsonValue canonical_messages(const support::JsonValue& messages) {
    const auto* array = messages.get_if<support::JsonValue::array_t>();
    REQUIRE(array != nullptr);
    support::JsonValue::array_t out;
    for (const auto& message : *array) {
        out.push_back(canonical_message(message));
    }
    return support::JsonValue{std::move(out)};
}

[[nodiscard]] support::JsonValue cpp_message_json(const ai::MessageVariant& message) {
    auto serialized = support::write_json(ai::glaze::to_message_dto(message));
    REQUIRE(serialized);
    auto parsed = support::read_json(*serialized);
    REQUIRE(parsed);
    return std::move(*parsed);
}

[[nodiscard]] support::JsonValue cpp_messages_canonical(const std::vector<ai::MessageVariant>& messages) {
    support::JsonValue::array_t out;
    for (const auto& message : messages) {
        out.push_back(canonical_message(cpp_message_json(message)));
    }
    return support::JsonValue{std::move(out)};
}

[[nodiscard]] std::string canonical_json(const support::JsonValue& value) {
    auto serialized = support::write_json(value);
    REQUIRE(serialized);
    return *serialized;
}

/// pi `convertToLlm`-equivalent applied to C++ context messages — the exact
/// per-role conversion the provider adapters run (MessageConversion.cpp uses
/// these same Message.hpp helpers), producing what the model sees.
[[nodiscard]] std::vector<ai::MessageVariant> to_llm_messages(
    const std::vector<ai::MessageVariant>& messages) {
    std::vector<ai::MessageVariant> converted;
    converted.reserve(messages.size());
    for (const auto& message : messages) {
        if (const auto* bash = std::get_if<ai::BashExecutionMessage>(&message)) {
            if (!bash->exclude_from_context) {
                converted.push_back(ai::bash_execution_to_user_message(*bash));
            }
        } else if (const auto* custom = std::get_if<ai::CustomMessage>(&message)) {
            converted.push_back(ai::custom_message_to_user_message(*custom));
        } else if (const auto* branch = std::get_if<ai::BranchSummaryMessage>(&message)) {
            converted.push_back(ai::branch_summary_to_user_message(*branch));
        } else if (const auto* compaction = std::get_if<ai::CompactionSummaryMessage>(&message)) {
            converted.push_back(ai::compaction_summary_to_user_message(*compaction));
        } else {
            converted.push_back(message);
        }
    }
    return converted;
}

support::ExpectedVoid run_prompt(agent::Agent& subject, std::string prompt) {
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> result;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await ai::detail::await_async_result(subject.prompt(std::move(prompt)));
            co_return;
        },
        boost::asio::detached);
    io.run();
    if (!result) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "agent prompt coroutine did not complete"));
    }
    return std::move(*result);
}

} // namespace

TEST_CASE("pi v3 11-entry-type session golden round-trips byte-identically",
          "[harness][session][issue356][golden]") {
    harness::session::EntrySerializer serializer;
    auto lines = fixture_lines("session-roundtrip.jsonl");
    REQUIRE(lines.size() > 1);

    auto loaded = serializer.parse_lines(lines);
    REQUIRE(loaded);

    // The header line parses into pi's v3 metadata fields.
    CHECK(loaded->metadata.session_id == "session-11-types");
    CHECK_FALSE(loaded->metadata.created_at.empty());
    CHECK(loaded->metadata.workspace == "/workspace");
    REQUIRE(loaded->entries.size() == lines.size());
    CHECK(loaded->entries[0].kind == harness::session::SessionEntryKind::Header);
    CHECK(loaded->unknown_lines.empty());

    // All eleven entry types are present.
    std::set<harness::session::SessionEntryKind> kinds;
    for (std::size_t i = 1; i < loaded->entries.size(); ++i) {
        kinds.insert(loaded->entries[i].kind);
        const auto& entry = loaded->entries[i];
        auto round_trip = serializer.serialize_entry(entry);
        REQUIRE(round_trip);
        REQUIRE(*round_trip == entry.raw_line + '\n');
    }
    const std::set<harness::session::SessionEntryKind> expected_kinds{
        harness::session::SessionEntryKind::Message,
        harness::session::SessionEntryKind::ModelChange,
        harness::session::SessionEntryKind::ThinkingLevelChange,
        harness::session::SessionEntryKind::ActiveToolsChange,
        harness::session::SessionEntryKind::Compaction,
        harness::session::SessionEntryKind::BranchSummary,
        harness::session::SessionEntryKind::Custom,
        harness::session::SessionEntryKind::CustomMessage,
        harness::session::SessionEntryKind::Label,
        harness::session::SessionEntryKind::SessionInfo,
        harness::session::SessionEntryKind::Leaf,
    };
    CHECK(kinds == expected_kinds);
}

TEST_CASE("pi v3 golden parses field/null/active-path semantics",
          "[harness][session][issue356]") {
    harness::session::EntrySerializer serializer;
    auto loaded = serializer.parse_lines(fixture_lines("session-roundtrip.jsonl"));
    REQUIRE(loaded);

    // Root entry: parentId is an explicit JSON null, not an absent field.
    REQUIRE(loaded->entries.size() > 1);
    CHECK_FALSE(loaded->entries[1].parent_id.has_value());
    CHECK(loaded->entries[1].raw_line.find(R"("parentId":null)") != std::string::npos);

    // active_tools_change uses pi's activeToolNames wire field.
    const harness::session::SessionEntry* tools_entry = nullptr;
    // compaction variants: full (retainedTail + usage + fromHook) and minimal
    // (firstKeptEntryId absent).
    const harness::session::SessionEntry* compaction_full = nullptr;
    const harness::session::SessionEntry* compaction_minimal = nullptr;
    // leaf variant with explicit null target (moveTo(null)).
    const harness::session::SessionEntry* root_leaf = nullptr;
    for (const auto& entry : loaded->entries) {
        switch (entry.kind) {
        case harness::session::SessionEntryKind::ActiveToolsChange:
            tools_entry = &entry;
            break;
        case harness::session::SessionEntryKind::Compaction:
            if (std::get<harness::session::CompactionEntryValue>(entry.value).retained_tail.has_value()) {
                compaction_full = &entry;
            } else {
                compaction_minimal = &entry;
            }
            break;
        case harness::session::SessionEntryKind::Leaf: {
            const auto& leaf = std::get<harness::session::LeafEntryValue>(entry.value);
            if (!leaf.target_id.has_value()) {
                root_leaf = &entry;
            }
            break;
        }
        default:
            break;
        }
    }

    REQUIRE(tools_entry != nullptr);
    const auto& tools = std::get<harness::session::ActiveToolsChangeValue>(tools_entry->value);
    const std::vector<std::string> expected_tools{"read", "bash", "edit", "write"};
    CHECK(tools.active_tool_names == expected_tools);
    CHECK(tools_entry->raw_line.find(R"("activeToolNames":["read","bash","edit","write"])") != std::string::npos);

    REQUIRE(compaction_full != nullptr);
    const auto& full = std::get<harness::session::CompactionEntryValue>(compaction_full->value);
    REQUIRE(full.retained_tail.has_value());
    CHECK(full.retained_tail->size() == 2);
    REQUIRE(full.first_kept_entry_id.has_value());
    REQUIRE(full.usage.has_value());
    CHECK(full.usage->input == 1);
    CHECK(full.usage->cost.total == 0.1);
    REQUIRE(full.from_hook.has_value());
    CHECK(*full.from_hook == true);

    REQUIRE(compaction_minimal != nullptr);
    const auto& minimal = std::get<harness::session::CompactionEntryValue>(compaction_minimal->value);
    CHECK_FALSE(minimal.first_kept_entry_id.has_value());
    CHECK_FALSE(minimal.retained_tail.has_value());
    CHECK_FALSE(minimal.usage.has_value());
    CHECK(minimal.tokens_before == 1234);

    REQUIRE(root_leaf != nullptr);
    CHECK(root_leaf->raw_line.find(R"("targetId":null)") != std::string::npos);

    // Custom entry: absent data vs explicit null both round-trip (covered
    // byte-exactly above); spot-check the explicit-null line parses as an
    // engaged null value.
    const harness::session::SessionEntry* null_data_custom = nullptr;
    for (const auto& entry : loaded->entries) {
        if (entry.kind != harness::session::SessionEntryKind::Custom) {
            continue;
        }
        const auto& custom = std::get<harness::session::CustomEntryValue>(entry.value);
        if (custom.data.has_value() &&
            custom.data->get_if<support::JsonValue::null_t>() != nullptr) {
            null_data_custom = &entry;
            break;
        }
    }
    REQUIRE(null_data_custom != nullptr);
    CHECK(null_data_custom->raw_line.find(R"("data":null)") != std::string::npos);
}

TEST_CASE("pi context projection: compaction retainedTail, custom omitted, custom_message",
          "[harness][session][issue356][projection]") {
    harness::session::EntrySerializer serializer;
    auto loaded = serializer.parse_lines(fixture_lines("projection-session.jsonl"));
    REQUIRE(loaded);

    harness::session::SessionTree tree(std::move(*loaded));
    auto context = tree.buildSessionContext();

    const auto expected = support::read_json(
        read_fixture_text("projection-context.json"));
    REQUIRE(expected);
    const auto actual = cpp_messages_canonical(context.messages);
    const auto expected_canonical = canonical_messages(*expected);

    auto actual_json = canonical_json(actual);
    auto expected_json = canonical_json(expected_canonical);
    if (actual_json != expected_json) {
        std::cerr << "\n[SessionRoundTripGoldenTest] projection mismatch\n--- expected ---\n"
                  << expected_json << "\n--- actual ---\n" << actual_json << "\n--- end ---\n";
    }
    CHECK(actual_json == expected_json);
}

TEST_CASE("pi context projection: branch_summary, custom omitted, custom_message",
          "[harness][session][issue356][projection]") {
    harness::session::EntrySerializer serializer;
    auto loaded = serializer.parse_lines(fixture_lines("projection-branch-session.jsonl"));
    REQUIRE(loaded);

    harness::session::SessionTree tree(std::move(*loaded));
    auto context = tree.buildSessionContext();

    const auto expected = support::read_json(
        read_fixture_text("projection-branch-context.json"));
    REQUIRE(expected);
    const auto actual = cpp_messages_canonical(context.messages);
    const auto expected_canonical = canonical_messages(*expected);

    auto actual_json = canonical_json(actual);
    auto expected_json = canonical_json(expected_canonical);
    if (actual_json != expected_json) {
        std::cerr << "\n[SessionRoundTripGoldenTest] branch projection mismatch\n--- expected ---\n"
                  << expected_json << "\n--- actual ---\n" << actual_json << "\n--- end ---\n";
    }
    CHECK(actual_json == expected_json);
}

TEST_CASE("pi session projection: getLabel and getSessionName",
          "[harness][session][issue356]") {
    harness::session::EntrySerializer serializer;
    auto loaded = serializer.parse_lines(fixture_lines("session-roundtrip.jsonl"));
    REQUIRE(loaded);
    harness::session::SessionTree tree(std::move(*loaded));

    // Label was set to "checkpoint" then cleared (undefined label) — pi's
    // last-wins projection leaves the target unlabeled.
    CHECK_FALSE(tree.get_label("a220aa34").has_value());
    // Session name is the sanitized last session_info name.
    REQUIRE(tree.get_session_name().has_value());
    CHECK(*tree.get_session_name() == "review name");

    // A set label on a target without a later clear resolves to the trimmed
    // label, and a non-target id resolves to nothing.
    auto set = serializer.parse_lines(fixture_lines("projection-session.jsonl"));
    REQUIRE(set);
    harness::session::SessionTree labeled(std::move(*set));
    CHECK_FALSE(labeled.get_label("5d680edc").has_value());
    CHECK_FALSE(labeled.get_session_name().has_value());
}

TEST_CASE("pi projection drives rebuilt context into the Agent at the fake-ModelRuntime seam",
          "[harness][session][issue356][agent][projection]") {
    harness::session::EntrySerializer serializer;
    auto loaded = serializer.parse_lines(fixture_lines("projection-session.jsonl"));
    REQUIRE(loaded);
    harness::session::SessionTree tree(std::move(*loaded));
    auto context = tree.buildSessionContext();

    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("hello user"));
    agent::ToolRegistry tools;
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    options.session_id = "session-1";
    agent::Agent subject(runtime->factory(), std::move(tools), std::move(options),
        agent::AgentInitialState{.messages = context.messages});

    REQUIRE(run_prompt(subject, "hi"));

    REQUIRE(runtime->calls.size() == 1);
    const auto& recorded = runtime->calls[0].context.messages;
    REQUIRE(recorded.size() == context.messages.size() + 1);

    // The session context arrives at the seam exactly as pi's buildContext
    // projects it: compactionSummary + retained tail, custom omitted,
    // custom_message → CustomMessage, then the prompt.
    std::vector<ai::MessageVariant> session_messages(
        recorded.begin(), recorded.end() - 1);
    const auto expected = support::read_json(
        read_fixture_text("projection-context.json"));
    REQUIRE(expected);
    auto actual_json = canonical_json(cpp_messages_canonical(session_messages));
    auto expected_json = canonical_json(canonical_messages(*expected));
    if (actual_json != expected_json) {
        std::cerr << "\n[SessionRoundTripGoldenTest] seam projection mismatch\n--- expected ---\n"
                  << expected_json << "\n--- actual ---\n" << actual_json << "\n--- end ---\n";
    }
    CHECK(actual_json == expected_json);

    // The model-visible messages (pi convertToLlm semantics, the same
    // conversion the provider adapters run) match pi's captured LLM view.
    const auto expected_llm = support::read_json(
        read_fixture_text("projection-context-llm.json"));
    REQUIRE(expected_llm);
    const auto llm_messages = to_llm_messages(session_messages);
    auto llm_json = canonical_json(cpp_messages_canonical(llm_messages));
    auto expected_llm_json = canonical_json(canonical_messages(*expected_llm));
    if (llm_json != expected_llm_json) {
        std::cerr << "\n[SessionRoundTripGoldenTest] LLM projection mismatch\n--- expected ---\n"
                  << expected_llm_json << "\n--- actual ---\n" << llm_json << "\n--- end ---\n";
    }
    CHECK(llm_json == expected_llm_json);
}

TEST_CASE("pi branch projection drives rebuilt context into the Agent",
          "[harness][session][issue356][agent][projection]") {
    harness::session::EntrySerializer serializer;
    auto loaded = serializer.parse_lines(fixture_lines("projection-branch-session.jsonl"));
    REQUIRE(loaded);
    harness::session::SessionTree tree(std::move(*loaded));
    auto context = tree.buildSessionContext();

    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("hello user"));
    agent::ToolRegistry tools;
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    options.model = tests::make_model("gpt-test");
    agent::Agent subject(runtime->factory(), std::move(tools), std::move(options),
        agent::AgentInitialState{.messages = context.messages});

    REQUIRE(run_prompt(subject, "hi"));

    REQUIRE(runtime->calls.size() == 1);
    const auto& recorded = runtime->calls[0].context.messages;
    REQUIRE(recorded.size() == context.messages.size() + 1);
    std::vector<ai::MessageVariant> session_messages(
        recorded.begin(), recorded.end() - 1);

    const auto expected = support::read_json(
        read_fixture_text("projection-branch-context.json"));
    REQUIRE(expected);
    auto actual_json = canonical_json(cpp_messages_canonical(session_messages));
    auto expected_json = canonical_json(canonical_messages(*expected));
    if (actual_json != expected_json) {
        std::cerr << "\n[SessionRoundTripGoldenTest] branch seam mismatch\n--- expected ---\n"
                  << expected_json << "\n--- actual ---\n" << actual_json << "\n--- end ---\n";
    }
    CHECK(actual_json == expected_json);

    const auto expected_llm = support::read_json(
        read_fixture_text("projection-branch-context-llm.json"));
    REQUIRE(expected_llm);
    const auto llm_messages = to_llm_messages(session_messages);
    auto llm_json = canonical_json(cpp_messages_canonical(llm_messages));
    auto expected_llm_json = canonical_json(canonical_messages(*expected_llm));
    if (llm_json != expected_llm_json) {
        std::cerr << "\n[SessionRoundTripGoldenTest] branch LLM mismatch\n--- expected ---\n"
                  << expected_llm_json << "\n--- actual ---\n" << llm_json << "\n--- end ---\n";
    }
    CHECK(llm_json == expected_llm_json);
}

// ── T08 (#357): derived session state — thinkingLevel / model / activeToolNames ──

namespace {

/// Serialize the pi-derived session state (`thinkingLevel`, `model
/// {provider, modelId} | null`, `activeToolNames | null`) into the stable JSON
/// object the committed golden pins.
[[nodiscard]] support::JsonValue derived_state_to_json(
    const harness::session::SessionContext& context) {
    support::JsonValue::object_t out;
    out.emplace("thinkingLevel", support::JsonValue{context.thinking_level});
    if (context.provider.has_value() && context.model.has_value()) {
        support::JsonValue::object_t model;
        model.emplace("provider", support::JsonValue{*context.provider});
        model.emplace("modelId", support::JsonValue{*context.model});
        out.emplace("model", support::JsonValue{std::move(model)});
    } else {
        out.emplace("model", support::JsonValue{nullptr});
    }
    if (context.active_tool_names.has_value()) {
        support::JsonValue::array_t tools;
        for (const auto& name : *context.active_tool_names) {
            tools.push_back(support::JsonValue{name});
        }
        out.emplace("activeToolNames", support::JsonValue{std::move(tools)});
    } else {
        out.emplace("activeToolNames", support::JsonValue{nullptr});
    }
    return support::JsonValue{std::move(out)};
}

/// One linear branch with a user root, a `model_change` (openai/gpt-4.1), a
/// later assistant message (anthropic/claude-sonnet-4-5, pi's
/// `createAssistantMessage` defaults), a `thinking_level_change` (high), and
/// an `active_tools_change` — the pi harness "tracks model and thinking level
/// changes in built context" scenario plus active tools.
[[nodiscard]] harness::session::SessionContext derived_state_full_branch() {
    harness::session::LoadedSession loaded;
    loaded.metadata = harness::session::SessionMetadata{
        .session_id = "derived-state",
        .created_at = "2026-08-05T00:00:00Z",
        .workspace = "/workspace",
        .provider = "fake",
        .model = "fake-model",
    };

    harness::session::SessionEntry user;
    user.kind = harness::session::SessionEntryKind::Message;
    user.entry_id = "user0001";
    user.timestamp = 1784678401000;
    user.message = ai::MessageVariant{ai::user_text_message("1")};
    loaded.entries.push_back(std::move(user));

    harness::session::SessionEntry model;
    model.kind = harness::session::SessionEntryKind::ModelChange;
    model.entry_id = "model001";
    model.parent_id = "user0001";
    model.value = harness::session::ModelChangeValue{.provider = "openai", .model_id = "gpt-4.1"};
    loaded.entries.push_back(std::move(model));

    harness::session::SessionEntry assistant;
    assistant.kind = harness::session::SessionEntryKind::Message;
    assistant.entry_id = "msg0002";
    assistant.parent_id = "model001";
    assistant.timestamp = 1784678402000;
    auto assistant_message = ai::assistant_text_message("a");
    assistant_message.api = "anthropic-messages";
    assistant_message.provider = "anthropic";
    assistant_message.model = "claude-sonnet-4-5";
    assistant_message.timestamp = 1784678402000;
    assistant.message = ai::MessageVariant{std::move(assistant_message)};
    loaded.entries.push_back(std::move(assistant));

    harness::session::SessionEntry thinking;
    thinking.kind = harness::session::SessionEntryKind::ThinkingLevelChange;
    thinking.entry_id = "think001";
    thinking.parent_id = "msg0002";
    thinking.value = harness::session::ThinkingLevelChangeValue{.thinking_level = "high"};
    loaded.entries.push_back(std::move(thinking));

    harness::session::SessionEntry tools;
    tools.kind = harness::session::SessionEntryKind::ActiveToolsChange;
    tools.entry_id = "tools001";
    tools.parent_id = "think001";
    tools.value = harness::session::ActiveToolsChangeValue{
        .active_tool_names = {"read", "bash", "edit", "write"}};
    loaded.entries.push_back(std::move(tools));

    harness::session::SessionTree tree(std::move(loaded));
    return tree.buildSessionContext();
}

} // namespace

TEST_CASE(
    "derived session state golden pins thinkingLevel/model/activeToolNames",
    "[harness][session][issue357][golden]") {
    const auto full_branch = derived_state_full_branch();
    // The assistant message lands after the model_change, so its provider/model
    // wins (pi harness test "tracks model and thinking level changes in built
    // context": loaded.model === the assistant's anthropic/claude-sonnet-4-5).
    CHECK(full_branch.thinking_level == "high");
    REQUIRE(full_branch.provider.has_value());
    CHECK(*full_branch.provider == "anthropic");
    REQUIRE(full_branch.model.has_value());
    CHECK(*full_branch.model == "claude-sonnet-4-5");
    REQUIRE(full_branch.active_tool_names.has_value());

    // The empty-branch defaults come from a header-only tree.
    harness::session::LoadedSession empty;
    empty.metadata = harness::session::SessionMetadata{
        .session_id = "derived-state-empty",
        .created_at = "2026-08-05T00:00:00Z",
        .workspace = "/workspace",
        .provider = "fake",
        .model = "fake-model",
    };
    harness::session::SessionTree empty_tree(std::move(empty));
    const auto empty_context = empty_tree.buildSessionContext();
    CHECK(empty_context.thinking_level == "off");
    CHECK_FALSE(empty_context.provider.has_value());
    CHECK_FALSE(empty_context.model.has_value());
    CHECK_FALSE(empty_context.active_tool_names.has_value());

    support::JsonValue golden{support::JsonValue::object_t{}};
    golden.get_object().emplace("fullBranch", derived_state_to_json(full_branch));
    golden.get_object().emplace("defaults", derived_state_to_json(empty_context));

    auto serialized = support::write_json(golden);
    REQUIRE(serialized);
    const auto expected = read_fixture_text("derived-session-state.json");
    if (*serialized != expected) {
        std::cerr << "\n[SessionRoundTripGoldenTest] fixture mismatch: derived-session-state.json"
                  << "\n--- expected ---\n"
                  << expected << "\n--- actual ---\n"
                  << *serialized << "\n--- end ---\n";
    }
    CHECK(*serialized == expected);
}

TEST_CASE(
    "derived thinking level and model flow into the Agent's turn options at the fake-ModelRuntime seam",
    "[harness][session][issue357][agent][projection]") {
    // The derived state of a resumed branch feeds the Agent construction: the
    // derived model identity becomes the per-turn model and the derived
    // thinking level becomes the per-turn `reasoning` stream option.
    const auto context = derived_state_full_branch();
    REQUIRE(context.provider.has_value());
    REQUIRE(context.model.has_value());
    REQUIRE(context.thinking_level == "high");

    auto runtime = std::make_shared<tests::FakeModelStream>();
    runtime->responses.push_back(ai::assistant_text_message("hello user"));
    agent::ToolRegistry tools;
    agent::AsyncAgentOptions options;
    options.max_turns = 3;
    // The re-resolved model identity (the derived provider/model with a full
    // thinking map so the derived "high" level survives the creation clamp).
    auto resolved_model = tests::make_full_thinking_model(*context.model);
    resolved_model.provider = *context.provider;
    options.model = std::move(resolved_model);
    options.session_id = "session-derived";
    agent::Agent subject(runtime->factory(), std::move(tools), std::move(options),
        agent::AgentInitialState{
            .messages = context.messages,
            .thinking_level = context.thinking_level,
        });

    REQUIRE(run_prompt(subject, "hi"));

    REQUIRE(runtime->calls.size() == 1);
    const auto& call = runtime->calls[0];
    // The derived model identity (from the last assistant message on the
    // branch) is the recorded per-turn model.
    CHECK(call.model.id == *context.model);
    CHECK(call.model.provider == *context.provider);
    // The derived thinking level is the recorded per-turn `reasoning` option
    // (pi `createLoopConfig`: any non-"off" level forwards as reasoning).
    REQUIRE(call.options.reasoning.has_value());
    CHECK(*call.options.reasoning == ai::ThinkingLevel::High);
}
