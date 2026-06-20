#pragma once

#include "../../../include/cch/util/Error.hpp"
#include "../../../include/cch/util/JsonValue.hpp"

#include <iosfwd>
#include <optional>
#include <string>

namespace cch::coding_agent::runtime::rpc_jsonl {

[[nodiscard]] std::string strip_trailing_cr(std::string line);
[[nodiscard]] std::string bounded_error(std::string message);

[[nodiscard]] std::optional<std::string> string_field(const util::JsonValue::object_t& object, const std::string& key);
[[nodiscard]] bool has_non_string_field(const util::JsonValue::object_t& object, const std::string& key);

[[nodiscard]] util::JsonValue::object_t success_response(
    const std::optional<std::string>& id,
    const std::string& command,
    std::optional<util::JsonValue::object_t> data = std::nullopt);

[[nodiscard]] util::JsonValue::object_t error_response(
    const std::optional<std::string>& id,
    const std::string& command,
    std::string error);

[[nodiscard]] util::ExpectedVoid write_record(std::ostream& out, util::JsonValue::object_t record);

} // namespace cch::coding_agent::runtime::rpc_jsonl
