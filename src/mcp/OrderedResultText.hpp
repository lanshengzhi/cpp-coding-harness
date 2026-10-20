#pragma once

// Order-preserving capture of the `tools/call` result member. `support::JsonValue`
// stores objects in a `std::map`, so a value parsed by the ordinary transport path
// cannot reproduce JavaScript's observable `JSON.stringify` member order.
//
// The transport parses each response once with `support::read_json`; that value
// remains the source of truth for every scalar and the validation authority.
// This private cch_mcp helper scans the same already-validated bytes a second
// time only to record member names and sequence: value strings are skipped
// without decoding, numbers/literals are skipped as tokens, and no value is
// reconstructed. Object keys are decoded because they are the lookup key into
// the JsonValue object. The two passes cannot be merged without changing the
// shared parser or exposing an ordered value type across the Owner boundary.
// The skeleton never crosses the Client interface.

#include "support/Json.hpp"

#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace cch::mcp::detail {

/// The order skeleton of one JSON value. Scalars carry no payload; objects keep
/// member names and arrays keep child skeletons.
struct OrderNode {
    enum class Kind { Scalar, Array, Object };
    Kind kind{Kind::Scalar};
    std::vector<OrderNode> elements;
    std::vector<std::pair<std::string, OrderNode>> members;
};

[[nodiscard]] inline std::optional<std::uint32_t> javascript_array_index(std::string_view key) {
    if (key.empty()) {
        return std::nullopt;
    }
    if (key == "0") {
        return 0;
    }
    if (key.front() == '0') {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char character : key) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint64_t>(character - '0');
        if (value > 4294967294ULL) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint32_t>(value);
}

/// JSON.parse property order: canonical array-index keys first, ascending, then
/// the remaining string keys in insertion order. A duplicate key updates its
/// existing position and value, matching JavaScript assignment.
inline void normalize_javascript_object(std::vector<std::pair<std::string, OrderNode>>& members) {
    std::vector<std::pair<std::uint32_t, std::size_t>> integer_keys;
    integer_keys.reserve(members.size());
    for (std::size_t index = 0; index < members.size(); ++index) {
        if (const auto array_index = javascript_array_index(members[index].first)) {
            integer_keys.emplace_back(*array_index, index);
        }
    }
    if (integer_keys.empty()) {
        return;
    }
    std::stable_sort(integer_keys.begin(), integer_keys.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    std::vector<std::pair<std::string, OrderNode>> ordered;
    ordered.reserve(members.size());
    for (const auto& [unused, index] : integer_keys) {
        (void)unused;
        ordered.push_back(std::move(members[index]));
    }
    for (std::size_t index = 0; index < members.size(); ++index) {
        if (javascript_array_index(members[index].first)) {
            continue;
        }
        ordered.push_back(std::move(members[index]));
    }
    members = std::move(ordered);
}

class OrderParser final {
public:
    explicit OrderParser(std::string_view text) : text_(text) {}

    [[nodiscard]] bool parse(OrderNode& out) {
        skip_whitespace();
        if (!parse_value(out)) {
            return false;
        }
        skip_whitespace();
        return position_ == text_.size();
    }

private:
    [[nodiscard]] bool parse_value(OrderNode& out) {
        skip_whitespace();
        if (position_ >= text_.size()) {
            return false;
        }
        switch (text_[position_]) {
        case '{':
            return parse_object(out);
        case '[':
            return parse_array(out);
        case '"':
            if (!parse_string(nullptr)) {
                return false;
            }
            out = OrderNode{};
            return true;
        case 't':
            return parse_literal("true", out);
        case 'f':
            return parse_literal("false", out);
        case 'n':
            return parse_literal("null", out);
        default:
            return skip_number(out);
        }
    }

    [[nodiscard]] bool parse_object(OrderNode& out) {
        ++position_; // '{'
        out.kind = OrderNode::Kind::Object;
        skip_whitespace();
        if (position_ < text_.size() && text_[position_] == '}') {
            ++position_;
            return true;
        }
        while (true) {
            skip_whitespace();
            std::string key;
            if (!parse_string(&key)) {
                return false;
            }
            skip_whitespace();
            if (position_ >= text_.size() || text_[position_] != ':') {
                return false;
            }
            ++position_;
            OrderNode child;
            if (!parse_value(child)) {
                return false;
            }
            auto existing = std::find_if(
                    out.members.begin(), out.members.end(), [&key](const auto& member) { return member.first == key; });
            if (existing == out.members.end()) {
                out.members.emplace_back(std::move(key), std::move(child));
            } else {
                existing->second = std::move(child);
            }
            skip_whitespace();
            if (position_ >= text_.size()) {
                return false;
            }
            if (text_[position_] == ',') {
                ++position_;
                continue;
            }
            if (text_[position_] == '}') {
                ++position_;
                normalize_javascript_object(out.members);
                return true;
            }
            return false;
        }
    }

    [[nodiscard]] bool parse_array(OrderNode& out) {
        ++position_; // '['
        out.kind = OrderNode::Kind::Array;
        skip_whitespace();
        if (position_ < text_.size() && text_[position_] == ']') {
            ++position_;
            return true;
        }
        while (true) {
            OrderNode child;
            if (!parse_value(child)) {
                return false;
            }
            out.elements.push_back(std::move(child));
            skip_whitespace();
            if (position_ >= text_.size()) {
                return false;
            }
            if (text_[position_] == ',') {
                ++position_;
                continue;
            }
            if (text_[position_] == ']') {
                ++position_;
                return true;
            }
            return false;
        }
    }

    [[nodiscard]] bool parse_string(std::string* out) {
        if (position_ >= text_.size() || text_[position_] != '"') {
            return false;
        }
        ++position_;
        while (position_ < text_.size()) {
            const char character = text_[position_++];
            if (character == '"') {
                return true;
            }
            if (character != '\\') {
                if (out != nullptr) {
                    out->push_back(character);
                }
                continue;
            }
            if (position_ >= text_.size()) {
                return false;
            }
            const char escaped = text_[position_++];
            if (out == nullptr) {
                if (escaped == 'u') {
                    position_ += 4;
                    if (position_ > text_.size()) {
                        return false;
                    }
                }
                continue;
            }
            switch (escaped) {
            case '"':
                out->push_back('"');
                break;
            case '\\':
                out->push_back('\\');
                break;
            case '/':
                out->push_back('/');
                break;
            case 'b':
                out->push_back('\b');
                break;
            case 'f':
                out->push_back('\f');
                break;
            case 'n':
                out->push_back('\n');
                break;
            case 'r':
                out->push_back('\r');
                break;
            case 't':
                out->push_back('\t');
                break;
            case 'u': {
                std::uint32_t code_point = 0;
                if (!parse_hex4(code_point)) {
                    return false;
                }
                if (code_point >= 0xD800 && code_point <= 0xDBFF) {
                    if (position_ + 1 >= text_.size() || text_[position_] != '\\' || text_[position_ + 1] != 'u') {
                        return false;
                    }
                    position_ += 2;
                    std::uint32_t low = 0;
                    if (!parse_hex4(low) || low < 0xDC00 || low > 0xDFFF) {
                        return false;
                    }
                    code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
                } else if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
                    return false;
                }
                append_utf8(*out, code_point);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    [[nodiscard]] bool parse_hex4(std::uint32_t& out) {
        if (position_ + 4 > text_.size()) {
            return false;
        }
        out = 0;
        for (int index = 0; index < 4; ++index) {
            const char character = text_[position_++];
            out <<= 4U;
            if (character >= '0' && character <= '9') {
                out |= static_cast<std::uint32_t>(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                out |= static_cast<std::uint32_t>(character - 'a' + 10);
            } else if (character >= 'A' && character <= 'F') {
                out |= static_cast<std::uint32_t>(character - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    static void append_utf8(std::string& out, std::uint32_t code_point) {
        if (code_point < 0x80) {
            out.push_back(static_cast<char>(code_point));
        } else if (code_point < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else if (code_point < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        }
    }

    [[nodiscard]] bool parse_literal(std::string_view literal, OrderNode& out) {
        if (text_.substr(position_, literal.size()) != literal) {
            return false;
        }
        position_ += literal.size();
        out = OrderNode{};
        return true;
    }

    /// Skip a value number token; `support::read_json` owns the numeric
    /// grammar and value conversion, so this pass only advances to the
    /// structural delimiter.
    [[nodiscard]] bool skip_number(OrderNode& out) {
        const std::size_t start = position_;
        while (position_ < text_.size()) {
            const char character = text_[position_];
            if (character == ',' || character == ']' || character == '}' || character == ' ' || character == '\t' ||
                    character == '\n' || character == '\r') {
                break;
            }
            ++position_;
        }
        if (position_ == start) {
            return false;
        }
        out = OrderNode{};
        return true;
    }

    void skip_whitespace() {
        while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\t' ||
                                                   text_[position_] == '\n' || text_[position_] == '\r')) {
            ++position_;
        }
    }

    std::string_view text_;
    std::size_t position_{0};
};

[[nodiscard]] inline const support::JsonValue* object_member(
        const support::JsonValue::object_t& object, std::string_view name) {
    const auto found = object.find(std::string{name});
    return found == object.end() ? nullptr : &found->second;
}

[[nodiscard]] inline const OrderNode* order_member(const OrderNode& node, std::string_view name) {
    for (const auto& [key, child] : node.members) {
        if (key == name) {
            return &child;
        }
    }
    return nullptr;
}

/// ECMAScript `Number::toString` / `JSON.stringify` number text. The shared
/// `support::write_json` deliberately reproduces the old Glaze formatter, whose
/// notation thresholds and `E` exponent differ from JavaScript, so the owner
/// formats its own numbers for the model text. The exact differences, measured
/// against Node's `JSON.stringify` for the values a `structuredContent` member
/// can carry:
///
/// | value                 | `JSON.stringify` | `support::write_json` |
/// |-----------------------|------------------|-----------------------|
/// | `1e20`                | `100000000000000000000` | `1E20`          |
/// | `1e21`                | `1e+21`          | `1E21`               |
/// | `1e-7`                | `1e-7`           | `1E-7`               |
/// | `0.000001`            | `0.000001`       | `1E-6`               |
/// | `1.2345678901234567e30` | `1.2345678901234567e+30` | `1.2345678901234567E30` |
/// | `-0`                  | `0`              | `-0`                 |
///
/// The shared formatter stays as it is: it is the wire format of every other
/// JSON value in the project, and changing it would be a cross-cutting change
/// outside this slice. Only the model-visible `structuredContent` text uses
/// the JavaScript notation, because only that text is contractually the bytes
/// pi's client produces.
[[nodiscard]] inline std::string javascript_number_text(double value) {
    if (std::isnan(value) || std::isinf(value)) {
        return "null";
    }
    if (value == 0.0) {
        return "0";
    }
    char buffer[64];
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::scientific);
    if (converted.ec != std::errc{}) {
        return "null";
    }
    const char* const end = converted.ptr;
    const char* cursor = buffer;
    const bool negative = cursor < end && *cursor == '-';
    if (negative) {
        ++cursor;
    }
    std::string digits;
    if (cursor < end) {
        digits.push_back(*cursor++);
    }
    if (cursor < end && *cursor == '.') {
        ++cursor;
        while (cursor < end && *cursor >= '0' && *cursor <= '9') {
            digits.push_back(*cursor++);
        }
    }
    if (cursor < end && (*cursor == 'e' || *cursor == 'E')) {
        ++cursor;
    }
    bool exponent_negative = false;
    if (cursor < end && (*cursor == '+' || *cursor == '-')) {
        exponent_negative = *cursor == '-';
        ++cursor;
    }
    int exponent = 0;
    while (cursor < end && *cursor >= '0' && *cursor <= '9') {
        exponent = exponent * 10 + (*cursor - '0');
        ++cursor;
    }
    if (exponent_negative) {
        exponent = -exponent;
    }
    while (digits.size() > 1 && digits.back() == '0') {
        digits.pop_back();
    }

    const int k = static_cast<int>(digits.size());
    const int n = exponent + 1;
    std::string out;
    if (negative) {
        out += '-';
    }
    if (k <= n && n <= 21) {
        out += digits;
        out.append(static_cast<std::size_t>(n - k), '0');
    } else if (0 < n && n <= 21) {
        out.append(digits, 0, static_cast<std::size_t>(n));
        out += '.';
        out.append(digits, static_cast<std::size_t>(n), std::string::npos);
    } else if (-6 < n && n <= 0) {
        out += "0.";
        out.append(static_cast<std::size_t>(-n), '0');
        out += digits;
    } else {
        out += digits[0];
        if (k > 1) {
            out += '.';
            out.append(digits, 1, std::string::npos);
        }
        out += 'e';
        const int magnitude = n - 1;
        if (magnitude >= 0) {
            out += '+';
            out += std::to_string(magnitude);
        } else {
            out += '-';
            out += std::to_string(-magnitude);
        }
    }
    return out;
}

/// Append the pretty form of `value`, taking object and array order from
/// `order`. The transport's ordinary JsonValue supplies scalar values.
[[nodiscard]] inline support::ExpectedVoid append_ordered_pretty_json(
        const support::JsonValue& value, const OrderNode* order, std::string& out, std::size_t indentation) {
    if (const auto* object = value.get_if<support::JsonValue::object_t>()) {
        if (object->empty()) {
            out += "{}";
            return {};
        }
        out += "{\n";
        std::size_t emitted = 0;
        if (order != nullptr && order->kind == OrderNode::Kind::Object) {
            for (const auto& [key, child_order] : order->members) {
                const auto member = object->find(key);
                if (member == object->end()) {
                    continue;
                }
                out.append(indentation + 2, ' ');
                auto key_json = support::write_json(support::JsonValue{key});
                if (!key_json) {
                    return std::unexpected(key_json.error());
                }
                out.append(*key_json);
                out += ": ";
                if (auto appended = append_ordered_pretty_json(member->second, &child_order, out, indentation + 2);
                        !appended) {
                    return appended;
                }
                if (++emitted < object->size()) {
                    out += ",\n";
                } else {
                    out += '\n';
                }
            }
        } else {
            // No order skeleton for this object: preserve the pre-existing
            // sorted JsonValue fallback rather than inventing an order.
            for (auto current = object->begin(); current != object->end(); ++current) {
                out.append(indentation + 2, ' ');
                auto key_json = support::write_json(support::JsonValue{current->first});
                if (!key_json) {
                    return std::unexpected(key_json.error());
                }
                out.append(*key_json);
                out += ": ";
                if (auto appended = append_ordered_pretty_json(current->second, nullptr, out, indentation + 2);
                        !appended) {
                    return appended;
                }
                if (++emitted < object->size()) {
                    out += ",\n";
                } else {
                    out += '\n';
                }
            }
        }
        out.append(indentation, ' ');
        out += '}';
        return {};
    }
    if (const auto* array = value.get_if<support::JsonValue::array_t>()) {
        if (array->empty()) {
            out += "[]";
            return {};
        }
        out += "[\n";
        for (std::size_t index = 0; index < array->size(); ++index) {
            out.append(indentation + 2, ' ');
            const OrderNode* child_order = nullptr;
            if (order != nullptr && order->kind == OrderNode::Kind::Array && index < order->elements.size()) {
                child_order = &order->elements[index];
            }
            if (auto appended = append_ordered_pretty_json((*array)[index], child_order, out, indentation + 2);
                    !appended) {
                return appended;
            }
            out += index + 1 < array->size() ? ",\n" : "\n";
        }
        out.append(indentation, ' ');
        out += ']';
        return {};
    }
    if (const auto* number = value.get_if<double>()) {
        out.append(javascript_number_text(*number));
        return {};
    }
    auto scalar = support::write_json(value);
    if (!scalar) {
        return std::unexpected(scalar.error());
    }
    out.append(*scalar);
    return {};
}

/// Format the `structuredContent` member of the response to `id` from one raw
/// JSON-RPC line/body. Returns nullopt when the value or order skeleton is
/// absent; callers keep the existing JsonValue fallback in that case.
[[nodiscard]] inline std::optional<std::string> format_structured_content(std::string_view raw, int id) {
    auto parsed = support::read_json(raw);
    if (!parsed) {
        return std::nullopt;
    }
    OrderNode order_root;
    if (!OrderParser{raw}.parse(order_root)) {
        return std::nullopt;
    }
    const support::JsonValue* message = nullptr;
    const OrderNode* message_order = nullptr;
    if (const auto* object = parsed->get_if<support::JsonValue::object_t>()) {
        message = &*parsed;
        message_order = &order_root;
        (void)object;
    } else if (const auto* array = parsed->get_if<support::JsonValue::array_t>()) {
        for (std::size_t index = 0; index < array->size(); ++index) {
            const auto* candidate = (*array)[index].get_if<support::JsonValue::object_t>();
            if (candidate == nullptr) {
                continue;
            }
            const auto response_id = candidate->find("id");
            if (response_id == candidate->end() || !response_id->second.holds<double>() ||
                    static_cast<int>(response_id->second.get_number()) != id) {
                continue;
            }
            message = &(*array)[index];
            if (index < order_root.elements.size()) {
                message_order = &order_root.elements[index];
            }
            break;
        }
    }
    if (message == nullptr) {
        return std::nullopt;
    }
    const auto* message_object = message->get_if<support::JsonValue::object_t>();
    if (message_object == nullptr) {
        return std::nullopt;
    }
    const auto result = message_object->find("result");
    if (result == message_object->end()) {
        return std::nullopt;
    }
    const auto* result_object = result->second.get_if<support::JsonValue::object_t>();
    if (result_object == nullptr) {
        return std::nullopt;
    }
    const auto structured = result_object->find("structuredContent");
    if (structured == result_object->end()) {
        return std::nullopt;
    }
    const OrderNode* result_order = message_order != nullptr ? order_member(*message_order, "result") : nullptr;
    const OrderNode* structured_order =
            result_order != nullptr ? order_member(*result_order, "structuredContent") : nullptr;
    if (structured_order == nullptr) {
        return std::nullopt;
    }
    std::string out;
    if (auto appended = append_ordered_pretty_json(structured->second, structured_order, out, 0); !appended) {
        return std::nullopt;
    }
    return out;
}

} // namespace cch::mcp::detail
