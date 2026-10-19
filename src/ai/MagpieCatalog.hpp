#pragma once

#include <cch/ai/Model.hpp>
#include <cch/support/Error.hpp>
#include "support/StreamTransport.hpp"

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace cch::ai {

struct MagpieHelloInfo {
    std::string name;
    std::string version;
};

/// Pure value transformations for gateway payloads (CODING_STANDARDS.md §13.1, §13.2).
[[nodiscard]] support::Expected<MagpieHelloInfo> parse_magpie_hello_response(
        std::string_view body, std::string_view url = {});

[[nodiscard]] support::Expected<std::vector<Model>> parse_magpie_models_response(
        std::string_view body, std::string_view url = {});

/// The transport is borrowed and must outlive the returned operation. URLs
/// are owned by the coroutine across suspension points.
[[nodiscard]] boost::asio::awaitable<support::Expected<MagpieHelloInfo>> fetch_magpie_hello(
        support::StreamTransport& transport,
        std::string base_url,
        std::string api_key,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{3000},
        std::stop_token stop_token = {});

/// The transport is borrowed and must outlive the returned operation. URLs
/// are owned by the coroutine across suspension points.
[[nodiscard]] boost::asio::awaitable<support::Expected<std::vector<Model>>> fetch_magpie_models(
        support::StreamTransport& transport,
        std::string base_url,
        std::string api_key,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{3000},
        std::stop_token stop_token = {});

} // namespace cch::ai
