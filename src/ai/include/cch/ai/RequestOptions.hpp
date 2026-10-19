#pragma once

#include <cch/ai/Model.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <stop_token>
#include <string>

namespace cch::ai {

enum class CacheRetention { None, Short, Long };

/// Request headers preserve explicit null deletion through the selected Adapter's
/// case-insensitive merge with protocol defaults and caller overrides.
using RequestEnvironment = std::map<std::string, std::string, std::less<>>;
using RequestHeaders = std::map<std::string, std::optional<std::string>, std::less<>>;

/// Per-turn generation, cancellation and delivery options. Request
/// Authentication is fixed by Models startup configuration (ADR 0072).
struct SimpleStreamOptions {
    std::optional<double> temperature{std::nullopt};
    std::optional<std::uint64_t> max_tokens{std::nullopt};
    std::stop_token stop_token{};
    RequestHeaders headers{};
    RequestEnvironment env{};
    std::optional<ThinkingLevel> reasoning{std::nullopt};
    std::optional<std::string> session_id{std::nullopt};
    std::optional<CacheRetention> cache_retention{std::nullopt};
    std::optional<std::uint64_t> timeout_ms{std::nullopt};
    std::uint32_t max_retries{0};
    std::optional<std::uint64_t> max_retry_delay_ms{std::nullopt};
};

} // namespace cch::ai
