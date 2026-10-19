#pragma once

#include <cch/ai/Message.hpp>
#include <cch/ai/Model.hpp>
#include <cch/ai/StreamEvent.hpp>
#include "support/StreamTransport.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/ReleaseGate.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_future.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace cch::tests {

/// Give a scripted assistant response the identity and real epoch timestamp
/// every real adapter supplies. Only what the caller left unset is filled
/// (`api`/`provider`/`model`, and the `assistant_text_message` timestamp
/// default of 0), so a test's own values survive while its terminal stays a
/// record the session writer can persist and read back (#665).
[[nodiscard]] inline ai::AssistantMessage stamped_response(ai::AssistantMessage message, const ai::Model& model) {
    constexpr std::int64_t kResponseTimestampMs = 1'718'000'000'123;
    if (message.api.empty()) {
        message.api = model.api;
    }
    if (message.provider.empty()) {
        message.provider = model.provider;
    }
    if (message.model.empty()) {
        message.model = model.id;
    }
    if (message.timestamp == 0) {
        message.timestamp = kResponseTimestampMs;
    }
    return message;
}

/// Stop reasons of every non-terminal (partial) assistant event, in emission
/// order. Terminal done/error events are excluded.
[[nodiscard]] inline std::vector<ai::AssistantStopReason> partial_stop_reasons(
    const std::vector<ai::AssistantStreamEvent>& events) {
    std::vector<ai::AssistantStopReason> reasons;
    for (const auto& event : events) {
        std::visit(
            [&reasons](const auto& concrete) {
                using Event = std::decay_t<decltype(concrete)>;
                if constexpr (!std::is_same_v<Event, ai::AssistantDoneEvent> &&
                              !std::is_same_v<Event, ai::AssistantErrorEvent>) {
                    reasons.push_back(concrete.partial.stop_reason);
                }
            },
            event);
    }
    return reasons;
}

template <typename T> [[nodiscard]] T run_awaitable(boost::asio::awaitable<T> operation) {
    boost::asio::io_context io;
    auto result = boost::asio::co_spawn(io, std::move(operation), boost::asio::use_future);
    io.run();
    return result.get();
}

template <typename T, typename E>
[[nodiscard]] std::expected<T, E> run_async_result(cch::support::AsyncResult<T, E> result) {
    boost::asio::io_context io;
    auto future = boost::asio::co_spawn(
            io,
            [](cch::support::AsyncResult<T, E> op) -> boost::asio::awaitable<std::expected<T, E>> {
                co_return co_await cch::support::detail::await_async_result(std::move(op));
            }(std::move(result)),
            boost::asio::use_future);
    io.run();
    return future.get();
}

struct TransportAttempt {
    support::StreamResponseHead head{
            .status_code = 200,
            .headers = {},
    };
    std::vector<std::string> chunks{};
    std::optional<support::Error> failure{std::nullopt};
};

class StopAwareBlockingTransport final : public support::StreamTransport {
public:
    [[nodiscard]] boost::asio::awaitable<support::Expected<support::StreamResponse>> async_stream(
            const support::StreamRequest& request, support::BodyChunkHandler) override {
        const auto request_number = request_count.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (request_number == 1) {
            co_return support::StreamResponse{
                    .head = {.status_code = 200, .headers = {}},
                    .body = R"({"data":[]})",
            };
        }

        observed_stop_token.store(request.stop_token.stop_possible(), std::memory_order_release);
        const auto executor = co_await boost::asio::this_coro::executor;
        const auto gate = std::make_shared<ReleaseGate>();
        std::stop_callback on_stop(request.stop_token,
                [gate, executor]() noexcept { boost::asio::post(executor, [gate] { gate->interrupt(); }); });
        if (!request.stop_token.stop_requested()) {
            co_await gate->wait();
        }
        if (request.stop_token.stop_requested()) {
            cancelled_requests.fetch_add(1, std::memory_order_acq_rel);
            co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "request cancelled"));
        }
        co_return std::unexpected(support::make_error(support::ErrorCode::Network, "blocking request interrupted"));
    }

    std::atomic_size_t request_count{0};
    std::atomic_size_t cancelled_requests{0};
    std::atomic<bool> observed_stop_token{false};
};

class ScriptedTransport final : public support::StreamTransport {
public:
    [[nodiscard]] boost::asio::awaitable<support::Expected<support::StreamResponse>> async_stream(
            const support::StreamRequest& request, support::BodyChunkHandler on_body_chunk) override {
        requests.push_back(request);
        if (on_request) {
            on_request();
        }
        if (attempt_index >= attempts.size() && (!repeat_last_attempt || attempts.empty())) {
            co_return std::unexpected(
                    support::make_error(support::ErrorCode::Network, "no scripted transport attempt"));
        }
        auto attempt = attempt_index < attempts.size() ? attempts[attempt_index++] : attempts.back();
        if (attempt.failure) {
            co_return std::unexpected(*attempt.failure);
        }

        support::StreamResponse response{
                .head = std::move(attempt.head),
                .body = {},
        };
        for (const auto& chunk : attempt.chunks) {
            response.body += chunk;
            if (on_body_chunk && response.head.status_code >= 200 && response.head.status_code < 300) {
                CCH_TRY_VOID(on_body_chunk(chunk));
            }
        }
        co_return response;
    }

    std::vector<TransportAttempt> attempts;
    std::vector<support::StreamRequest> requests;
    std::size_t attempt_index{0};
    bool repeat_last_attempt{false};
    std::function<void()> on_request;
};

} // namespace cch::tests
