#pragma once

#include "support/Json.hpp"

#include <cstddef>
#include <string>

namespace cch::coding_agent::detail {

[[nodiscard]] inline support::ExpectedVoid append_pretty_json(
        const support::JsonValue& value, std::string& output, std::size_t indentation) {
    using JsonObject = support::JsonValue::object_t;

    if (const auto* object = value.get_if<JsonObject>()) {
        output.push_back('{');
        if (!object->empty()) {
            output.push_back('\n');
            auto current = object->begin();
            while (current != object->end()) {
                output.append(indentation + 2, ' ');
                auto key = support::write_json(support::JsonValue{current->first});
                if (!key) {
                    return std::unexpected(key.error());
                }
                output.append(*key);
                output.append(": ");
                if (auto appended = append_pretty_json(current->second, output, indentation + 2); !appended) {
                    return appended;
                }
                ++current;
                if (current != object->end()) {
                    output.push_back(',');
                }
                output.push_back('\n');
            }
            output.append(indentation, ' ');
        }
        output.push_back('}');
        return {};
    }

    if (const auto* array = value.get_if<support::JsonValue::array_t>()) {
        output.push_back('[');
        if (!array->empty()) {
            output.push_back('\n');
            for (std::size_t index = 0; index < array->size(); ++index) {
                output.append(indentation + 2, ' ');
                if (auto appended = append_pretty_json((*array)[index], output, indentation + 2); !appended) {
                    return appended;
                }
                if (index + 1 != array->size()) {
                    output.push_back(',');
                }
                output.push_back('\n');
            }
            output.append(indentation, ' ');
        }
        output.push_back(']');
        return {};
    }

    auto serialized = support::write_json(value);
    if (!serialized) {
        return std::unexpected(serialized.error());
    }
    output.append(*serialized);
    return {};
}

[[nodiscard]] inline support::Expected<std::string> serialize_pretty_json(
        const support::JsonValue& value, bool trailing_newline) {
    std::string output;
    if (auto appended = append_pretty_json(value, output, 0); !appended) {
        return std::unexpected(appended.error());
    }
    if (trailing_newline) {
        output.push_back('\n');
    }
    return output;
}

} // namespace cch::coding_agent::detail
