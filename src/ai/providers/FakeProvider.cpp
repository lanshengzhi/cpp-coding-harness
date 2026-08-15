#include "FakeProvider.hpp"

#include <cch/ai/Content.hpp>
#include <cch/ai/Models.hpp>
#include <cch/ai/Provider.hpp>
#include "ai/ModelStreamBridge.hpp"
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

namespace cch::ai::providers {
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
    const ai::AssistantMessage& final_message,
    ai::AssistantEventSink& sink) {
    auto partial = final_message;
    partial.content.clear();

    if (auto emitted = emit(
            sink,
            ai::AssistantStartEvent{.partial = partial});
        !emitted) {
        return std::unexpected(emitted.error());
    }

    for (std::size_t content_index = 0;
         content_index < final_message.content.size();
         ++content_index) {
        const auto& completed = final_message.content[content_index];
        if (const auto* text = std::get_if<ai::TextContent>(&completed)) {
            partial.content.emplace_back(ai::TextContent{
                .text = "",
                .text_signature = std::nullopt,
            });
            if (auto emitted = emit(
                    sink,
                    ai::TextStartEvent{
                        .content_index = content_index,
                        .partial = partial,
                    });
                !emitted) {
                return std::unexpected(emitted.error());
            }
            if (!text->text.empty()) {
                std::get<ai::TextContent>(partial.content[content_index]).text = text->text;
                if (auto emitted = emit(
                        sink,
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
            if (auto emitted = emit(
                    sink,
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
        if (auto emitted = emit(
                sink,
                ai::ToolCallStartEvent{
                    .content_index = content_index,
                    .partial = partial,
                });
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
                        .content_index = content_index,
                        .delta = tool_call.raw_arguments,
                        .partial = partial,
                    });
                !emitted) {
                return std::unexpected(emitted.error());
            }
        }
        partial.content[content_index] = tool_call;
        if (auto emitted = emit(
                sink,
                ai::ToolCallEndEvent{
                    .content_index = content_index,
                    .tool_call = tool_call,
                    .partial = partial,
                });
            !emitted) {
            return std::unexpected(emitted.error());
        }
    }

    return emit(
        sink,
        ai::AssistantDoneEvent{
            .reason = final_message.stop_reason,
            .message = final_message,
        });
}

[[nodiscard]] support::ExpectedVoid respond_with_tool_call(
    ai::AssistantMessage& assistant,
    ai::AssistantEventSink& sink,
    FakeToolCallSpec spec) {
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

class EmptyCredentialStore final : public ai::CredentialStore {
public:
    [[nodiscard]] cch::support::AsyncResult<std::optional<ai::Credential>> read(
        std::string) override {
        return cch::support::AsyncResult<std::optional<ai::Credential>>(
            std::expected<std::optional<ai::Credential>, cch::support::Error>{
                std::optional<ai::Credential>{}});
    }

    [[nodiscard]] cch::support::AsyncResult<std::vector<ai::CredentialInfo>> list() override {
        return cch::support::AsyncResult<std::vector<ai::CredentialInfo>>(
            std::expected<std::vector<ai::CredentialInfo>, cch::support::Error>{
                std::vector<ai::CredentialInfo>{}});
    }

    [[nodiscard]] cch::support::AsyncResult<std::optional<ai::Credential>> modify(
        std::string,
        ai::CredentialModifyHook) override {
        return cch::support::AsyncResult<std::optional<ai::Credential>>(
            std::expected<std::optional<ai::Credential>, cch::support::Error>{
                std::optional<ai::Credential>{}});
    }

    [[nodiscard]] cch::support::AsyncResult<void> remove(std::string) override {
        return cch::support::AsyncResult<void>(
            std::expected<void, cch::support::Error>{});
    }
};

class EmptyAuthContext final : public ai::AuthContext {
public:
    [[nodiscard]] cch::support::AsyncResult<std::optional<std::string>> environment(
        std::string) const override {
        return cch::support::AsyncResult<std::optional<std::string>>(
            std::expected<std::optional<std::string>, cch::support::Error>{
                std::optional<std::string>{}});
    }

    [[nodiscard]] cch::support::AsyncResult<bool> file_exists(
        std::string) const override {
        return cch::support::AsyncResult<bool>(
            std::expected<bool, cch::support::Error>{false});
    }
};

ai::ProviderAuth fake_auth() {
    ai::ApiKeyAuth api_key;
    api_key.name = "scripted fake";
    api_key.check = [](const ai::AuthContext&, std::optional<ai::ApiKeyCredential>)
        -> cch::support::AsyncResult<std::optional<ai::AuthCheck>> {
        return cch::support::AsyncResult<std::optional<ai::AuthCheck>>(
            std::expected<std::optional<ai::AuthCheck>, cch::support::Error>{
                ai::AuthCheck{.source = "scripted fake", .type = ai::AuthType::ApiKey}});
    };
    api_key.resolve = [](const ai::AuthContext&, std::optional<ai::ApiKeyCredential>)
        -> cch::support::AsyncResult<std::optional<ai::AuthResult>> {
        return cch::support::AsyncResult<std::optional<ai::AuthResult>>(
            std::expected<std::optional<ai::AuthResult>, cch::support::Error>{
                ai::AuthResult{.source = "scripted fake"}});
    };
    return ai::ProviderAuth{.api_key = std::move(api_key)};
}

[[nodiscard]] std::optional<support::ErrorCode> scripted_failure_code(
    std::string_view prompt) {
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

class ScriptedFakeProvider final : public ai::Provider {
public:
    explicit ScriptedFakeProvider(std::string provider_id)
        : provider_id_(std::move(provider_id)), auth_(fake_auth()) {}

    [[nodiscard]] std::string_view id() const noexcept override { return provider_id_; }
    [[nodiscard]] std::string_view name() const noexcept override { return "Fake"; }
    [[nodiscard]] ai::ProviderAuth& auth() noexcept override { return auth_; }
    [[nodiscard]] std::vector<ai::Model> models() const override { return {}; }

    [[nodiscard]] ai::ModelStream stream(
        ai::Model model,
        ai::AiContext context,
        ai::ProviderStreamOptions options) override {
        return detail::make_model_stream(
            [model = std::move(model),
             context = std::move(context),
             options = std::move(options)](
                ai::AssistantEventSink sink)
                -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
        ai::AssistantMessage assistant;
        set_fake_metadata(assistant, model);
        assistant.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        if (options.stop_token.stop_requested()) {
            assistant.stop_reason = ai::AssistantStopReason::Aborted;
            assistant.error_message = "Request was aborted";
            auto failure = support::make_error(
                support::ErrorCode::Cancelled,
                *assistant.error_message);
            if (auto emitted = emit(
                    sink,
                    ai::AssistantErrorEvent{
                        .reason = assistant.stop_reason,
                        .error = assistant,
                        .failure = std::move(failure),
                    });
                !emitted) {
                co_return std::unexpected(emitted.error());
            }
            co_return assistant;
        }

        if (!context.messages.empty()) {
            if (const auto* last_tool = std::get_if<ai::ToolResultMessage>(&context.messages.back())) {
                assistant.content.emplace_back(ai::text_content(
                    "fake observed: " + ai::text_from_content(last_tool->content)));
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
            if (auto emitted = emit(
                    sink,
                    ai::AssistantErrorEvent{
                        .reason = assistant.stop_reason,
                        .error = assistant,
                        .failure = std::move(failure),
                    });
                !emitted) {
                co_return std::unexpected(emitted.error());
            }
            co_return assistant;
        }

        if (prompt.starts_with("read ")) {
            const auto path = prompt.substr(5);
            CCH_TRY_VOID(respond_with_tool_call(
                assistant,
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
            CCH_TRY_VOID(respond_with_tool_call(
                assistant,
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
    }

private:
    std::string provider_id_;
    ai::ProviderAuth auth_;
};

} // namespace

std::shared_ptr<ai::Provider> make_scripted_fake_provider(
    std::string provider_id) {
    return std::make_shared<ScriptedFakeProvider>(std::move(provider_id));
}

std::shared_ptr<ai::Models> make_scripted_fake_models() {
    auto models = std::make_shared<ai::Models>(
        std::make_shared<EmptyCredentialStore>(),
        std::make_shared<EmptyAuthContext>());
    if (auto added = models->set_provider(make_scripted_fake_provider()); !added) {
        return nullptr;
    }
    if (auto added = models->set_provider(make_scripted_fake_provider("sdk-host")); !added) {
        return nullptr;
    }
    return models;
}

} // namespace cch::ai::providers
