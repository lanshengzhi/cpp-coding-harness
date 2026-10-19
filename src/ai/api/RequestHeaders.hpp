#pragma once

#include "ai/Headers.hpp"
#include <cch/ai/RequestOptions.hpp>

#include <string_view>

namespace cch::ai::api {

/// True when `name` (any casing) stays deleted in the resolved request
/// headers: an explicit null tombstone suppresses the adapter default.
[[nodiscard]] inline bool has_header_tombstone(const ResolvedHeaders& headers, std::string_view name) {
    const auto found = headers.find(name);
    return found != headers.end() && !found->second;
}

/// Merges the trusted authentication headers with the caller's request
/// header overrides into the resolved request header list
/// (`RequestOptions.hpp` explicit-null deletion contract): a null override
/// stays deleted as a tombstone, `authorization` is always erased because
/// the gateway key reintroduces it or the request fails the no-key check,
/// and surviving callers keep the casing of their own entry.
[[nodiscard]] inline ResolvedHeaders resolve_stream_request_headers(
        const HeaderValues& auth_headers, const RequestHeaders& overrides) {
    ResolvedHeaders headers;
    for (const auto& [name, value] : auth_headers) {
        headers.emplace(name, value);
    }
    merge_headers(headers, overrides);
    erase_header(headers, "authorization");
    return headers;
}

} // namespace cch::ai::api
