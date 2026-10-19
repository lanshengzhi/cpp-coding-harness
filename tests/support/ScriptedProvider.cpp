#include "support/ScriptedProvider.hpp"

#include <cch/ai/Content.hpp>
#include <cch/ai/Models.hpp>
#include "ai/ModelStreamBridge.hpp"
#include "ai/ModelsTestAccess.hpp"
#include "ai/providers/RetryPolicy.hpp"
#include "ai/providers/StreamEmit.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/Json.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::tests {

// The scripted provider is installed through cch_ai's private provider seam; this
// file names that seam explicitly instead of living inside it.
namespace providers = cch::ai::providers;
namespace {

void set_fake_metadata(ai::AssistantMessage& assistant, const ai::Model& model) {
    assistant.model = model.id;
    assistant.provider = "fake";
    assistant.api = "scripted-fake";
    assistant.usage = ai::Usage{};
}

[[nodiscard]] support::Expected<std::string> make_tool_arguments(std::string key, std::string value) {
    support::JsonValue::object_t object;
    object.emplace(std::move(key), support::JsonValue{std::move(value)});
    return support::write_json(support::JsonValue{std::move(object)});
}

struct FakeToolCallSpec {
    std::string id;
    std::string name;
    std::string argument_key;
    std::string argument_value;
    std::string announcement;
};

[[nodiscard]] support::ExpectedVoid emit_complete_lifecycle(
        const ai::AssistantMessage& final_message, ai::AssistantEventSink& sink) {
    auto partial = final_message;
    partial.content.clear();

    if (auto emitted = providers::emit(sink, ai::AssistantStartEvent{.partial = partial}); !emitted) {
        return std::unexpected(emitted.error());
    }

    for (std::size_t content_index = 0; content_index < final_message.content.size(); ++content_index) {
        const auto& completed = final_message.content[content_index];
        if (const auto* text = std::get_if<ai::TextContent>(&completed)) {
            partial.content.emplace_back(ai::TextContent{
                    .text = "",
                    .text_signature = std::nullopt,
            });
            if (auto emitted = providers::emit(sink,
                        ai::TextStartEvent{
                                .content_index = content_index,
                                .partial = partial,
                        });
                    !emitted) {
                return std::unexpected(emitted.error());
            }
            if (!text->text.empty()) {
                std::get<ai::TextContent>(partial.content[content_index]).text = text->text;
                if (auto emitted = providers::emit(sink,
                            ai::TextDeltaEvent{
                                    .content_index = content_index,
                                    .delta = text->text,
                                    .partial = partial,
                            });
                        !emitted) {
                    return std::unexpected(emitted.error());
                }
            }
            partial.content[content_index] = *text;
            if (auto emitted = providers::emit(sink,
                        ai::TextEndEvent{
                                .content_index = content_index,
                                .content = text->text,
                                .partial = partial,
                        });
                    !emitted) {
                return std::unexpected(emitted.error());
            }
            continue;
        }

        const auto& tool_call = std::get<ai::ToolCallContent>(completed);
        partial.content.emplace_back(ai::ToolCallContent{
                .id = tool_call.id,
                .name = tool_call.name,
                .arguments = support::JsonValue{support::JsonValue::object_t{}},
                .raw_arguments = {},
                .thought_signature = std::nullopt,
                .arguments_valid = true,
                .argument_error = std::nullopt,
        });
        if (auto emitted = providers::emit(sink,
                    ai::ToolCallStartEvent{
                            .content_index = content_index,
                            .partial = partial,
                    });
                !emitted) {
            return std::unexpected(emitted.error());
        }
        if (!tool_call.raw_arguments.empty()) {
            auto& streaming_call = std::get<ai::ToolCallContent>(partial.content[content_index]);
            streaming_call.raw_arguments += tool_call.raw_arguments;
            if (auto emitted = providers::emit(sink,
                        ai::ToolCallDeltaEvent{
                                .content_index = content_index,
                                .delta = tool_call.raw_arguments,
                                .partial = partial,
                        });
                    !emitted) {
                return std::unexpected(emitted.error());
            }
        }
        partial.content[content_index] = tool_call;
        if (auto emitted = providers::emit(sink,
                    ai::ToolCallEndEvent{
                            .content_index = content_index,
                            .tool_call = tool_call,
                            .partial = partial,
                    });
                !emitted) {
            return std::unexpected(emitted.error());
        }
    }

    return providers::emit(sink,
            ai::AssistantDoneEvent{
                    .reason = final_message.stop_reason,
                    .message = final_message,
            });
}

[[nodiscard]] support::ExpectedVoid respond_with_tool_call(
        ai::AssistantMessage& assistant, ai::AssistantEventSink& sink, FakeToolCallSpec spec) {
    auto raw = make_tool_arguments(std::move(spec.argument_key), std::move(spec.argument_value));
    if (!raw) {
        return std::unexpected(raw.error());
    }
    auto args = support::read_json(*raw);
    ai::ToolCallContent call;
    call.id = std::move(spec.id);
    call.name = std::move(spec.name);
    call.raw_arguments = *raw;
    if (args) {
        call.arguments = *args;
    }
    assistant.content.emplace_back(ai::text_content(std::move(spec.announcement)));
    assistant.content.emplace_back(std::move(call));
    assistant.stop_reason = ai::AssistantStopReason::ToolUse;
    return emit_complete_lifecycle(assistant, sink);
}

[[nodiscard]] std::optional<support::ErrorCode> scripted_failure_code(std::string_view prompt) {
    if (prompt == "fail model_source") {
        return support::ErrorCode::ModelSource;
    }
    if (prompt == "fail model_validation") {
        return support::ErrorCode::ModelValidation;
    }
    if (prompt == "fail provider") {
        return support::ErrorCode::Provider;
    }
    if (prompt == "fail stream") {
        return support::ErrorCode::Stream;
    }
    if (prompt == "fail auth") {
        return support::ErrorCode::Auth;
    }
    if (prompt == "fail oauth") {
        return support::ErrorCode::OAuth;
    }
    return std::nullopt;
}

} // namespace

support::ExpectedVoid apply_scripted_provider(ai::Models& models, ScriptedProviderDefinition definition) {
    auto catalog = models.models();
    for (auto& model : definition.definition.models) {
        catalog.push_back(std::move(model));
    }
    if (auto replaced = ai::detail::ModelsTestAccess::replace_models(models, std::move(catalog)); !replaced) {
        return replaced;
    }
    if (definition.transport.http_transport) {
        ai::detail::ModelsTestAccess::set_transport(models, std::move(definition.transport.http_transport));
    }
    if (definition.stream) {
        auto previous = ai::detail::ModelsTestAccess::take_stream(models);
        ai::detail::ModelsTestAccess::set_stream(models,
                [id = std::move(definition.definition.id),
                        stream = std::move(definition.stream),
                        previous = std::move(previous)](ai::Model model,
                        ai::AiContext context,
                        ai::SimpleStreamOptions options) mutable -> ai::ModelStream {
                    if (model.provider == id) {
                        return stream(std::move(model), std::move(context), std::move(options));
                    }
                    if (previous) {
                        return previous(std::move(model), std::move(context), std::move(options));
                    }
                    return ai::detail::make_model_stream(
                            [](ai::AssistantEventSink)
                                    -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
                                co_return std::unexpected(support::make_error(
                                        support::ErrorCode::Provider, "No scripted stream for model identity"));
                            });
                });
    }
    return {};
}

support::ExpectedVoid apply_scripted_transport_options(ai::Models& models, ScriptedTransportOptions options) {
    if (!options.http_transport) {
        return {};
    }
    ai::detail::ModelsTestAccess::set_transport(models, std::move(options.http_transport));
    return {};
}

ScriptedProviderDefinition make_ai_scripted_fake_provider_definition(std::string provider_id) {
    ScriptedProviderDefinition definition;
    definition.definition.id = provider_id;
    definition.definition.name = "Fake";
    definition.stream = [](ai::Model model, ai::AiContext context, ai::SimpleStreamOptions options) {
        return ai::detail::make_model_stream(
                [model = std::move(model), context = std::move(context), options = std::move(options)](
                        ai::AssistantEventSink sink)
                        -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
                    ai::AssistantMessage assistant;
                    set_fake_metadata(assistant, model);
                    assistant.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                                                  .count();

                    if (options.stop_token.stop_requested()) {
                        assistant.stop_reason = ai::AssistantStopReason::Aborted;
                        assistant.error_message = "Request was aborted";
                        auto failure = support::make_error(support::ErrorCode::Cancelled, *assistant.error_message);
                        if (auto emitted = providers::emit(sink,
                                    ai::AssistantErrorEvent{
                                            .reason = assistant.stop_reason,
                                            .error = assistant,
                                            .failure = std::move(failure),
                                            .inference_failure =
                                                    ai::InferenceFailure{
                                                            .kind = ai::InferenceFailureKind::Cancelled,
                                                            .output_started = false,
                                                    },
                                    });
                                !emitted) {
                            co_return std::unexpected(emitted.error());
                        }
                        co_return assistant;
                    }

                    if (!context.messages.empty()) {
                        if (const auto* last_tool = std::get_if<ai::ToolResultMessage>(&context.messages.back())) {
                            assistant.content.emplace_back(
                                    ai::text_content("fake observed: " + ai::text_from_content(last_tool->content)));
                            CCH_TRY_VOID(emit_complete_lifecycle(assistant, sink));
                            co_return assistant;
                        }
                    }

                    std::string prompt;
                    for (auto it = context.messages.rbegin(); it != context.messages.rend(); ++it) {
                        if (const auto* user = std::get_if<ai::UserMessage>(&*it)) {
                            prompt = ai::text_from_user_message(*user);
                            break;
                        }
                    }

                    if (const auto failure_code = scripted_failure_code(prompt)) {
                        assistant.stop_reason = ai::AssistantStopReason::Error;
                        assistant.error_message =
                                "scripted fake " + std::string{support::to_string(*failure_code)} + " failure";
                        auto failure = support::make_error(*failure_code, *assistant.error_message);
                        if (auto emitted = providers::emit(sink,
                                    ai::AssistantErrorEvent{
                                            .reason = assistant.stop_reason,
                                            .error = assistant,
                                            .failure = std::move(failure),
                                            .inference_failure =
                                                    ai::InferenceFailure{
                                                            .kind = providers::inference_failure_kind_from_transport(
                                                                    *failure_code),
                                                            .output_started = false,
                                                    },
                                    });
                                !emitted) {
                            co_return std::unexpected(emitted.error());
                        }
                        co_return assistant;
                    }

                    if (prompt.starts_with("read ")) {
                        const auto path = prompt.substr(5);
                        CCH_TRY_VOID(respond_with_tool_call(assistant,
                                sink,
                                FakeToolCallSpec{
                                        .id = "fake-read-1",
                                        .name = "read",
                                        .argument_key = "path",
                                        .argument_value = path,
                                        .announcement = "reading " + path,
                                }));
                        co_return assistant;
                    }
                    if (prompt.starts_with("bash ")) {
                        CCH_TRY_VOID(respond_with_tool_call(assistant,
                                sink,
                                FakeToolCallSpec{
                                        .id = "fake-bash-1",
                                        .name = "bash",
                                        .argument_key = "command",
                                        .argument_value = prompt.substr(5),
                                        .announcement = "running bash",
                                }));
                        co_return assistant;
                    }

                    assistant.content.emplace_back(ai::text_content("fake: " + prompt));
                    CCH_TRY_VOID(emit_complete_lifecycle(assistant, sink));
                    co_return assistant;
                });
    };
    return definition;
}

std::vector<ScriptedProviderDefinition> make_ai_scripted_fake_provider_definitions() {
    std::vector<ScriptedProviderDefinition> definitions;
    definitions.push_back(make_ai_scripted_fake_provider_definition());
    definitions.push_back(make_ai_scripted_fake_provider_definition("sdk-host"));
    return definitions;
}

std::shared_ptr<ai::Models> make_ai_scripted_fake_models() {
    auto models = std::make_shared<ai::Models>();
    for (auto&& definition : make_ai_scripted_fake_provider_definitions()) {
        if (auto added = apply_scripted_provider(*models, std::move(definition)); !added) {
            return nullptr;
        }
    }
    return models;
}

} // namespace cch::tests
