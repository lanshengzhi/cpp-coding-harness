#include "mcp/JsonRpc.hpp"

#include "mcp/Protocol.hpp"
#include "support/Json.hpp"

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cch::mcp::jsonrpc {
namespace {

using support::Error;
using support::ErrorCode;
using support::Expected;
using support::JsonValue;
using support::make_error;

/// The largest JSON-RPC id this build round-trips. Above it a `double` id can
/// no longer represent consecutive values, so anything larger is rejected
/// instead of compared approximately.
constexpr double kMaxExactId = 9007199254740992.0; // 2^53

[[nodiscard]] bool is_integral(double value) { return std::isfinite(value) && value == std::trunc(value); }

[[nodiscard]] std::optional<std::int64_t> as_integral(const JsonValue& value) {
    const auto* number = value.get_if<double>();
    if (number == nullptr || !is_integral(*number)) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(*number);
}

[[nodiscard]] std::optional<std::string> as_string(const JsonValue& value) {
    const auto* text = value.get_if<std::string>();
    if (text == nullptr) {
        return std::nullopt;
    }
    return *text;
}

/// The member named `key`, or `nullptr` when the value is not an object or
/// carries no such member. `cch::support::JsonValue` has no `contains()`.
[[nodiscard]] const JsonValue* member(const JsonValue& value, std::string_view key) {
    const auto* object = value.get_if<JsonValue::object_t>();
    if (object == nullptr) {
        return nullptr;
    }
    const auto found = object->find(std::string(key));
    return found == object->end() ? nullptr : &found->second;
}

[[nodiscard]] bool is_null(const JsonValue& value) { return value.holds<JsonValue::null_t>(); }

[[nodiscard]] Error violation(std::string detail) {
    return make_error(ErrorCode::Validation, "malformed JSON-RPC message", std::move(detail));
}

[[nodiscard]] JsonValue error_object(int code, std::string message, JsonValue data) {
    JsonValue::object_t error{
            {"code", JsonValue(static_cast<double>(code))},
            {"message", JsonValue(std::move(message))},
    };
    if (!is_null(data)) {
        error.emplace("data", std::move(data));
    }
    return JsonValue(std::move(error));
}

/// Byte offset just past the first complete JSON object or array in `body` at
/// or after `start`, or `std::nullopt` when no complete frame starts there.
/// String contents and their escapes do not change the bracket depth, so a
/// `}` or `]` inside a string cannot end a frame early.
[[nodiscard]] std::optional<std::size_t> frame_end(std::string_view body, std::size_t start) {
    std::size_t index = start;
    while (index < body.size() && std::isspace(static_cast<unsigned char>(body[index])) != 0) {
        ++index;
    }
    if (index >= body.size() || (body[index] != '{' && body[index] != '[')) {
        return std::nullopt;
    }
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (; index < body.size(); ++index) {
        const char ch = body[index];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
        } else if (ch == '{' || ch == '[') {
            ++depth;
        } else if (ch == '}' || ch == ']') {
            --depth;
            if (depth == 0) {
                return index + 1;
            }
            if (depth < 0) {
                return std::nullopt;
            }
        }
    }
    return std::nullopt;
}

} // namespace

JsonValue encode_request(double id, std::string_view method, JsonValue params) {
    return JsonValue(JsonValue::object_t{
            {"id", JsonValue(id)},
            {"jsonrpc", JsonValue(std::string(protocol::kJsonRpcVersion))},
            {"method", JsonValue(std::string(method))},
            {"params", std::move(params)},
    });
}

JsonValue encode_notification(std::string_view method, JsonValue params) {
    return JsonValue(JsonValue::object_t{
            {"jsonrpc", JsonValue(std::string(protocol::kJsonRpcVersion))},
            {"method", JsonValue(std::string(method))},
            {"params", std::move(params)},
    });
}

JsonValue encode_result(double id, JsonValue result) {
    return JsonValue(JsonValue::object_t{
            {"id", JsonValue(id)},
            {"jsonrpc", JsonValue(std::string(protocol::kJsonRpcVersion))},
            {"result", std::move(result)},
    });
}

JsonValue encode_error(double id, int code, std::string message, JsonValue data) {
    return JsonValue(JsonValue::object_t{
            {"error", error_object(code, std::move(message), std::move(data))},
            {"id", JsonValue(id)},
            {"jsonrpc", JsonValue(std::string(protocol::kJsonRpcVersion))},
    });
}

Expected<WireMessage> decode_message(const JsonValue& value) {
    if (value.get_if<JsonValue::object_t>() == nullptr) {
        return std::unexpected(violation("a JSON-RPC message is not a JSON object"));
    }
    const auto* raw_version = member(value, "jsonrpc");
    if (raw_version == nullptr) {
        return std::unexpected(violation("the \"jsonrpc\" member is missing"));
    }
    const auto version = as_string(*raw_version);
    if (!version.has_value() || *version != protocol::kJsonRpcVersion) {
        return std::unexpected(violation("the \"jsonrpc\" member is not the string \"2.0\""));
    }

    WireMessage message;
    if (const auto* raw_id = member(value, "id"); raw_id != nullptr && !is_null(*raw_id)) {
        const auto integral = as_integral(*raw_id);
        if (!integral.has_value() || *integral > kMaxExactId || *integral < -kMaxExactId) {
            return std::unexpected(violation("the \"id\" member is not a whole number this build can round-trip"));
        }
        message.id = static_cast<double>(*integral);
    }

    if (const auto* raw_method = member(value, "method"); raw_method != nullptr) {
        const auto method = as_string(*raw_method);
        if (!method.has_value() || method->empty()) {
            return std::unexpected(violation("the \"method\" member is not a non-empty string"));
        }
        message.method = *method;
        if (const auto* raw_params = member(value, "params"); raw_params != nullptr) {
            if (raw_params->get_if<JsonValue::object_t>() == nullptr &&
                    raw_params->get_if<JsonValue::array_t>() == nullptr) {
                return std::unexpected(violation("the \"params\" member is neither an object nor an array"));
            }
            message.params = *raw_params;
        }
        return message;
    }

    if (!message.id.has_value()) {
        return std::unexpected(violation("a response message carries no \"id\""));
    }
    const auto* raw_error = member(value, "error");
    const auto* raw_result = member(value, "result");
    if ((raw_error == nullptr) == (raw_result == nullptr)) {
        return std::unexpected(violation("a response message carries neither or both of \"result\" and \"error\""));
    }
    if (raw_error != nullptr) {
        if (raw_error->get_if<JsonValue::object_t>() == nullptr) {
            return std::unexpected(violation("the \"error\" member is not a JSON object"));
        }
        const auto* raw_code = member(*raw_error, "code");
        const auto* raw_message = member(*raw_error, "message");
        const auto code = raw_code == nullptr ? std::optional<std::int64_t>{} : as_integral(*raw_code);
        const auto error_message = raw_message == nullptr ? std::optional<std::string>{} : as_string(*raw_message);
        if (!code.has_value() || *code < std::numeric_limits<int>::min() || *code > std::numeric_limits<int>::max() ||
                !error_message.has_value()) {
            return std::unexpected(
                    violation("the \"error\" member has no whole-number \"code\" or no string \"message\""));
        }
        message.is_error = true;
        message.error_code = static_cast<int>(*code);
        message.error_message = *error_message;
        if (const auto* raw_data = member(*raw_error, "data"); raw_data != nullptr) {
            message.error_data = *raw_data;
        }
        return message;
    }
    message.result = *raw_result;
    message.has_result = true;
    return message;
}

Expected<std::vector<JsonValue>> split_messages(std::string_view body) {
    std::vector<JsonValue> messages;
    std::size_t index = 0;
    while (index < body.size()) {
        const auto end = frame_end(body, index);
        if (!end.has_value()) {
            return std::unexpected(make_error(ErrorCode::Validation,
                    "malformed JSON-RPC frame",
                    "no complete JSON-RPC message at or after byte offset " + std::to_string(index)));
        }
        auto parsed = support::read_json(body.substr(index, *end - index));
        if (!parsed) {
            return std::unexpected(make_error(ErrorCode::Validation,
                    "malformed JSON-RPC frame",
                    "the frame at byte offset " + std::to_string(index) + " is not readable JSON"));
        }
        messages.push_back(std::move(*parsed));
        index = *end;
    }
    return messages;
}

} // namespace cch::mcp::jsonrpc
