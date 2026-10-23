#include "codemode/Shared.hpp"

#include "support/Json.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::codemode::detail {

bool is_js_identifier(std::string_view part) {
    if (part.empty()) return false;
    const auto is_start = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '$';
    };
    if (!is_start(part.front())) return false;
    return std::all_of(part.begin() + 1, part.end(), [&](char c) { return is_start(c) || (c >= '0' && c <= '9'); });
}

support::ExpectedVoid validate_globals(const std::vector<CodemodeGlobal>& globals) {
    std::set<std::string> seen;
    std::set<std::string> plain;
    std::set<std::string> namespaces;
    for (const auto& global : globals) {
        bool valid = !global.name.empty() && global.name != "tools";
        std::size_t start = 0;
        while (valid) {
            const std::size_t dot = global.name.find('.', start);
            const std::string_view part = std::string_view{global.name}.substr(
                    start, dot == std::string::npos ? std::string_view::npos : dot - start);
            if (!is_js_identifier(part)) {
                valid = false;
                break;
            }
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        if (!valid) {
            return std::unexpected(sandbox_error(std::format("Invalid global name \"{}\"", global.name)));
        }
        if (!seen.insert(global.name).second) {
            return std::unexpected(sandbox_error(std::format("Global \"{}\" is already registered", global.name)));
        }
        const std::size_t dot = global.name.find('.');
        const std::string top = dot == std::string::npos ? global.name : global.name.substr(0, dot);
        if (dot == std::string::npos) {
            if (namespaces.contains(top)) {
                return std::unexpected(
                        sandbox_error(std::format("Global \"{}\" conflicts with the namespace \"{}\"", top, top)));
            }
            plain.insert(std::move(top));
        } else {
            if (plain.contains(top)) {
                return std::unexpected(
                        sandbox_error(std::format("Global \"{}\" conflicts with the namespace \"{}\"", top, top)));
            }
            namespaces.insert(std::move(top));
        }
    }
    return {};
}

std::string tools_json(const std::vector<CodemodeToolDescriptor>& tools) {
    support::JsonValue::array_t list;
    list.reserve(tools.size());
    for (const auto& tool : tools) {
        list.push_back(support::JsonValue::object_t{
                {"name", tool.name},
                {"jsName", tool.js_name},
                {"description", tool.description},
        });
    }
    auto written = support::write_json(support::JsonValue{std::move(list)});
    return written ? *written : std::string{"[]"};
}

std::string globals_json(const std::vector<CodemodeGlobal>& globals) {
    support::JsonValue::array_t list;
    list.reserve(globals.size());
    for (const auto& global : globals) {
        list.push_back(support::JsonValue::object_t{
                {"name", global.name},
                {"spread", global.spread},
        });
    }
    auto written = support::write_json(support::JsonValue{std::move(list)});
    return written ? *written : std::string{"[]"};
}

std::string store_json(const std::map<std::string, support::JsonValue>& store) {
    support::JsonValue::object_t object;
    for (const auto& [key, value] : store) {
        auto written = support::write_json(value);
        object.emplace(key, support::JsonValue{written ? std::move(*written) : std::string{"null"}});
    }
    auto written = support::write_json(support::JsonValue{std::move(object)});
    return written ? *written : std::string{"{}"};
}

/// pi `storeWrites` payload of `done(ok, payload, writes)`: `[[key, json], [key]]`
/// entries — a two-element entry sets the key, a one-element entry deletes it.
CodemodeStoreWrites parse_store_writes(std::string_view payload) {
    CodemodeStoreWrites writes;
    auto parsed = support::read_json(payload);
    if (!parsed || !parsed->holds<support::JsonValue::array_t>()) return writes;
    for (const auto& entry : parsed->get_array()) {
        if (!entry.holds<support::JsonValue::array_t>()) continue;
        const auto& fields = entry.get_array();
        if (fields.empty() || !fields[0].holds<std::string>()) continue;
        const std::string& key = fields[0].get_string();
        if (fields.size() == 1) {
            writes.deleted.push_back(key);
            continue;
        }
        if (!fields[1].holds<std::string>()) continue;
        // The guest hands the value back as JSON text; parse it so callers
        // merge structured values into the next snapshot.
        auto value = support::read_json(fields[1].get_string());
        writes.set.emplace(key, value ? std::move(*value) : support::JsonValue{});
    }
    return writes;
}

support::Error sandbox_error(std::string message, std::string detail) {
    return support::make_error(support::ErrorCode::Validation, std::move(message), std::move(detail));
}

} // namespace cch::codemode::detail
