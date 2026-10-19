#include "MagpieCatalog.hpp"

#include "support/Json.hpp"
#include <cch/support/JsonAccess.hpp>
#include <cch/support/JsonValue.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cch::ai {

namespace {

[[nodiscard]] std::optional<ai::ModelThinkingLevel> parse_thinking_level(std::string_view effort) {
    if (effort == "none") return ai::ModelThinkingLevel::Off;
    if (effort == "minimal") return ai::ModelThinkingLevel::Minimal;
    if (effort == "low") return ai::ModelThinkingLevel::Low;
    if (effort == "medium") return ai::ModelThinkingLevel::Medium;
    if (effort == "high") return ai::ModelThinkingLevel::High;
    if (effort == "xhigh") return ai::ModelThinkingLevel::XHigh;
    if (effort == "max" || effort == "ultra") return ai::ModelThinkingLevel::Max;
    return std::nullopt;
}

[[nodiscard]] boost::asio::awaitable<support::Expected<std::string>> fetch_magpie_get(
        support::StreamTransport& transport, // must outlive the returned operation (§7.5)
        std::string url,
        std::string api_key,
        std::chrono::milliseconds timeout,
        std::stop_token stop_token) {
    support::StreamRequest request;
    request.method = "GET";
    request.url = url;
    request.headers["Accept"] = "application/json";
    request.headers["Authorization"] = "Bearer " + api_key;
    request.timeout = timeout;
    request.stop_token = std::move(stop_token);

    auto response = co_await transport.async_stream(request, nullptr);
    if (!response) {
        auto error = std::move(response.error());
        error.message = std::format("Cannot connect to Magpie gateway at {}: {}", url, error.message);
        co_return std::unexpected(std::move(error));
    }
    if (response->head.status_code != 200) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Provider,
                std::format("Magpie gateway returned HTTP {}: {}", response->head.status_code, response->body)));
    }
    co_return std::move(response->body);
}

} // namespace

support::Expected<MagpieHelloInfo> parse_magpie_hello_response(std::string_view body, std::string_view url) {
    auto parsed = support::read_json(body);
    if (!parsed) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                url.empty()
                        ? std::format("Failed to parse Magpie hello JSON: {}", parsed.error().message)
                        : std::format("Failed to parse Magpie hello JSON from {}: {}", url, parsed.error().message)));
    }
    const auto* root = support::json_object(*parsed);
    if (root == nullptr) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                url.empty() ? "Magpie hello payload is not a JSON object"
                            : std::format("Magpie hello payload from {} is not a JSON object", url)));
    }

    MagpieHelloInfo info;
    info.name = std::string(support::json_string_member(*root, "name").value_or("magpie"));
    info.version = std::string(support::json_string_member(*root, "version").value_or("unknown"));
    return info;
}

support::Expected<std::vector<ai::Model>> parse_magpie_models_response(std::string_view body, std::string_view url) {
    auto parsed = support::read_json(body);
    if (!parsed) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                url.empty()
                        ? std::format("Failed to parse Magpie models JSON: {}", parsed.error().message)
                        : std::format("Failed to parse Magpie models JSON from {}: {}", url, parsed.error().message)));
    }
    const auto* root = support::json_object(*parsed);
    if (root == nullptr) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                url.empty() ? "Magpie models payload is not a JSON object"
                            : std::format("Magpie models payload from {} is not a JSON object", url)));
    }
    const auto* data = support::json_array_member(*root, "data");
    if (data == nullptr) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                url.empty() ? "Magpie models payload lacks a 'data' array"
                            : std::format("Magpie models payload from {} lacks a 'data' array", url)));
    }

    std::vector<ai::Model> models;
    models.reserve(data->size());
    for (const auto& item_val : *data) {
        const auto* item = support::json_object(item_val);
        if (item == nullptr) continue;
        auto id_opt = support::json_string_member(*item, "id");
        if (!id_opt || id_opt->empty()) continue;

        ai::Model model;
        model.id = std::string(*id_opt);
        model.name = std::string(support::json_string_member(*item, "display_name").value_or(*id_opt));
        model.provider = "magpie";
        model.api = std::string{kOpenAICompletionsApi};
        model.reasoning = support::json_bool_member(*item, "reasoning").value_or(false);

        const auto context = support::json_integer_member(*item, "context_window")
                                     .or_else([&]() { return support::json_integer_member(*item, "context_length"); })
                                     .value_or(128000);
        model.context_window = static_cast<std::uint64_t>(std::max<std::int64_t>(1, context));

        const auto max_output = support::json_integer_member(*item, "max_output_tokens").value_or(16384);
        model.max_tokens = static_cast<std::uint64_t>(std::max<std::int64_t>(1, max_output));

        std::vector<ai::ModelInput> inputs{ai::ModelInput::Text};
        if (const auto* modalities = support::json_object_member(*item, "modalities")) {
            if (const auto* in_list = support::json_array_member(*modalities, "input")) {
                for (const auto& mod : *in_list) {
                    if (const auto* str = std::get_if<std::string>(&mod.data); str && *str == "image") {
                        inputs.push_back(ai::ModelInput::Image);
                        break;
                    }
                }
            }
        }
        model.input = std::move(inputs);

        if (const auto* levels = support::json_array_member(*item, "supported_reasoning_levels")) {
            ai::ThinkingLevelMap level_map;
            // Absent `none` means this gateway model cannot stop thinking; a
            // `none` record maps the Off level onto the gateway's own wire
            // value so the thinking cycle offers it exactly where the gateway
            // states it.
            level_map[ai::ModelThinkingLevel::Off] = std::nullopt;
            for (const auto& level_val : *levels) {
                if (const auto* level_obj = support::json_object(level_val)) {
                    if (auto effort = support::json_string_member(*level_obj, "effort")) {
                        if (auto parsed_level = parse_thinking_level(*effort)) {
                            level_map[*parsed_level] = std::string(*effort);
                        }
                    }
                }
            }
            if (level_map.size() > 1) {
                model.reasoning = true;
                model.thinking_level_map = std::move(level_map);
            }
        }

        if (auto valid = ai::validate_model(model); !valid) {
            return std::unexpected(valid.error());
        }
        models.push_back(std::move(model));
    }

    return models;
}

boost::asio::awaitable<support::Expected<MagpieHelloInfo>> fetch_magpie_hello(support::StreamTransport& transport,
        std::string base_url,
        std::string api_key,
        std::chrono::milliseconds timeout,
        std::stop_token stop_token) {
    const std::string url = base_url + "/api/hello";
    auto body = co_await fetch_magpie_get(transport, url, std::move(api_key), timeout, std::move(stop_token));
    if (!body) {
        co_return std::unexpected(std::move(body.error()));
    }
    co_return parse_magpie_hello_response(*body, url);
}

boost::asio::awaitable<support::Expected<std::vector<ai::Model>>> fetch_magpie_models(
        support::StreamTransport& transport,
        std::string base_url,
        std::string api_key,
        std::chrono::milliseconds timeout,
        std::stop_token stop_token) {
    const std::string url = base_url + "/models";
    auto body = co_await fetch_magpie_get(transport, url, std::move(api_key), timeout, std::move(stop_token));
    if (!body) {
        co_return std::unexpected(std::move(body.error()));
    }
    co_return parse_magpie_models_response(*body, url);
}

} // namespace cch::ai
