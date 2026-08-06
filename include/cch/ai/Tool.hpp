#pragma once

#include "../util/JsonValue.hpp"

#include <string>

namespace cch::ai {

struct Tool {
    std::string name{};
    std::string description{};
    util::JsonValue parameters{};
};

} // namespace cch::ai
