#pragma once

#include "ai/Timestamps.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/BoundedText.hpp"

#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <chrono>

#include <boost/asio/awaitable.hpp>

#include <expected>
#include <string>
#include <type_traits>
#include <utility>

#include <cstdint>

namespace cch::ai {

/// Shared small vocabulary for the Models implementation files: credential
/// expiry, error categorization/scrubbing, and the AsyncResult await bridge.

inline constexpr std::chrono::milliseconds kOAuthMinimumValidity{std::chrono::minutes{5}};
inline constexpr std::size_t kMaxPublicErrorBytes = 1024;

[[nodiscard]] inline bool expires_soon(const OAuthCredential& credential) {
    return credential.expires <= current_timestamp_ms() + kOAuthMinimumValidity.count();
}

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

[[nodiscard]] inline support::Error safe_error(support::Error error) {
    error.message = support::bounded_redacted_text(std::move(error.message), kMaxPublicErrorBytes, "...");
    error.detail = support::bounded_redacted_text(std::move(error.detail), kMaxPublicErrorBytes, "...");
    if (error.context) {
        error.context = support::bounded_redacted_text(std::move(*error.context), kMaxPublicErrorBytes, "...");
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
