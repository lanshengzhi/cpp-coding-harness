#pragma once

#include "ai/Headers.hpp"
#include <cch/ai/Model.hpp>
#include <cch/ai/RequestOptions.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <algorithm>

namespace cch::ai::api {

/// Case-insensitive ordering for resolved request header names. It preserves
/// the spelling of the selected map key while making tombstone lookup direct.
struct ResolvedHeaderNameLess final {
    using is_transparent = void;

    [[nodiscard]] bool operator()(std::string_view left, std::string_view right) const noexcept {
        return std::lexicographical_compare(
                left.begin(), left.end(), right.begin(), right.end(), [](char left_character, char right_character) {
                    const auto lower = [](char character) {
                        return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                                                    : character;
                    };
                    return lower(left_character) < lower(right_character);
                });
    }
};

/// Resolved request headers preserve explicit-null tombstones and key spelling
/// while comparing names case-insensitively inside the adapter.
using ResolvedHeaders = std::map<std::string, std::optional<std::string>, ResolvedHeaderNameLess>;

/// Trusted request-local gateway authentication, private to cch_ai.
struct RequestAuth {
    std::optional<std::string> api_key{std::nullopt};
    HeaderValues headers{};
};

/// The one Chat Completions request state the adapter executes: the
/// Models-level preparation of a caller's `SimpleStreamOptions` against one
/// Model and AiContext — clamped generation fields, resolved cache retention
/// and session identity, and assembled authentication.
///
/// `headers` always stays in the adapter: it is the resolved request header
/// list, not the caller's `SimpleStreamOptions::headers` (explicit null
/// values are tombstones that suppress adapter defaults,
/// `RequestOptions.hpp`).
struct PreparedStreamOptions {
    RequestAuth auth{};
    /// Resolved request headers: non-null values ride the request and null
    /// values stay deleted through the case-insensitive merge.
    ResolvedHeaders headers{};
    std::optional<double> temperature{std::nullopt};
    std::uint64_t max_tokens{1};
    std::optional<ModelThinkingLevel> reasoning{std::nullopt};
    std::optional<std::string> session_id{std::nullopt};
    CacheRetention cache_retention{CacheRetention::Short};
    std::optional<std::uint64_t> timeout_ms{std::nullopt};
    std::uint32_t max_retries{0};
    std::uint64_t max_retry_delay_ms{60000};
    std::stop_token stop_token{};
};

} // namespace cch::ai::api
