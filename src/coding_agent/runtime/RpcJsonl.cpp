#include "RpcJsonl.hpp"

#include "util/Json.hpp"

#include <ostream>
#include <utility>

namespace cch::coding_agent::runtime::rpc_jsonl {
namespace {
constexpr std::size_t kMaxErrorBytes = 512;
}

std::string strip_trailing_cr(std::string line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

std::string bounded_error(std::string message) {
    if (message.size() > kMaxErrorBytes) {
        message.resize(kMaxErrorBytes);
    }
    return message;
}

std::optional<std::string> string_field(const util::JsonValue::object_t& object, const std::string& key) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return std::nullopt;
    }
    if (const auto* text = it->second.get_if<std::string>()) {
        return *text;
    }
    return std::nullopt;
}

bool has_non_string_field(const util::JsonValue::object_t& object, const std::string& key) {
    const auto it = object.find(key);
    return it != object.end() && !it->second.holds<std::string>();
}

util::JsonValue::object_t success_response(
    const std::optional<std::string>& id,
    const std::string& command,
    std::optional<util::JsonValue::object_t> data) {
    util::JsonValue::object_t record;
    if (id) {
        record.emplace("id", util::JsonValue{*id});
    }
    record.emplace("type", util::JsonValue{"response"});
    record.emplace("command", util::JsonValue{command});
    record.emplace("success", util::JsonValue{true});
    if (data) {
        record.emplace("data", util::JsonValue{std::move(*data)});
    }
    return record;
}

util::JsonValue::object_t error_response(
    const std::optional<std::string>& id,
    const std::string& command,
    std::string error) {
    util::JsonValue::object_t record;
    if (id) {
        record.emplace("id", util::JsonValue{*id});
    }
    record.emplace("type", util::JsonValue{"response"});
    record.emplace("command", util::JsonValue{command});
    record.emplace("success", util::JsonValue{false});
    record.emplace("error", util::JsonValue{bounded_error(std::move(error))});
    return record;
}

util::ExpectedVoid write_record(std::ostream& out, util::JsonValue::object_t record) {
    auto serialized = util::write_json(util::JsonValue{std::move(record)});
    if (!serialized) {
        return std::unexpected(serialized.error());
    }
    out << *serialized << '\n';
    if (!out) {
        return std::unexpected(util::make_error(
            util::ErrorCode::JsonSerialize,
            "failed to write RPC JSON record",
            "output stream failed"));
    }
    return {};
}

} // namespace cch::coding_agent::runtime::rpc_jsonl
