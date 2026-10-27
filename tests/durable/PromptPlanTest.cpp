#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/ai/Models.hpp>
#include <cch/durable/Harness.hpp>
#include <cch/durable/JsonlStorage.hpp>
#include <cch/durable/Session.hpp>
#include <cch/durable/Workflow.hpp>
#include <cch/support/JsonValue.hpp>

#include "durable/FrozenExpectation.hpp"
#include "durable/Outcome.hpp"
#include "support/Json.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tests::durable {
using namespace cch::durable;

namespace {

using support::JsonValue;

JsonValue json_object(std::initializer_list<JsonValue::object_t::value_type> members) {
    return JsonValue{JsonValue::object_t{members}};
}

JsonValue json_array(JsonValue::array_t values) { return JsonValue{std::move(values)}; }

JsonValue json_string(std::string value) { return JsonValue{std::move(value)}; }

/// One `pi.system` entry's sections (as ordered `[name, value]` pairs, with
/// `null` removals) and tool deltas, matching the frozen artifact's shape.
JsonValue observe_system_entry(const EntryRecord& entry) {
    REQUIRE(!entry.model.empty());
    const auto* message = entry.model.front().as_object();
    REQUIRE(message != nullptr);
    JsonValue::array_t sections;
    if (const auto found = message->find("sections"); found != message->end()) {
        const auto* fields = found->second.as_object();
        REQUIRE(fields != nullptr);
        for (const auto& [name, value] : *fields) {
            JsonValue::array_t pair{json_string(name), value.is_null() ? JsonValue{} : json_string(*value.as_string())};
            sections.push_back(json_array(std::move(pair)));
        }
    }
    const auto tool_names = [message](std::string_view field) {
        JsonValue::array_t names;
        const auto found = message->find(field);
        if (found == message->end()) return json_array(std::move(names));
        const auto* values = found->second.as_array();
        REQUIRE(values != nullptr);
        for (const auto& value : *values) {
            const auto* declaration = value.as_object();
            REQUIRE(declaration != nullptr);
            const auto name = declaration->find("name");
            REQUIRE(name != declaration->end());
            names.push_back(json_string(*name->second.as_string()));
        }
        return json_array(std::move(names));
    };
    return json_object({
            {"sections", json_array(std::move(sections))},
            {"toolsRemoved", tool_names("toolsRemoved")},
            {"toolsAdded", tool_names("toolsAdded")},
    });
}

JsonValue observe_entries(const std::vector<EntryRecord>& entries) {
    JsonValue::array_t systems;
    for (const auto& entry : entries)
        if (entry.kind == "pi.system") systems.push_back(observe_system_entry(entry));
    return json_array(std::move(systems));
}

std::string user_text(const ai::UserMessage& message) {
    if (const auto* text = std::get_if<std::string>(&message.content)) return *text;
    return ai::text_from_content(std::get<std::vector<ai::Content>>(message.content));
}

/// The effective request the model sees: the replayed system prompt and tool
/// order plus the non-system transcript messages, without volatile timestamps.
JsonValue observe_request(const ai::AiContext& context) {
    JsonValue::array_t messages;
    for (const auto& message : context.messages) {
        if (const auto* user = std::get_if<ai::UserMessage>(&message)) {
            messages.push_back(json_object({{"role", json_string("user")}, {"text", json_string(user_text(*user))}}));
        } else if (const auto* assistant = std::get_if<ai::AssistantMessage>(&message)) {
            messages.push_back(json_object({{"role", json_string("assistant")},
                    {"text", json_string(ai::text_from_assistant_content(assistant->content))}}));
        }
    }
    JsonValue::array_t tools;
    for (const auto& tool : context.tools)
        tools.push_back(json_string(tool.name));
    return json_object({
            {"systemPrompt", json_string(context.system_prompt.value_or(""))},
            {"tools", json_array(std::move(tools))},
            {"messages", json_array(std::move(messages))},
    });
}

struct Observations {
    std::mutex mutex;
    std::vector<JsonValue> requests;
};

JsonValue requests_so_far(Observations& observations, std::size_t count) {
    std::lock_guard lock(observations.mutex);
    JsonValue::array_t requests;
    for (std::size_t index = 0; index < count && index < observations.requests.size(); ++index)
        requests.push_back(observations.requests[index]);
    return json_array(std::move(requests));
}

ai::ModelStream answer_stream() {
    return ai::ModelStream{
            ai::ModelStreamProducer{[](ai::AssistantEventSink sink, ai::ModelStreamCompletion done) noexcept {
                auto answer = ai::assistant_text_message("ok");
                auto result = sink(ai::AssistantDoneEvent{.reason = answer.stop_reason, .message = answer});
                if (!result)
                    done(std::unexpected(result.error()));
                else
                    done(std::move(answer));
            }}};
}

void submit(Harness& harness, ConversationId conversation, std::string text) {
    auto admitted = consume(harness.submit(
            conversation, SubmissionDraft{.body = InputSubmissionDraft{.content = chord::Json{std::move(text)}}}));
    REQUIRE(admitted.has_value());
    auto settled = consume(admitted.value().wait());
    REQUIRE(settled.has_value());
    REQUIRE(settled.value().status == "done");
}

std::vector<EntryRecord> entries_oldest_first(Storage& storage, ConversationId conversation) {
    auto entries = storage.scan_entries(conversation, 100);
    REQUIRE(entries.has_value());
    std::ranges::reverse(*entries);
    return std::move(*entries);
}

void check_observation(const JsonValue& actual, const support::JsonValue* expected, std::string_view label) {
    REQUIRE(expected != nullptr);
    const auto actual_text = support::canonical_json(actual);
    const auto expected_text = support::canonical_json(*expected);
    REQUIRE(actual_text.has_value());
    REQUIRE(expected_text.has_value());
    INFO(label);
    CHECK(*actual_text == *expected_text);
}

/// Drive the same three-generation sequence the frozen artifact captured: one
/// configuration, a registry reorder between the first and second generation,
/// then a third generation that must plan nothing.
void run_reorder_scenario(const FrozenScenario& frozen,
        const std::function<void(Registry&)>& install,
        const std::function<void(Registry&)>& reorder) {
    TempWorkspace workspace;
    auto opened = JsonlStorage::open(workspace.path());
    REQUIRE(opened.has_value());
    std::unique_ptr<JsonlStorage> storage = std::move(*opened);

    Registry registry;
    install(registry);
    auto observations = std::make_shared<Observations>();
    HarnessOptions options{.registry = &registry};
    options.settings.compaction.enabled = false;
    options.stream = [observations](ai::Model, ai::AiContext context, ai::SimpleStreamOptions) {
        {
            std::lock_guard lock(observations->mutex);
            observations->requests.push_back(observe_request(context));
        }
        return answer_stream();
    };
    auto opened_harness = consume(Harness::open(*storage, std::move(options)));
    REQUIRE(opened_harness.has_value());
    auto harness = std::move(opened_harness.value());
    auto root = consume(harness->root(RootOptions{.model = ai::Model{.id = "model", .provider = "test"}}));
    REQUIRE(root.has_value());
    REQUIRE(harness->resume().has_value());

    submit(*harness, root.value(), "u1");
    const auto first = json_object({
            {"systemEntries", observe_entries(entries_oldest_first(*storage, root.value()))},
            {"requests", requests_so_far(*observations, 1)},
    });

    reorder(registry);
    submit(*harness, root.value(), "u2");
    const auto second = json_object({
            {"systemEntries", observe_entries(entries_oldest_first(*storage, root.value()))},
            {"requests", requests_so_far(*observations, 2)},
    });

    submit(*harness, root.value(), "u3");
    const auto third = json_object({
            {"systemEntries", observe_entries(entries_oldest_first(*storage, root.value()))},
            {"requests", requests_so_far(*observations, 3)},
    });

    check_observation(first, frozen.member("first"), "first generation");
    check_observation(second, frozen.member("second"), "second generation");
    check_observation(third, frozen.member("third"), "third generation");
    const auto expected_count = frozen.number_member("systemEntryCount");
    REQUIRE(expected_count.has_value());
    CHECK(*expected_count == static_cast<double>(std::ranges::count_if(entries_oldest_first(*storage, root.value()),
                                     [](const auto& entry) { return entry.kind == "pi.system"; })));

    REQUIRE(consume(harness->close()).has_value());

    // A fresh parse of the committed bytes is what the planner reads back; the
    // in-memory record alone would not prove the section order survived storage.
    auto reopened = JsonlStorage::open(workspace.path());
    REQUIRE(reopened.has_value());
    std::unique_ptr<JsonlStorage> reopened_storage = std::move(*reopened);
    const auto* third_expected = frozen.member("third");
    REQUIRE(third_expected != nullptr);
    const auto* third_object = third_expected->get_if<JsonValue::object_t>();
    REQUIRE(third_object != nullptr);
    const auto third_entries = third_object->find("systemEntries");
    REQUIRE(third_entries != third_object->end());
    check_observation(observe_entries(entries_oldest_first(*reopened_storage, root.value())),
            &third_entries->second,
            "reopened third generation");
}

Extension ordered_sections(std::string first, std::string second) {
    const auto text = [](const std::string& key) { return key == "a" ? "1" : "2"; };
    const std::string first_text = text(first);
    const std::string second_text = text(second);
    Extension extension{.name = "ordered-sections"};
    extension.sections.push_back(PromptSection{.key = std::move(first), .text = first_text, .tag = false});
    extension.sections.push_back(PromptSection{.key = std::move(second), .text = second_text, .tag = false});
    return extension;
}

ToolRegistration tool_declaration(std::string name) {
    return ToolRegistration{.declaration = ai::Tool{.name = name,
                                    .description = name + " tool",
                                    .parameters = support::JsonValue{support::JsonValue::object_t{}}},
            .execute = [](support::JsonValue, std::stop_token) {
                return support::AsyncResult<std::vector<ai::Content>>{std::vector<ai::Content>{ai::text_content("ok")}};
            }};
}

Extension ordered_tools(std::string first, std::string second) {
    Extension extension{.name = "ordered-tools"};
    extension.tools.push_back(tool_declaration(std::move(first)));
    extension.tools.push_back(tool_declaration(std::move(second)));
    return extension;
}

} // namespace

TEST_CASE("generation removes and re-adds sections whose replayed order changed",
        "[durable][workflow][issue1151][spec]") {
    const auto frozen = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1151/prompt-plan.json",
            "reordered sections are removed and re-added in the desired order");
    REQUIRE(frozen.has_value());
    run_reorder_scenario(
            *frozen,
            [](Registry& registry) { REQUIRE(registry.install(ordered_sections("a", "b")).has_value()); },
            [](Registry& registry) {
                registry.uninstall("ordered-sections");
                REQUIRE(registry.install(ordered_sections("b", "a")).has_value());
            });
}

TEST_CASE("generation removes and re-adds tools whose replayed order changed", "[durable][workflow][issue1151][spec]") {
    const auto frozen = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1151/prompt-plan.json",
            "reordered tools are removed and re-added in the desired order");
    REQUIRE(frozen.has_value());
    run_reorder_scenario(
            *frozen,
            [](Registry& registry) { REQUIRE(registry.install(ordered_tools("x", "y")).has_value()); },
            [](Registry& registry) {
                registry.uninstall("ordered-tools");
                REQUIRE(registry.install(ordered_tools("y", "x")).has_value());
            });
}

TEST_CASE("generation rides tool changes on the last reordered-section entry", "[durable][workflow][issue1151][spec]") {
    const auto frozen = FrozenScenario::load(
            "fixtures/pi-v1.3.0/audit-1151/prompt-plan.json", "section and tool reorders share the last planned entry");
    REQUIRE(frozen.has_value());
    run_reorder_scenario(
            *frozen,
            [](Registry& registry) {
                REQUIRE(registry.install(ordered_sections("a", "b")).has_value());
                REQUIRE(registry.install(ordered_tools("x", "y")).has_value());
            },
            [](Registry& registry) {
                registry.uninstall("ordered-sections");
                registry.uninstall("ordered-tools");
                REQUIRE(registry.install(ordered_sections("b", "a")).has_value());
                REQUIRE(registry.install(ordered_tools("y", "x")).has_value());
            });
}

TEST_CASE("generation patches changed and removed sections and tools without reordering",
        "[durable][workflow][issue1151][spec]") {
    const auto frozen = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1151/prompt-plan.json",
            "changed and removed sections and tools produce one minimal patch");
    REQUIRE(frozen.has_value());
    run_reorder_scenario(
            *frozen,
            [](Registry& registry) {
                REQUIRE(registry.install(ordered_sections("a", "b")).has_value());
                REQUIRE(registry.install(ordered_tools("x", "y")).has_value());
            },
            [](Registry& registry) {
                registry.uninstall("ordered-sections");
                registry.uninstall("ordered-tools");
                Extension sections{.name = "ordered-sections"};
                sections.sections.push_back(PromptSection{.key = "a", .text = "one", .tag = false});
                REQUIRE(registry.install(std::move(sections)).has_value());
                Extension tools{.name = "ordered-tools"};
                auto changed = tool_declaration("x");
                changed.declaration.description = "x changed";
                tools.tools.push_back(std::move(changed));
                tools.tools.push_back(tool_declaration("z"));
                REQUIRE(registry.install(std::move(tools)).has_value());
            });
}

} // namespace cch::tests::durable
