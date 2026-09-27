#pragma once

#include "ai/Headers.hpp"
#include "ai/providers/Provider.hpp"

#include <utility>

namespace cch::ai::api {

[[nodiscard]] inline support::ExpectedVoid prepare_stream_request_headers(
        const Model& model, ProviderStreamOptions& options, RequestHeaders adapter_defaults = {}) {
    RequestHeaders headers;
    for (auto& [name, value] : options.auth.headers) {
        headers.emplace(name, std::move(value));
    }
    options.auth.headers.clear();

    if (model.provider == "opencode-go" && options.session_id && !options.session_id->empty() &&
            !find_header(headers, "x-opencode-session")) {
        set_header(headers, "x-opencode-session", *options.session_id);
    }
    for (auto& [name, value] : adapter_defaults) {
        if (!find_header(headers, name)) {
            headers.emplace(name, std::move(value));
        }
    }
    merge_headers(headers, options.header_overrides);
    if (options.transform_headers) {
        auto transformed = std::move(options.transform_headers)(std::move(headers));
        if (!transformed) {
            return std::unexpected(transformed.error());
        }
        headers = std::move(*transformed);
    }

    options.deleted_headers.clear();
    for (auto& [name, value] : headers) {
        if (value) {
            options.auth.headers.emplace(name, std::move(*value));
        } else {
            options.deleted_headers.push_back(name);
        }
    }
    return {};
}

} // namespace cch::ai::api
