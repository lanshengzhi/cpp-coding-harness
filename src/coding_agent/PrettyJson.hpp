#pragma once

#include "support/Json.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::detail {

[[nodiscard]] inline support::ExpectedVoid append_pretty_json(
        const support::JsonValue& value, std::string& output, std::size_t indentation) {
    using JsonObject = support::JsonValue::object_t;

    if (const auto* object = value.get_if<JsonObject>()) {
        output.push_back('{');
        if (!object->empty()) {
            output.push_back('\n');
            // Sorted members: the settings writer's text is canonical, so the
            // emitted file does not depend on parse or insertion order.
            std::vector<std::string_view> keys;
            keys.reserve(object->size());
            for (const auto& member : *object) {
                keys.push_back(member.first);
            }
            std::sort(keys.begin(), keys.end());
            for (std::size_t index = 0; index < keys.size(); ++index) {
                const auto& member = object->at(keys[index]);
                output.append(indentation + 2, ' ');
                auto key = support::write_json(support::JsonValue{std::string{keys[index]}});
                if (!key) {
                    return std::unexpected(key.error());
                }
                output.append(*key);
                output.append(": ");
                if (auto appended = append_pretty_json(member, output, indentation + 2); !appended) {
                    return appended;
                }
                if (index + 1 != keys.size()) {
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
