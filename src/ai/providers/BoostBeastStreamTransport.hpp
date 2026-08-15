#pragma once

#include "ai/providers/StreamTransport.hpp"

namespace cch::ai::providers {

class BoostBeastStreamTransport final : public StreamTransport {
public:
    [[nodiscard]] boost::asio::awaitable<support::Expected<StreamResponse>> async_stream(
        const StreamRequest& request,
        BodyChunkHandler on_body_chunk) override;
};

} // namespace cch::ai::providers
