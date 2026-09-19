#include "AnthropicShared.hpp"

#include "MessageConversion.hpp"
#include "ai/Headers.hpp"
#include "support/Json.hpp"

#include <chrono>
#include <string>
#include <string_view>
#include <utility>

namespace cch::ai::api {
namespace {

using JsonObject = support::JsonValue::object_t;

[[nodiscard]] std::string messages_url(std::string_view base_url) {
    std::string result{base_url};
    while (!result.empty() && result.back() == '/') {
        result.pop_back();
    }
    return result + "/v1/messages";
}

} // namespace

support::Expected<providers::StreamRequest> build_anthropic_stream_request(
        const Model& model, const AiContext& context, const ProviderStreamOptions& options) {
    auto payload = build_adapter_payload(AdapterKind::AnthropicMessages, model, context, options);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    auto body = support::write_json(*payload);
    if (!body) {
        return std::unexpected(body.error());
    }

    providers::StreamRequest request;
    request.url = messages_url(model.base_url);
    request.timeout = std::chrono::milliseconds{options.timeout_ms.value_or(30000)};
    request.stop_token = options.stop_token;
    if (model.headers) {
        for (const auto& [name, value] : *model.headers) {
            if (!header_deleted(options, name)) {
                set_header(request.headers, name, value);
            }
        }
    }
    for (const auto& [name, value] : options.auth.headers) {
        set_header(request.headers, name, value);
    }
    if (options.auth.api_key && !has_header(request.headers, "authorization") &&
            !has_header(request.headers, "x-api-key") && !header_deleted(options, "x-api-key")) {
        set_header(request.headers, "x-api-key", *options.auth.api_key);
    }
    if (!has_header(request.headers, "anthropic-version") && !header_deleted(options, "anthropic-version")) {
        set_header(request.headers, "anthropic-version", "2023-06-01");
    }
    if (!has_header(request.headers, "anthropic-dangerous-direct-browser-access") &&
            !header_deleted(options, "anthropic-dangerous-direct-browser-access")) {
        set_header(request.headers, "anthropic-dangerous-direct-browser-access", "true");
    }
    if (!has_header(request.headers, "content-type") && !header_deleted(options, "content-type")) {
        set_header(request.headers, "Content-Type", "application/json");
    }
    if (!has_header(request.headers, "accept") && !header_deleted(options, "accept")) {
        set_header(request.headers, "Accept", "application/json");
    }
    request.body = std::move(*body);
    return request;
}
} // namespace cch::ai::api
