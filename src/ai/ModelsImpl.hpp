#pragma once

#include <cch/ai/Models.hpp>

#include "support/StreamTransport.hpp"

#include <memory>
#include <string>
#include <vector>

namespace cch::ai {

struct Models::Impl {
    std::string api_key;
    bool request_started{false};
    std::string gateway_url;
    std::optional<support::Error> discovery_error{std::nullopt};
    std::vector<Model> catalog;
    std::shared_ptr<support::StreamTransport> transport;
    /// Private test mount typed over the same factory shape Models::stream exposes.
    ModelStreamFactory test_stream{};
};

} // namespace cch::ai
