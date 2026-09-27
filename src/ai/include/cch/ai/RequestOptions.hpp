#pragma once

#include <cch/ai/Auth.hpp>
#include <cch/ai/Model.hpp>
#include <cch/support/Error.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stop_token>
#include <string>

namespace cch::ai {

enum class CacheRetention { None, Short, Long };

/// Request headers preserve explicit null deletion through the selected Adapter's
/// case-insensitive merge with protocol defaults and caller overrides.
using RequestHeaders = std::map<std::string, std::optional<std::string>, std::less<>>;
using TransformHeadersHook = std::move_only_function<cch::support::Expected<RequestHeaders>(RequestHeaders)>;

/// Exact caller-facing option set accepted by Models::stream at the
/// frozen pi baseline. Authentication is resolved by Models; the selected
/// Adapter applies the transform hook after preparing protocol defaults.
struct SimpleStreamOptions {
    std::optional<double> temperature{std::nullopt};
    std::optional<std::uint64_t> max_tokens{std::nullopt};
    std::stop_token stop_token{};
    std::optional<std::string> api_key{std::nullopt};
    RequestHeaders headers{};
    ProviderEnv env{};
    TransformHeadersHook transform_headers{};
    std::optional<ThinkingLevel> reasoning{std::nullopt};
    std::optional<std::string> session_id{std::nullopt};
    std::optional<CacheRetention> cache_retention{std::nullopt};
    std::optional<std::uint64_t> timeout_ms{std::nullopt};
    std::uint32_t max_retries{0};
    std::optional<std::uint64_t> max_retry_delay_ms{std::nullopt};
};

} // namespace cch::ai
