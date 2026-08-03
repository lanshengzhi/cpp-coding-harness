#pragma once

#include "util/ExpectedMacros.hpp"

#include <cch/util/Error.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>

namespace cch::ai::auth {

/// Frozen device-flow messages from pi baseline 83114817
/// (`packages/ai/src/auth/oauth/device-code.ts`).
inline constexpr std::string_view kDeviceCancelMessage = "Login cancelled";
inline constexpr std::string_view kDeviceTimeoutMessage = "Device flow timed out";
inline constexpr std::string_view kDeviceSlowDownTimeoutMessage =
    "Device flow timed out after one or more slow_down responses. This is "
    "often caused by clock drift in WSL or VM environments. Please sync or "
    "restart the VM clock and try again.";

/// One poll outcome, mirroring pi's `OAuthDeviceCodePollResult`.
template <typename T>
struct DevicePollResult {
    struct Pending {};
    struct SlowDown {
        /// Server-provided new minimum interval; when absent RFC 8628 §3.5
        /// applies a +5s increment.
        std::optional<int> interval_seconds{std::nullopt};
    };
    struct Failed {
        std::string message{};
    };
    struct Complete {
        T value{};
    };

    std::variant<Pending, SlowDown, Failed, Complete> kind{Pending{}};
};

template <typename T>
using DevicePollFn = std::move_only_function<
    boost::asio::awaitable<util::Expected<DevicePollResult<T>>>()>;

template <typename T>
struct DevicePollOptions {
    std::optional<int> interval_seconds{std::nullopt};
    std::optional<int> expires_in_seconds{std::nullopt};
    bool wait_before_first_poll{false};
    DevicePollFn<T> poll{};
    std::stop_token stop_token{};
};

namespace detail {

[[nodiscard]] inline boost::asio::awaitable<util::ExpectedVoid> abortable_sleep(
    std::chrono::milliseconds duration,
    std::stop_token stop_token) {
    if (stop_token.stop_requested()) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Cancelled,
            std::string{kDeviceCancelMessage}));
    }
    auto executor = co_await boost::asio::this_coro::executor;
    // The stop callback captures only shared state and posts to the executor
    // (never a coroutine-local timer), so a cross-thread request_stop cannot
    // race this frame's destruction (same pattern as BoostBeast transports).
    auto cancellation_signal =
        std::make_shared<boost::asio::cancellation_signal>();
    std::stop_callback cancellation{stop_token, [executor, cancellation_signal] {
        boost::asio::post(executor, [cancellation_signal] {
            cancellation_signal->emit(boost::asio::cancellation_type::all);
        });
    }};
    boost::asio::steady_timer timer(executor, duration);
    boost::system::error_code error;
    co_await timer.async_wait(boost::asio::bind_cancellation_slot(
        cancellation_signal->slot(),
        boost::asio::redirect_error(boost::asio::use_awaitable, error)));
    if (stop_token.stop_requested()) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Cancelled,
            std::string{kDeviceCancelMessage}));
    }
    co_return util::ExpectedVoid{};
}

} // namespace detail

/// Shared RFC 8628 device-flow poll loop matching pi's
/// `pollOAuthDeviceCodeFlow` exactly: pending/slow_down/failed/complete
/// outcomes, a wall deadline, abortable sleeps, and the frozen timeout
/// messages. `slow_down` honors the server interval when supplied, otherwise
/// adds 5 seconds.
template <typename T>
[[nodiscard]] inline boost::asio::awaitable<util::Expected<T>> poll_device_flow(
    DevicePollOptions<T> options) {
    constexpr auto kMinimumInterval = std::chrono::milliseconds{1000};
    constexpr auto kDefaultInterval = std::chrono::seconds{5};
    constexpr auto kSlowDownIncrement = std::chrono::milliseconds{5000};

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = options.expires_in_seconds
        ? start + std::chrono::seconds{*options.expires_in_seconds}
        : std::chrono::steady_clock::time_point::max();
    auto interval = std::max(
        kMinimumInterval,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::seconds{options.interval_seconds.value_or(5)}));
    int slow_down_count = 0;

    if (options.wait_before_first_poll) {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining > std::chrono::steady_clock::duration::zero()) {
            CCH_TRY_VOID(co_await detail::abortable_sleep(
                std::min(interval, std::chrono::duration_cast<std::chrono::milliseconds>(remaining)),
                options.stop_token));
        }
    }

    while (std::chrono::steady_clock::now() < deadline) {
        if (options.stop_token.stop_requested()) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Cancelled,
                std::string{kDeviceCancelMessage}));
        }
        CCH_TRY(result, co_await options.poll());
        if (auto* complete = std::get_if<typename DevicePollResult<T>::Complete>(
                &result.kind)) {
            co_return std::move(complete->value);
        }
        if (auto* failed = std::get_if<typename DevicePollResult<T>::Failed>(
                &result.kind)) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::OAuth,
                failed->message));
        }
        if (auto* slow = std::get_if<typename DevicePollResult<T>::SlowDown>(
                &result.kind)) {
            ++slow_down_count;
            interval = slow->interval_seconds && *slow->interval_seconds > 0
                ? std::max(
                      kMinimumInterval,
                      std::chrono::milliseconds{*slow->interval_seconds * 1000})
                : std::max(kMinimumInterval, interval + kSlowDownIncrement);
        }
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) {
            break;
        }
        CCH_TRY_VOID(co_await detail::abortable_sleep(
            std::min(interval, std::chrono::duration_cast<std::chrono::milliseconds>(remaining)),
            options.stop_token));
    }

    co_return std::unexpected(util::make_error(
        util::ErrorCode::Timeout,
        std::string{slow_down_count > 0
            ? kDeviceSlowDownTimeoutMessage
            : kDeviceTimeoutMessage}));
}

} // namespace cch::ai::auth
