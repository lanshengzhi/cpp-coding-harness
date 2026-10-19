#pragma once

#include <cch/ai/Timestamps.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/BoundedText.hpp>
#include <cch/support/Error.hpp>
#include "support/AsyncResultBridge.hpp"

#include <boost/algorithm/string/replace.hpp>
#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace cch::ai {

/// Shared error categorization/scrubbing for model-stream delivery.

inline constexpr std::size_t kMaxPublicErrorBytes = 1024;

[[nodiscard]] inline support::Error categorized_error(
        support::ErrorCode code, std::string message, const support::Error& cause) {
    std::string detail = cause.message;
    if (!cause.detail.empty()) {
        detail += ": ";
        detail += cause.detail;
    }
    return support::make_error(code,
            support::bounded_redacted_text(std::move(message), kMaxPublicErrorBytes, "..."),
            support::bounded_redacted_text(std::move(detail), kMaxPublicErrorBytes, "..."));
}

[[nodiscard]] inline std::string safe_diagnostic_text(std::string text, std::string_view request_key = {}) {
    if (!request_key.empty() && request_key != "magpie") {
        boost::algorithm::replace_all(text, request_key, support::kRedactionMarker);
    }
    return support::bounded_redacted_text(std::move(text), kMaxPublicErrorBytes, "...");
}

[[nodiscard]] inline support::Error safe_error(support::Error error, std::string_view request_key = {}) {
    // ADR 0073: a gateway may echo its invocation key without a recognizable
    // token prefix. Scrub that exact request secret before bounding diagnostics.
    // The default "magpie" placeholder is not a secret and must not be redacted.
    error.message = safe_diagnostic_text(std::move(error.message), request_key);
    error.detail = safe_diagnostic_text(std::move(error.detail), request_key);
    if (error.context) {
        error.context = safe_diagnostic_text(std::move(*error.context), request_key);
    }
    return error;
}

template <typename T> struct AsyncResultValue;

template <typename T, typename E> struct AsyncResultValue<cch::support::AsyncResult<T, E>> {
    using type = std::expected<T, E>;
};

/// Await one `AsyncResult`-returning operation from within an asio coroutine.
/// Callback and producer failures already use the operation's explicit
/// `Expected` terminal value; this helper only carries that value across the
/// private Asio bridge.
template <typename Operation>
[[nodiscard]] boost::asio::awaitable<typename AsyncResultValue<std::invoke_result_t<Operation&>>::type>
invoke_async_operation(Operation operation) {
    auto result = operation();
    co_return co_await support::detail::await_async_result(std::move(result));
}

} // namespace cch::ai
