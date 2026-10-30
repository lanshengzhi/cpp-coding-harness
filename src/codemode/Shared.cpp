#include "codemode/Shared.hpp"

#include "support/Json.hpp"

#include <cch/codemode/Declarations.hpp>

#include <cmath>
#include <optional>

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

support::ExpectedVoid validate_run_options(const CodemodeRunOptions& options) {
    if (options.started_at < 0) {
        return std::unexpected(
                sandbox_error("startedAt must be a non-negative number of milliseconds since the epoch"));
    }
    if (options.seed > 0xffffffffull) {
        return std::unexpected(sandbox_error("seed must be a 32-bit unsigned integer"));
    }
    if (options.expected_seed.has_value() && *options.expected_seed != options.seed) {
        return std::unexpected(sandbox_error(std::format(
                "Deterministic replay seed mismatch: expected {}, got {}", *options.expected_seed, options.seed)));
    }
    std::set<std::string> names;
    for (const auto& tool : options.tools) {
        if (!names.insert(tool.name).second) return std::unexpected(sandbox_error("Tool names must be unique"));
    }
    std::set<std::string> script_names;
    for (const auto& tool : options.tools) {
        const std::string js_name = tool.js_name.empty() ? to_codemode_identifier(tool.name) : tool.js_name;
        if (!script_names.insert(js_name).second) {
            return std::unexpected(sandbox_error("Tool script names must be unique"));
        }
    }
    return validate_globals(options.globals);
}

std::optional<CodemodeDeliveryRejection> delivery_rejection(std::int64_t seq,
        double now,
        std::int64_t id,
        const CodemodeOutcome& outcome,
        bool sequence_used,
        bool call_waiting) {
    using Reason = CodemodeDeliveryRejection::Reason;
    // pi `Execution.deliver` validates on the caller's side before posting
    // anything, so a refused answer leaves the call waiting and the run
    // unsettled; the caller can retry with a valid one. The order matters
    // because one answer can violate more than one condition.
    if (!std::isfinite(now)) {
        return CodemodeDeliveryRejection{
                Reason::Clock, std::format("The clock value of result {} is not a finite number", seq)};
    }
    if (seq < 1 || sequence_used) {
        return CodemodeDeliveryRejection{
                Reason::ResultNumber, std::format("Result number {} is not a positive integer used once", seq)};
    }
    if (!call_waiting) {
        return CodemodeDeliveryRejection{Reason::UnknownCall, std::format("Call {} is not waiting for a result", id)};
    }
    if (!outcome.ok && outcome.properties_json.has_value()) {
        const auto properties = support::read_json(*outcome.properties_json);
        if (!properties.has_value() || !properties->holds<support::JsonValue::object_t>()) {
            return CodemodeDeliveryRejection{
                    Reason::Properties, std::format("The properties of result {} are not an object", seq)};
        }
    }
    if (outcome.ok && !outcome.value_json.empty() && !support::read_json(outcome.value_json).has_value()) {
        // pi serializes the value with `JSON.stringify` before recording
        // anything: a value JSON cannot hold throws and leaves the call
        // waiting for another answer. This API receives the text already, so
        // the same refusal is text that is not a JSON value.
        return CodemodeDeliveryRejection{Reason::Serialization, std::format("The value of result {} is not JSON", seq)};
    }
    return std::nullopt;
}

} // namespace cch::codemode::detail
