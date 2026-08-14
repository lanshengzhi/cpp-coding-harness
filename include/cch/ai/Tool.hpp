#pragma once

#include <cch/support/JsonValue.hpp>

#include <string>

namespace cch::ai {

struct Tool {
    std::string name{};
    std::string description{};
    cch::support::JsonValue parameters{};
};

} // namespace cch::ai
