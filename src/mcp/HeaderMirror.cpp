#include "mcp/HeaderMirror.hpp"

#include "mcp/Protocol.hpp"
#include "support/Json.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace cch::mcp::headers {
namespace {

using support::ErrorCode;
using support::Expected;
using support::JsonValue;
using support::make_error;

constexpr std::string_view kBase64Alphabet{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};

/// Standard base64 with padding. The client only ever encodes: a mirrored
/// value is the server's to decode.
[[nodiscard]] std::string encode_base64(std::string_view bytes) {
    std::string encoded;
    encoded.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t index = 0;
    while (index + 3 <= bytes.size()) {
        const auto group = (static_cast<unsigned char>(bytes[index]) << 16) |
                           (static_cast<unsigned char>(bytes[index + 1]) << 8) |
                           static_cast<unsigned char>(bytes[index + 2]);
        encoded.push_back(kBase64Alphabet[(group >> 18) & 0x3f]);
        encoded.push_back(kBase64Alphabet[(group >> 12) & 0x3f]);
        encoded.push_back(kBase64Alphabet[(group >> 6) & 0x3f]);
        encoded.push_back(kBase64Alphabet[group & 0x3f]);
        index += 3;
    }
    const auto remaining = bytes.size() - index;
    if (remaining == 1) {
        const auto group = static_cast<unsigned char>(bytes[index]) << 16;
        encoded.push_back(kBase64Alphabet[(group >> 18) & 0x3f]);
        encoded.push_back(kBase64Alphabet[(group >> 12) & 0x3f]);
        encoded.append("==");
    } else if (remaining == 2) {
        const auto group =
                (static_cast<unsigned char>(bytes[index]) << 16) | (static_cast<unsigned char>(bytes[index + 1]) << 8);
        encoded.push_back(kBase64Alphabet[(group >> 18) & 0x3f]);
        encoded.push_back(kBase64Alphabet[(group >> 12) & 0x3f]);
        encoded.push_back(kBase64Alphabet[(group >> 6) & 0x3f]);
        encoded.push_back('=');
    }
    return encoded;
}

[[nodiscard]] bool is_printable_ascii(std::string_view value) {
    for (const char ch : value) {
        if (ch < ' ' || ch > '~') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_json_null(const JsonValue& value) { return value.holds<JsonValue::null_t>(); }

/// The argument value's header text. A structured value is mirrored as its
/// compact JSON text, which is lossless and already printable ASCII.
[[nodiscard]] Expected<std::string> render_value(const JsonValue& value) {
    if (const auto* text = value.get_if<std::string>(); text != nullptr) {
        return *text;
    }
    if (value.get_if<double>() != nullptr || value.get_if<bool>() != nullptr ||
            value.get_if<JsonValue::object_t>() != nullptr || value.get_if<JsonValue::array_t>() != nullptr) {
        auto json = support::write_json(value);
        if (!json) {
            return std::unexpected(json.error());
        }
        return *json;
    }
    return std::unexpected(make_error(ErrorCode::Validation,
            "an annotated argument has an unmappable value",
            "a mirrored argument must be a string, number, boolean, object, or array"));
}

[[nodiscard]] std::string apply_sentinel(std::string value) {
    if (is_printable_ascii(value) && !value.starts_with(protocol::kHeaderValueBase64Sentinel)) {
        return value;
    }
    return std::string(protocol::kHeaderValueBase64Sentinel) + encode_base64(value);
}

} // namespace

Expected<std::map<std::string, std::string>> mirror_parameter_headers(
        const UpstreamToolDescriptor& tool, const JsonValue& arguments) {
    if (tool.header_parameters.empty()) {
        return std::map<std::string, std::string>{};
    }
    const auto* supplied = arguments.get_if<JsonValue::object_t>();
    std::map<std::string, std::string> mirrored;
    for (const auto& parameter : tool.header_parameters) {
        const JsonValue* argument = nullptr;
        if (supplied != nullptr) {
            const auto found = supplied->find(parameter.argument_name);
            argument = found == supplied->end() ? nullptr : &found->second;
        }
        if (argument == nullptr || is_json_null(*argument)) {
            if (parameter.required) {
                return std::unexpected(make_error(ErrorCode::Validation,
                        "a required mirrored tool argument is missing",
                        "tool \"" + tool.name + "\" requires \"" + parameter.argument_name + "\""));
            }
            continue;
        }
        auto rendered = render_value(*argument);
        if (!rendered) {
            return std::unexpected(std::move(rendered).error());
        }
        mirrored.emplace(std::string(protocol::kHeaderParamPrefix) + parameter.header_name,
                apply_sentinel(std::move(*rendered)));
    }
    return mirrored;
}

} // namespace cch::mcp::headers
