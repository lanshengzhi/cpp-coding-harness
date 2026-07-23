#include "FakeChatClient.hpp"

#include "../../../src/util/ExpectedMacros.hpp"
#include "../../../include/cch/ai/ChatClient.hpp"
#include "../../../include/cch/ai/Content.hpp"
#include "util/Json.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cch::ai::providers {
namespace {

void set_fake_metadata(ai::AssistantMessage& assistant, const std::string& model) {
    assistant.model = model;
    assistant.provider = "fake";
    assistant.api = "scripted-fake";
    assistant.usage = ai::Usage{};
}

[[nodiscard]] util::Expected<std::string> make_tool_arguments(std::string key, std::string value) {
    util::JsonValue::object_t object;
    object.emplace(std::move(key), util::JsonValue{std::move(value)});
    return util::write_json(util::JsonValue{std::move(object)});
}

[[nodiscard]] util::ExpectedVoid emit(
    ai::AssistantEventSink& sink,
    const ai::AssistantStreamEvent& event) {
    if (!sink) {
        return {};
    }
    return sink(event);
}

[[nodiscard]] util::ExpectedVoid emit_complete_lifecycle(
    const ai::AssistantMessage& final_message,
    ai::AssistantEventSink& sink) {
    auto partial = final_message;
    partial.content.clear();

    if (auto emitted = emit(sink, ai::AssistantStartEvent{partial}); !emitted) {
        return std::unexpected(emitted.error());
    }

    for (std::size_t content_index = 0;
         content_index < final_message.content.size();
         ++content_index) {
        const auto& completed = final_message.content[content_index];
        if (const auto* text = std::get_if<ai::TextContent>(&completed)) {
            partial.content.emplace_back(ai::TextContent{"", std::nullopt});
            if (auto emitted = emit(
                    sink,
                    ai::TextStartEvent{content_index, partial});
                !emitted) {
                return std::unexpected(emitted.error());
            }
            if (!text->text.empty()) {
                std::get<ai::TextContent>(partial.content[content_index]).text = text->text;
                if (auto emitted = emit(
                        sink,
                        ai::TextDeltaEvent{content_index, text->text, partial});
                    !emitted) {
                    return std::unexpected(emitted.error());
                }
            }
            partial.content[content_index] = *text;
            if (auto emitted = emit(
                    sink,
                    ai::TextEndEvent{content_index, text->text, partial});
                !emitted) {
                return std::unexpected(emitted.error());
            }
            continue;
        }

        const auto& tool_call = std::get<ai::ToolCallContent>(completed);
        partial.content.emplace_back(ai::ToolCallContent{
            .id = tool_call.id,
            .name = tool_call.name,
            .arguments = util::JsonValue{util::JsonValue::object_t{}},
            .raw_arguments = {},
            .thought_signature = std::nullopt,
            .arguments_valid = true,
            .argument_error = std::nullopt,
        });
        if (auto emitted = emit(
                sink,
                ai::ToolCallStartEvent{content_index, partial});
            !emitted) {
            return std::unexpected(emitted.error());
        }
        if (!tool_call.raw_arguments.empty()) {
            auto& streaming_call =
                std::get<ai::ToolCallContent>(partial.content[content_index]);
            streaming_call.raw_arguments += tool_call.raw_arguments;
            if (auto emitted = emit(
                    sink,
                    ai::ToolCallDeltaEvent{
                        content_index,
                        tool_call.raw_arguments,
                        partial});
                !emitted) {
                return std::unexpected(emitted.error());
            }
        }
        partial.content[content_index] = tool_call;
        if (auto emitted = emit(
                sink,
                ai::ToolCallEndEvent{content_index, tool_call, partial});
            !emitted) {
            return std::unexpected(emitted.error());
        }
    }

    return emit(
        sink,
        ai::AssistantDoneEvent{final_message.stop_reason, final_message});
}

class ScriptedStreamingFakeClient final : public ai::StreamingChatClient {
public:
    boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& request,
        ai::AssistantEventSink sink) override {
        if (request.model.empty()) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "model is required",
                "scripted fake request model must not be empty"));
        }

        ai::AssistantMessage assistant;
        set_fake_metadata(assistant, request.model);
        assistant.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        if (!request.context.messages.empty()) {
            if (const auto* last_tool = std::get_if<ai::ToolResultMessage>(&request.context.messages.back())) {
                assistant.content.emplace_back(ai::text_content(
                    "fake observed: " + ai::text_from_content(last_tool->content)));
                CCH_TRY_VOID(emit_complete_lifecycle(assistant, sink));
                co_return assistant;
            }
        }

        std::string prompt;
        for (auto it = request.context.messages.rbegin(); it != request.context.messages.rend(); ++it) {
            if (const auto* user = std::get_if<ai::UserMessage>(&*it)) {
                prompt = ai::text_from_content(user->content);
                break;
            }
        }

        if (prompt.rfind("read ", 0) == 0) {
            const auto path = prompt.substr(5);
            auto raw = make_tool_arguments("path", path);
            if (!raw) {
                co_return std::unexpected(raw.error());
            }
            auto args = util::read_json<util::JsonValue>(*raw);
            ai::ToolCallContent call;
            call.id = "fake-read-1";
            call.name = "read";
            call.raw_arguments = *raw;
            if (args) {
                call.arguments = *args;
            }
            assistant.content.emplace_back(ai::TextContent{"reading " + path, std::nullopt});
            assistant.content.emplace_back(std::move(call));
            assistant.stop_reason = ai::AssistantStopReason::ToolUse;
            CCH_TRY_VOID(emit_complete_lifecycle(assistant, sink));
            co_return assistant;
        }
        if (prompt.rfind("bash ", 0) == 0) {
            const auto command = prompt.substr(5);
            auto raw = make_tool_arguments("command", command);
            if (!raw) {
                co_return std::unexpected(raw.error());
            }
            auto args = util::read_json<util::JsonValue>(*raw);
            ai::ToolCallContent call;
            call.id = "fake-bash-1";
            call.name = "bash";
            call.raw_arguments = *raw;
            if (args) {
                call.arguments = *args;
            }
            assistant.content.emplace_back(ai::TextContent{"running bash", std::nullopt});
            assistant.content.emplace_back(std::move(call));
            assistant.stop_reason = ai::AssistantStopReason::ToolUse;
            CCH_TRY_VOID(emit_complete_lifecycle(assistant, sink));
            co_return assistant;
        }

        assistant.content.emplace_back(ai::text_content("fake: " + prompt));
        CCH_TRY_VOID(emit_complete_lifecycle(assistant, sink));
        co_return assistant;
    }
};

} // namespace

std::unique_ptr<ai::StreamingChatClient> make_scripted_fake_chat_client() {
    return std::make_unique<ScriptedStreamingFakeClient>();
}

} // namespace cch::ai::providers
