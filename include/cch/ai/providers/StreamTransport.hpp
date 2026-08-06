#pragma once

#include <cch/util/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <functional>
#include <map>
#include <stop_token>
#include <string>
#include <string_view>

namespace cch::ai::providers {

struct StreamRequest {
    std::string method{"POST"};
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
    /// Bounds connection setup, request dispatch, and response headers. Body
    /// streaming remains active until completion or caller cancellation.
    std::chrono::milliseconds timeout{30000};
    /// Active provider-request cancellation token. Transports return a
    /// Cancelled error when the request is stopped so the provider can produce
    /// the ordinary assistant `aborted` terminal outcome.
    std::stop_token stop_token{};
};

struct StreamResponseHead {
    int status_code{0};
    std::map<std::string, std::string> headers;
};

/// One received HTTP response, including non-success statuses so protocol
/// adapters can apply their retry and terminal-error policy.
struct StreamResponse {
    StreamResponseHead head;
    std::string body;
};

using BodyChunkHandler = std::move_only_function<util::ExpectedVoid(std::string_view)>;

class StreamTransport {
public:
    virtual ~StreamTransport() = default;

    /// Streams one HTTP request. The borrowed StreamRequest is read across
    /// suspension points, so it must remain valid until the returned
    /// awaitable completes.
    ///
    /// Executor contract: the transport is driven by the calling executor and
    /// is not internally synchronized; drive it from a single-threaded executor
    /// and do not run `async_stream` on the same transport from two threads.
    [[nodiscard]] virtual boost::asio::awaitable<util::Expected<StreamResponse>> async_stream(
        const StreamRequest& request,
        BodyChunkHandler on_body_chunk) = 0;
};

} // namespace cch::ai::providers
