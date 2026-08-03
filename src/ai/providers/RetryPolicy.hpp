#pragma once

#include <cch/ai/Auth.hpp>
#include <cch/util/Error.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace cch::ai::providers {

struct ProviderFailure {
    bool network_error{false};
    std::optional<int> status{std::nullopt};
    ProviderHeaders headers{};
    std::string message{};
    bool terminal_quota_or_billing{false};
};

[[nodiscard]] bool is_retryable_provider_failure(const ProviderFailure& failure);
[[nodiscard]] util::Expected<std::uint64_t> provider_retry_delay_ms(
    const ProviderFailure& failure,
    std::uint32_t retry_index,
    std::optional<std::uint64_t> max_retry_delay_ms,
    std::int64_t now_epoch_ms);

} // namespace cch::ai::providers
