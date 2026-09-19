#pragma once

#include "support/ScriptedProvider.hpp"
#include "support/StreamAdapterFixture.hpp"

#include <cch/ai/Models.hpp>

#include "ai/SimpleOptions.hpp"
#include "ai/providers/EnvApiKeyAuth.hpp"

#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tests {

/// Shared scenario vocabulary for the adapter/wire tests (#721): one run
/// harness, fixture loading, event-name rendering, and scripted-provider
/// wiring, replacing the per-file copies the Responses / Anthropic / Codex
/// adapter test files each carried.

struct RunResult {
    support::Expected<ai::AssistantMessage> result;
    std::vector<ai::AssistantStreamEvent> events;
};

/// Run one Models stream to its terminal outcome, collecting every event.
[[nodiscard]] inline RunResult run_models(
        ai::Models& models, const ai::Model& model, ai::AiContext context, ai::SimpleStreamOptions options) {
    std::vector<ai::AssistantStreamEvent> events;
    auto stream = models.stream(model, std::move(context), std::move(options));
    auto result = run_async_result(
            std::move(stream).run([&events](const ai::AssistantStreamEvent& event) -> support::ExpectedVoid {
                events.push_back(event);
                return {};
            }));
    return RunResult{.result = std::move(result), .events = std::move(events)};
}

/// Load a frozen pi wire fixture relative to the repository root; the ai test
/// targets define CCH_SOURCE_DIR.
[[nodiscard]] inline std::string read_fixture_text(std::string_view relative_path) {
    const std::string path = std::string{CCH_SOURCE_DIR} + "/fixtures/pi-ai/" + std::string{relative_path};
    std::ifstream input(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] inline std::string event_name(const ai::AssistantStreamEvent& event) {
    if (std::holds_alternative<ai::AssistantStartEvent>(event)) return "start";
    if (std::holds_alternative<ai::ThinkingStartEvent>(event)) return "thinking_start";
    if (std::holds_alternative<ai::ThinkingDeltaEvent>(event)) return "thinking_delta";
    if (std::holds_alternative<ai::ThinkingEndEvent>(event)) return "thinking_end";
    if (std::holds_alternative<ai::TextStartEvent>(event)) return "text_start";
    if (std::holds_alternative<ai::TextDeltaEvent>(event)) return "text_delta";
    if (std::holds_alternative<ai::TextEndEvent>(event)) return "text_end";
    if (std::holds_alternative<ai::ToolCallStartEvent>(event)) return "toolcall_start";
    if (std::holds_alternative<ai::ToolCallDeltaEvent>(event)) return "toolcall_delta";
    if (std::holds_alternative<ai::ToolCallEndEvent>(event)) return "toolcall_end";
    if (std::holds_alternative<ai::AssistantDoneEvent>(event)) return "done";
    return "error";
}

[[nodiscard]] inline std::vector<std::string> event_names(const std::vector<ai::AssistantStreamEvent>& events) {
    std::vector<std::string> names;
    for (const auto& event : events) {
        names.push_back(event_name(event));
    }
    return names;
}

/// Wire one env-api-key scripted provider (id == name == model.provider) into
/// a fresh Models runtime. Returns nullptr when installation fails.
[[nodiscard]] inline std::shared_ptr<ai::Models> make_scripted_models(
        const ai::Model& model, ScriptedTransportOptions transport = {}) {
    auto models = std::make_shared<ai::Models>(
            std::make_shared<EmptyCredentialStore>(), std::make_shared<EmptyAuthContext>());
    ScriptedProviderDefinition definition;
    definition.definition = ai::ProviderDefinition{
            .id = std::string{model.provider},
            .name = std::string{model.provider},
            .models = {model},
            .auth = ai::providers::make_env_api_key_auth("API key", {}),
    };
    definition.transport = std::move(transport);
    if (auto registered = apply_scripted_provider(*models, std::move(definition)); !registered) {
        return nullptr;
    }
    return models;
}

} // namespace cch::tests
