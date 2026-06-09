#pragma once

#include <boost/json.hpp>

#include <string>
#include <vector>

namespace cch::util {

inline boost::json::object string_property(std::string description) {
    boost::json::object property;
    property["type"] = "string";
    property["description"] = std::move(description);
    return property;
}

inline boost::json::object integer_property(std::string description) {
    boost::json::object property;
    property["type"] = "integer";
    property["description"] = std::move(description);
    return property;
}

inline boost::json::object boolean_property(std::string description) {
    boost::json::object property;
    property["type"] = "boolean";
    property["description"] = std::move(description);
    return property;
}

inline boost::json::object object_schema(boost::json::object properties, std::vector<std::string> required) {
    boost::json::object schema;
    schema["type"] = "object";
    schema["properties"] = std::move(properties);
    boost::json::array req;
    for (const auto& item : required) {
        req.push_back(boost::json::value(item));
    }
    schema["required"] = std::move(req);
    schema["additionalProperties"] = false;
    return schema;
}

} // namespace cch::util
