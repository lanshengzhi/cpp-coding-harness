#include "FakeChatClient.hpp"

#include "../../../src/util/ExpectedMacros.hpp"
#include "../../../include/cch/ai/ChatClient.hpp"
#include "../../../include/cch/ai/Content.hpp"
#include "util/Json.hpp"

#include <chrono>
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
        if (sink) {
            CCH_TRY_VOID(sink(ai::AssistantStartEvent{assistant}));
        }

        if (!request.context.messages.empty()) {
            if (const auto* last_tool = std::get_if<ai::ToolResultMessage>(&request.context.messages.back())) {
                assistant.content.emplace_back(ai::text_content(
                    "fake observed: " + ai::text_from_content(last_tool->content)));
                co_return co_await emit_text(std::move(assistant), sink);
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
            if (sink) {
                CCH_TRY_VOID(sink(ai::TextDeltaEvent{0, "reading " + path, assistant}));
                CCH_TRY_VOID(sink(ai::AssistantDoneEvent{assistant.stop_reason, assistant}));
            }
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
            if (sink) {
                CCH_TRY_VOID(sink(ai::TextDeltaEvent{0, "running bash", assistant}));
                CCH_TRY_VOID(sink(ai::AssistantDoneEvent{assistant.stop_reason, assistant}));
            }
            co_return assistant;
        }

        assistant.content.emplace_back(ai::text_content("fake: " + prompt));
        co_return co_await emit_text(std::move(assistant), sink);
    }

private:
    boost::asio::awaitable<util::Expected<ai::AssistantMessage>> emit_text(
        ai::AssistantMessage assistant,
        ai::AssistantEventSink& sink) {
        if (sink && !assistant.content.empty()) {
            auto text = ai::text_from_assistant_content(assistant.content);
            CCH_TRY_VOID(sink(ai::TextDeltaEvent{0, text, assistant}));
            CCH_TRY_VOID(sink(ai::AssistantDoneEvent{assistant.stop_reason, assistant}));
        }
        co_return assistant;
    }
};

} // namespace

std::unique_ptr<ai::StreamingChatClient> make_scripted_fake_chat_client() {
    return std::make_unique<ScriptedStreamingFakeClient>();
}

} // namespace cch::ai::providers
