#include "RetryPolicy.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace cch::ai::providers {
namespace {

constexpr std::uint64_t kDefaultMaxRetryDelayMs = 60000;

[[nodiscard]] bool equal_ascii_case_insensitive(std::string_view left, std::string_view right) {
    return std::ranges::equal(left, right, [](char left_character, char right_character) {
        return std::tolower(static_cast<unsigned char>(left_character)) ==
               std::tolower(static_cast<unsigned char>(right_character));
    });
}

[[nodiscard]] std::optional<std::string_view> header(
    const ProviderHeaders& headers,
    std::string_view name) {
    for (const auto& [candidate, value] : headers) {
        if (equal_ascii_case_insensitive(candidate, name)) {
            return value;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::string ascii_lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](char character) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    });
    return value;
}

[[nodiscard]] bool terminal_quota_or_billing(const ProviderFailure& failure) {
    if (failure.terminal_quota_or_billing) {
        return true;
    }
    const auto message = ascii_lower(failure.message);
    constexpr std::string_view kPatterns[]{
        "insufficient quota",
        "quota exceeded",
        "billing",
        "available balance",
        "monthly usage",
        "budget exceeded",
    };
    return std::ranges::any_of(kPatterns, [&message](std::string_view pattern) {
        return message.find(pattern) != std::string::npos;
    });
}

[[nodiscard]] std::optional<double> parse_number(std::string_view text) {
    double value = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<std::int64_t> parse_http_date_ms(std::string_view text) {
    std::tm parsed{};
    std::istringstream input{std::string{text}};
    input.imbue(std::locale::classic());
    input >> std::get_time(&parsed, "%a, %d %b %Y %H:%M:%S GMT");
    if (input.fail()) {
        return std::nullopt;
    }
#if defined(_WIN32)
    const auto seconds = _mkgmtime(&parsed);
#else
    const auto seconds = timegm(&parsed);
#endif
    if (seconds < 0) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(seconds) * 1000;
}

[[nodiscard]] util::Expected<std::uint64_t> validate_delay(
    double delay_ms,
    std::optional<std::uint64_t> requested_max,
    std::string_view message) {
    const auto non_negative = std::max(0.0, delay_ms);
    const auto delay = non_negative >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())
        ? std::numeric_limits<std::uint64_t>::max()
        : static_cast<std::uint64_t>(non_negative);
    const auto maximum = requested_max.value_or(kDefaultMaxRetryDelayMs);
    if (maximum > 0 && delay > maximum) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Stream,
            "Server requested retry delay above configured maximum",
            std::string{message}));
    }
    return delay;
}

} // namespace

bool is_retryable_provider_failure(const ProviderFailure& failure) {
    if (terminal_quota_or_billing(failure)) {
        return false;
    }
    if (const auto should_retry = header(failure.headers, "x-should-retry")) {
        if (*should_retry == "true") {
            return true;
        }
        if (*should_retry == "false") {
            return false;
        }
    }
    if (failure.network_error || !failure.status) {
        return failure.network_error;
    }
    return *failure.status == 408 || *failure.status == 409 ||
           *failure.status == 429 || *failure.status >= 500;
}

util::Expected<std::uint64_t> provider_retry_delay_ms(
    const ProviderFailure& failure,
    std::uint32_t retry_index,
    std::optional<std::uint64_t> max_retry_delay_ms,
    std::int64_t now_epoch_ms) {
    if (const auto retry_after_ms = header(failure.headers, "retry-after-ms")) {
        if (const auto parsed = parse_number(*retry_after_ms)) {
            return validate_delay(*parsed, max_retry_delay_ms, failure.message);
        }
    }
    if (const auto retry_after = header(failure.headers, "retry-after")) {
        if (const auto seconds = parse_number(*retry_after)) {
            return validate_delay(*seconds * 1000, max_retry_delay_ms, failure.message);
        }
        if (const auto date_ms = parse_http_date_ms(*retry_after)) {
            return validate_delay(
                static_cast<double>(*date_ms - now_epoch_ms),
                max_retry_delay_ms,
                failure.message);
        }
    }
    const auto shift = std::min<std::uint32_t>(retry_index, 3);
    return std::uint64_t{1000} << shift;
}

} // namespace cch::ai::providers
