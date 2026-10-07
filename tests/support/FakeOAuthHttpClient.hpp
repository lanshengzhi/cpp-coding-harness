#pragma once

#include "ai/auth/OAuthHttpClient.hpp"

#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace cch::tests {

/// Scripted OAuthHttpClient for the auth tests (#721): per-URL response
/// queues, a recorded request log, and optional scripted transport failures
/// (first N requests) or response delay. One behavior note: every scripted
/// point observes the request stop token and reports Cancelled when it fires.
///
/// GET responses (OAuth discovery, #884) are scripted independently of the
/// POST queues, so a test can give the same URL different answers to a
/// metadata fetch and a token exchange.
class FakeOAuthHttpClient final : public ai::auth::OAuthHttpClient {
public:
    struct Request {
        std::string url;
        std::map<std::string, std::string, std::less<>> headers;
        std::string body;
        std::stop_token stop_token;
    };
    struct GetRequest {
        std::string url;
        std::map<std::string, std::string, std::less<>> headers;
        std::stop_token stop_token;
    };
    struct ScriptedResponse {
        int status{200};
        std::string body;
    };

    boost::asio::awaitable<support::Expected<ai::auth::OAuthHttpResponse>>
    get(std::string url, std::map<std::string, std::string, std::less<>> headers, std::stop_token stop_token) override {
        get_requests.push_back(GetRequest{
                std::move(url),
                std::move(headers),
                stop_token,
        });
        auto& queue = get_responses[get_requests.back().url];
        if (queue.empty()) {
            co_return std::unexpected(support::make_error(
                    support::ErrorCode::Network, "no scripted GET response for " + get_requests.back().url));
        }
        auto scripted = std::move(queue.front());
        queue.pop_front();
        if (stop_token.stop_requested()) {
            co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "fake client cancelled"));
        }
        co_return ai::auth::OAuthHttpResponse{
                .status_code = scripted.status,
                .body = std::move(scripted.body),
        };
    }

    boost::asio::awaitable<support::Expected<ai::auth::OAuthHttpResponse>> post(std::string url,
            std::map<std::string, std::string, std::less<>> headers,
            std::string body,
            std::stop_token stop_token) override {
        requests.push_back(Request{
                url,
                std::move(headers),
                std::move(body),
                stop_token,
        });
        if (fail_first_n_requests > 0) {
            --fail_first_n_requests;
            if (stop_token.stop_requested()) {
                co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "fake client cancelled"));
            }
            co_return std::unexpected(
                    failure_error.value_or(support::make_error(support::ErrorCode::Network, "connection reset")));
        }
        if (respond_delay > std::chrono::milliseconds::zero()) {
            // Emulate a real transport that observes the composed request stop
            // token while a request is in flight.
            auto executor = co_await boost::asio::this_coro::executor;
            boost::asio::steady_timer timer(executor, respond_delay);
            boost::system::error_code error;
            co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
            if (stop_token.stop_requested()) {
                co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "fake client cancelled"));
            }
        }
        auto& queue = responses[requests.back().url];
        if (queue.empty()) {
            co_return std::unexpected(support::make_error(
                    support::ErrorCode::Network, "no scripted response for " + requests.back().url));
        }
        auto scripted = std::move(queue.front());
        queue.pop_front();
        if (stop_token.stop_requested()) {
            co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "fake client cancelled"));
        }
        co_return ai::auth::OAuthHttpResponse{
                .status_code = scripted.status,
                .body = std::move(scripted.body),
        };
    }

    std::map<std::string, std::deque<ScriptedResponse>, std::less<>> responses;
    std::vector<Request> requests;
    std::map<std::string, std::deque<ScriptedResponse>, std::less<>> get_responses;
    std::vector<GetRequest> get_requests;
    std::optional<support::Error> failure_error;
    int fail_first_n_requests{0};
    std::chrono::milliseconds respond_delay{0};
};

} // namespace cch::tests
