#include "RetryPolicy.hpp"

#include "ai/Headers.hpp"
#include "ai/JsonAccess.hpp"
#include "support/Json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <chrono>
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

[[nodiscard]] InferenceFailureKind effective_failure_kind(const ProviderFailure& failure) noexcept {
    if (failure.inference_failure) {
        return failure.inference_failure->kind;
    }
    if (failure.network_error) {
        return InferenceFailureKind::TransientTransportFailure;
    }
    if (failure.status) {
        return inference_failure_kind_from_http_status(*failure.status);
    }
    return InferenceFailureKind::InvalidRequest;
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
    const auto seconds = timegm(&parsed);
    if (seconds < 0) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(seconds) * 1000;
}

[[nodiscard]] support::Expected<std::uint64_t> validate_delay(
    double delay_ms,
    std::optional<std::uint64_t> requested_max,
    std::string_view message) {
    const auto non_negative = std::max(0.0, delay_ms);
    const auto delay = non_negative >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())
        ? std::numeric_limits<std::uint64_t>::max()
        : static_cast<std::uint64_t>(non_negative);
    const auto maximum = requested_max.value_or(kDefaultMaxRetryDelayMs);
    if (maximum > 0 && delay > maximum) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Stream,
            "Server requested retry delay above configured maximum",
            std::string{message}));
    }
    return delay;
}

[[nodiscard]] std::optional<std::uint64_t> non_negative_delay_ms(double value, double multiplier) {
    if (!std::isfinite(value) || value < 0) {
        return std::nullopt;
    }
    const auto scaled = value * multiplier;
    if (scaled >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(scaled);
}

[[nodiscard]] std::optional<double> numeric_value(const support::JsonValue& value) {
    if (const auto* number = value.get_if<double>()) {
        return *number;
    }
    if (const auto* text = value.get_if<std::string>()) {
        return parse_number(*text);
    }
    return std::nullopt;
}

} // namespace

std::optional<std::uint64_t> provider_backoff_hint_ms(const ProviderFailure& failure, std::int64_t now_epoch_ms) {
    if (const auto retry_after_ms = find_header(failure.headers, "retry-after-ms")) {
        if (const auto parsed = parse_number(*retry_after_ms)) {
            if (const auto delay = non_negative_delay_ms(*parsed, 1.0)) {
                return delay;
            }
        }
    }
    if (const auto retry_after = find_header(failure.headers, "retry-after")) {
        if (const auto seconds = parse_number(*retry_after)) {
            if (const auto delay = non_negative_delay_ms(*seconds, 1000.0)) {
                return delay;
            }
        }
        if (const auto date_ms = parse_http_date_ms(*retry_after)) {
            return non_negative_delay_ms(static_cast<double>(*date_ms - now_epoch_ms), 1.0);
        }
    }
    return std::nullopt;
}

std::optional<std::uint64_t> provider_backoff_hint_ms(
        const support::JsonValue::object_t& payload, std::int64_t now_epoch_ms) {
    const auto find_hint = [](const support::JsonValue::object_t& object,
                                   std::string_view key,
                                   double multiplier) -> std::optional<std::uint64_t> {
        const auto found = object.find(std::string{key});
        if (found == object.end()) {
            return std::nullopt;
        }
        const auto number = numeric_value(found->second);
        return number ? non_negative_delay_ms(*number, multiplier) : std::nullopt;
    };
    const auto find_in = [&find_hint](const support::JsonValue::object_t& object) -> std::optional<std::uint64_t> {
        for (const auto key : {"retry_after_ms", "retry-after-ms", "retryAfterMs"}) {
            if (const auto hint = find_hint(object, key, 1.0)) {
                return hint;
            }
        }
        for (const auto key : {"retry_after", "retry-after", "retryAfter"}) {
            if (const auto hint = find_hint(object, key, 1000.0)) {
                return hint;
            }
        }
        return std::nullopt;
    };
    if (const auto hint = find_in(payload)) {
        return hint;
    }
    if (const auto error = payload.find("error"); error != payload.end()) {
        if (const auto* object = error->second.get_if<support::JsonValue::object_t>()) {
            if (const auto hint = find_in(*object)) {
                return hint;
            }
        }
    }
    (void)now_epoch_ms;
    return std::nullopt;
}

std::optional<std::uint64_t> provider_backoff_hint_ms(std::string_view payload, std::int64_t now_epoch_ms) {
    const auto parsed = support::read_json(payload);
    if (!parsed) {
        return std::nullopt;
    }
    const auto* object = parsed->get_if<support::JsonValue::object_t>();
    return object ? provider_backoff_hint_ms(*object, now_epoch_ms) : std::nullopt;
}

InferenceFailureKind inference_failure_kind_from_provider_code(
    std::string_view provider_code) noexcept {
    struct CodeGroup {
        InferenceFailureKind kind;
        std::initializer_list<std::string_view> codes;
    };
    static constexpr CodeGroup kGroups[] = {
            {InferenceFailureKind::Unauthorized,
                    {"invalid_api_key",
                            "authentication_error",
                            "permission_error",
                            "unauthorized",
                            "unauthorized_error",
                            "invalid_token"}},
            {InferenceFailureKind::RateLimited,
                    {"rate_limit_exceeded", "rate_limited", "rate_limit_error", "too_many_requests"}},
            {InferenceFailureKind::ContextOverflow,
                    {"context_length_exceeded", "request_too_large", "prompt_too_long", "context_window_exceeded"}},
            {InferenceFailureKind::InvalidRequest,
                    {"insufficient_quota",
                            "quota_exceeded",
                            "billing_error",
                            "budget_exceeded",
                            "out_of_budget",
                            "usage_limit_reached",
                            "free_usage_limit_error",
                            "go_usage_limit_error"}},
            {InferenceFailureKind::TransientTransportFailure,
                    {"overloaded",
                            "overloaded_error",
                            "server_error",
                            "internal_server_error",
                            "service_unavailable",
                            "resource_exhausted",
                            "timeout",
                            "temporarily_unavailable"}},
            {InferenceFailureKind::Cancelled, {"cancelled", "canceled"}},
    };
    for (const auto& group : kGroups) {
        if (std::ranges::any_of(group.codes,
                    [provider_code](std::string_view code) { return header_name_equal(provider_code, code); })) {
            return group.kind;
        }
    }
    return InferenceFailureKind::InvalidRequest;
}

InferenceFailureKind inference_failure_kind_from_http_status(int status) noexcept {
    if (status == 401 || status == 403) {
        return InferenceFailureKind::Unauthorized;
    }
    if (status == 429) {
        return InferenceFailureKind::RateLimited;
    }
    if (status == 413) {
        return InferenceFailureKind::ContextOverflow;
    }
    if (status == 408 || status == 409 || status >= 500) {
        return InferenceFailureKind::TransientTransportFailure;
    }
    return InferenceFailureKind::InvalidRequest;
}

InferenceFailureKind inference_failure_kind_from_transport(
    support::ErrorCode code) noexcept {
    if (code == support::ErrorCode::Cancelled) {
        return InferenceFailureKind::Cancelled;
    }
    if (code == support::ErrorCode::Network || code == support::ErrorCode::Timeout) {
        return InferenceFailureKind::TransientTransportFailure;
    }
    if (code == support::ErrorCode::Auth || code == support::ErrorCode::OAuth) {
        return InferenceFailureKind::Unauthorized;
    }
    return InferenceFailureKind::InvalidRequest;
}

std::optional<std::string> provider_error_code_from_payload(
    std::string_view payload) {
    auto parsed = support::read_json(payload);
    if (!parsed) {
        return std::nullopt;
    }
    const auto* object = parsed->get_if<support::JsonValue::object_t>();
    if (!object) {
        return std::nullopt;
    }
    if (auto code = json_string_member(*object, "code")) {
        return std::string{*code};
    }
    const auto error = object->find("error");
    if (error == object->end()) {
        return std::nullopt;
    }
    const auto* error_object = error->second.get_if<support::JsonValue::object_t>();
    if (!error_object) {
        return std::nullopt;
    }
    if (auto code = json_string_member(*error_object, "code")) {
        return std::string{*code};
    }
    if (auto type = json_string_member(*error_object, "type")) {
        return std::string{*type};
    }
    return std::nullopt;
}

bool is_retryable_provider_failure(const ProviderFailure& failure) {
    if (failure.inference_failure) {
        const auto kind = failure.inference_failure->kind;
        if (kind != InferenceFailureKind::RateLimited &&
            kind != InferenceFailureKind::TransientTransportFailure) {
            return false;
        }
    }
    if (const auto should_retry = find_header(failure.headers, "x-should-retry")) {
        if (*should_retry == "true") {
            return true;
        }
        if (*should_retry == "false") {
            return false;
        }
    }
    const auto kind = effective_failure_kind(failure);
    return kind == InferenceFailureKind::RateLimited ||
           kind == InferenceFailureKind::TransientTransportFailure;
}

support::Expected<std::uint64_t> provider_retry_delay_ms(
    const ProviderFailure& failure,
    std::uint32_t retry_index,
    std::optional<std::uint64_t> max_retry_delay_ms,
    std::int64_t now_epoch_ms) {
    if (const auto hint = provider_backoff_hint_ms(failure, now_epoch_ms)) {
        return validate_delay(static_cast<double>(*hint), max_retry_delay_ms, failure.message);
    }
    const auto shift = std::min<std::uint32_t>(retry_index, 3);
    return std::uint64_t{1000} << shift;
}

} // namespace cch::ai::providers
