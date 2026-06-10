#pragma once

#include <cch/util/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace cch::ai::providers {

struct StreamRequest {
    std::string method{"POST"};
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
    std::chrono::milliseconds timeout{30000};
};

struct StreamResponseHead {
    int status_code{0};
    std::map<std::string, std::string> headers;
};

struct StreamResponse {
    StreamResponseHead head;
    std::string body;
};

using BodyChunkHandler = std::function<util::ExpectedVoid(std::string_view)>;

class StreamTransport {
public:
    virtual ~StreamTransport() = default;

    [[nodiscard]] virtual boost::asio::awaitable<util::Expected<StreamResponse>> async_stream(
        const StreamRequest& request,
        BodyChunkHandler on_body_chunk) = 0;
};

class BoostBeastStreamTransport final : public StreamTransport {
public:
    [[nodiscard]] boost::asio::awaitable<util::Expected<StreamResponse>> async_stream(
        const StreamRequest& request,
        BodyChunkHandler on_body_chunk) override;
};

} // namespace cch::ai::providers
