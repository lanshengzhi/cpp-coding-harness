#pragma once

#include <boost/json.hpp>

#include <string>

namespace cch::ai {

struct ToolDefinition {
    std::string name;
    std::string description;
    boost::json::object parameters;
};

} // namespace cch::ai
