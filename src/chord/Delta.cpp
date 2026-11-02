#include <cch/chord/Delta.hpp>

#include "chord/DeltaInternal.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace cch::chord {

namespace {

constexpr std::size_t kMaxOverlapScan = 65'536;
constexpr std::size_t kOverlapProbe = 64;
constexpr std::size_t kMaxOverlapCandidates = 8;

[[nodiscard]] std::string path_to_string(const Path& path) {
    std::string result{"["};
    for (std::size_t index = 0; index < path.size(); ++index) {
        if (index != 0) result += ",";
        if (const auto* key = std::get_if<PathKey>(&path[index])) {
            result += '"';
            result += key->key;
            result += '"';
        } else if (const auto* position = std::get_if<PathIndex>(&path[index])) {
            result += std::to_string(position->index);
        }
    }
    result += "]";
    return result;
}

[[nodiscard]] support::Error path_error(const Path& path) {
    return support::make_error(support::ErrorCode::Validation, "unresolvable path: " + path_to_string(path));
}

[[nodiscard]] support::Error type_error(std::string message) {
    return support::make_error(support::ErrorCode::Validation, std::move(message));
}

/// JavaScript prints integral values below 1e21 in fixed notation and larger
/// magnitudes in exponential notation; the frozen error text uses `String()`.
constexpr double kFixedNumberLimit = 1e21;

[[nodiscard]] std::string frozen_string(double value) {
    if (value == 0) return "0";
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
    std::array<char, 400> buffer{};
    const auto written =
            std::trunc(value) == value && std::abs(value) < kFixedNumberLimit
                    ? std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed)
                    : std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return std::string{buffer.data(), written.ptr};
}

/// The frozen `String(value)` spelling used by the rejection vocabulary. Object
/// and array spellings follow JavaScript's `Array.prototype.toString`, which is
/// what a non-string verb or path segment is rendered through.
[[nodiscard]] std::string frozen_string(const Json& value) {
    if (const auto* text = value.as_string()) return *text;
    if (const auto* number = value.as_number()) return frozen_string(*number);
    if (const auto* flag = value.as_boolean()) return *flag ? "true" : "false";
    if (value.is_null()) return "null";
    if (const auto* array = value.as_array()) {
        std::string result;
        for (std::size_t index = 0; index < array->size(); ++index) {
            if (index != 0) result += ',';
            if (!(*array)[index].is_null()) result += frozen_string((*array)[index]);
        }
        return result;
    }
    return "[object Object]";
}

/// A frozen index or count: any integral double at or above zero. A value above
/// `std::size_t` saturates, which keeps the frozen applier's clamping of splice
/// and truncate bounds.
[[nodiscard]] std::optional<std::size_t> decoded_size(const Json& value) {
    const auto* number = value.as_number();
    if (number == nullptr || !std::isfinite(*number) || std::trunc(*number) != *number || *number < 0) {
        return std::nullopt;
    }
    if (*number >= static_cast<double>(std::numeric_limits<std::size_t>::max())) {
        return std::numeric_limits<std::size_t>::max();
    }
    return static_cast<std::size_t>(*number);
}

/// Decode a frozen path argument. `allow_empty` is true only for the two root
/// operations (`p` and `m`), matching frozen `assertPathArg`.
[[nodiscard]] support::Expected<Path> decode_path(const Json& value, bool allow_empty) {
    const auto* segments = value.as_array();
    if (segments == nullptr) return std::unexpected(type_error("path is not an array"));
    if (!allow_empty && segments->empty()) return std::unexpected(type_error("path is empty"));
    Path path;
    path.reserve(segments->size());
    for (const Json& segment : *segments) {
        if (const auto* key = segment.as_string()) {
            if (detail::is_reserved_segment(*key)) {
                return std::unexpected(type_error(std::format("unsafe path segment: {}", *key)));
            }
            path.push_back(path_key(*key));
            continue;
        }
        const auto index = decoded_size(segment);
        if (!index.has_value())
            return std::unexpected(type_error(std::format("unsafe path segment: {}", frozen_string(segment))));
        path.push_back(path_index(*index));
    }
    return path;
}

[[nodiscard]] bool is_high_surrogate(char16_t unit) { return unit >= 0xD800 && unit <= 0xDBFF; }

[[nodiscard]] bool is_low_surrogate(char16_t unit) { return unit >= 0xDC00 && unit <= 0xDFFF; }

[[nodiscard]] std::optional<std::u16string> decode_utf8(std::string_view text) {
    std::u16string result;
    result.reserve(text.size());
    for (std::size_t offset = 0; offset < text.size();) {
        const auto lead = static_cast<unsigned char>(text[offset]);
        char32_t code_point = 0;
        std::size_t length = 0;
        if (lead < 0x80) {
            code_point = lead;
            length = 1;
        } else if ((lead & 0xE0) == 0xC0) {
            code_point = lead & 0x1F;
            length = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            code_point = lead & 0x0F;
            length = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            code_point = lead & 0x07;
            length = 4;
        } else {
            return std::nullopt;
        }
        if (offset + length > text.size()) return std::nullopt;
        for (std::size_t continuation = 1; continuation < length; ++continuation) {
            const auto byte = static_cast<unsigned char>(text[offset + continuation]);
            if ((byte & 0xC0) != 0x80) return std::nullopt;
            code_point = (code_point << 6) | (byte & 0x3F);
        }
        if ((length == 2 && code_point < 0x80) || (length == 3 && code_point < 0x800) ||
                (length == 4 && code_point < 0x10000) || code_point > 0x10FFFF ||
                (code_point >= 0xD800 && code_point <= 0xDFFF)) {
            return std::nullopt;
        }
        if (code_point <= 0xFFFF) {
            result.push_back(static_cast<char16_t>(code_point));
        } else {
            const char32_t adjusted = code_point - 0x10000;
            result.push_back(static_cast<char16_t>(0xD800 + (adjusted >> 10)));
            result.push_back(static_cast<char16_t>(0xDC00 + (adjusted & 0x3FF)));
        }
        offset += length;
    }
    return result;
}

[[nodiscard]] std::optional<std::string> encode_utf8(std::u16string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        char32_t code_point = text[index];
        if (is_high_surrogate(text[index])) {
            if (index + 1 >= text.size() || !is_low_surrogate(text[index + 1])) return std::nullopt;
            code_point = 0x10000 + ((static_cast<char32_t>(text[index]) - 0xD800) << 10) +
                         (static_cast<char32_t>(text[index + 1]) - 0xDC00);
            ++index;
        } else if (is_low_surrogate(text[index])) {
            return std::nullopt;
        }
        if (code_point < 0x80) {
            result.push_back(static_cast<char>(code_point));
        } else if (code_point < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
            result.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else if (code_point < 0x10000) {
            result.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
            result.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
            result.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        }
    }
    return result;
}

[[nodiscard]] std::size_t overlap_units(
        std::u16string_view a, std::u16string_view b, std::size_t scan, std::size_t probe, std::size_t max_candidates) {
    if (a.empty() || b.empty() || scan == 0) return 0;
    const auto tail = a.size() > scan ? a.substr(a.size() - scan) : a;
    for (const std::size_t head_length : {std::min(probe, b.size()), std::size_t{1}}) {
        if (head_length == 0) continue;
        const auto head = b.substr(0, head_length);
        std::size_t tried = 0;
        for (std::size_t position = tail.find(head); position != std::u16string_view::npos;
                position = tail.find(head, position + 1)) {
            if (++tried > max_candidates) break;
            const std::size_t length = tail.size() - position;
            if (length <= b.size() && tail.substr(position) == b.substr(0, length)) return length;
        }
        if (head_length == 1) break;
    }
    return 0;
}

[[nodiscard]] support::Expected<std::string> truncate_front_utf16(std::string_view text, std::size_t count) {
    if (count == 0) return std::string{text};
    const auto units = decode_utf8(text);
    if (!units) return std::unexpected(type_error("value contains invalid UTF-8"));
    if (count >= units->size()) return std::string{};
    if (is_high_surrogate((*units)[count - 1]) && is_low_surrogate((*units)[count])) {
        return std::unexpected(type_error("truncate splits a surrogate pair"));
    }
    auto encoded = encode_utf8(std::u16string_view{units->data(), units->size()}.substr(count));
    if (!encoded) return std::unexpected(type_error("value contains invalid UTF-16"));
    return std::move(*encoded);
}

} // namespace

namespace detail {

/// Append the ops that turn `before` into `after`. Strings use append/truncate
/// when a suffix/prefix run exists; otherwise the whole string is replaced.
void emit_string_change(const std::string& before, const std::string& after, const Path& path, OpBatch& operations) {
    if (after.starts_with(before)) {
        operations.push_back(AppendOp{.path = path, .text = after.substr(before.size())});
        return;
    }
    const auto before_units = decode_utf8(before);
    const auto after_units = decode_utf8(after);
    if (!before_units || !after_units) {
        operations.push_back(SetOp{.path = path, .value = Json{after}});
        return;
    }
    const std::size_t shared =
            overlap_units(*before_units, *after_units, kMaxOverlapScan, kOverlapProbe, kMaxOverlapCandidates);
    if (shared == 0) {
        operations.push_back(SetOp{.path = path, .value = Json{after}});
        return;
    }
    const std::size_t boundary = before_units->size() - shared;
    // A UTF-16 boundary can split a surrogate pair. Frozen JS strings can hold
    // that lone surrogate, but a UTF-8 C++ value cannot, so replace instead.
    if (boundary != 0 && boundary < before_units->size() && is_high_surrogate((*before_units)[boundary - 1]) &&
            is_low_surrogate((*before_units)[boundary])) {
        operations.push_back(SetOp{.path = path, .value = Json{after}});
        return;
    }
    const auto suffix = encode_utf8(std::u16string_view{after_units->data(), after_units->size()}.substr(shared));
    if (!suffix) {
        operations.push_back(SetOp{.path = path, .value = Json{after}});
        return;
    }
    operations.push_back(TruncateOp{.path = path, .count = boundary});
    if (!suffix->empty()) operations.push_back(AppendOp{.path = path, .text = std::move(*suffix)});
}

bool is_reserved_segment(std::string_view key) {
    return key == "__proto__" || key == "constructor" || key == "prototype";
}

bool path_is_prefix(const Path& prefix, const Path& path) {
    if (prefix.size() > path.size()) return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        const auto* prefix_key = std::get_if<PathKey>(&prefix[index]);
        const auto* path_key_value = std::get_if<PathKey>(&path[index]);
        if (prefix_key != nullptr && path_key_value != nullptr) {
            if (prefix_key->key != path_key_value->key) return false;
            continue;
        }
        const auto* prefix_index = std::get_if<PathIndex>(&prefix[index]);
        const auto* path_index_value = std::get_if<PathIndex>(&path[index]);
        if (prefix_index == nullptr || path_index_value == nullptr) return false;
        if (prefix_index->index != path_index_value->index) return false;
    }
    return true;
}

support::ExpectedVoid validate_safe_path(const Path& path) {
    for (const auto& segment : path) {
        const auto* key = std::get_if<PathKey>(&segment);
        if (key != nullptr && is_reserved_segment(key->key)) {
            return std::unexpected(type_error("unsafe path segment: " + key->key));
        }
    }
    return {};
}

support::ExpectedVoid validate_permutation(const std::vector<std::size_t>& permutation) {
    std::vector<bool> seen(permutation.size(), false);
    for (const std::size_t index : permutation) {
        if (index >= permutation.size() || seen[index]) {
            return std::unexpected(type_error("m permutation is not a bijection"));
        }
        seen[index] = true;
    }
    return {};
}

bool path_is_identity_permutation(const std::vector<std::size_t>& permutation) {
    for (std::size_t index = 0; index < permutation.size(); ++index) {
        if (permutation[index] != index) return false;
    }
    return true;
}

std::optional<Json> try_get_path(const Json& root, const Path& path) {
    const Json* current = &root;
    for (const auto& segment : path) {
        if (const auto* key = std::get_if<PathKey>(&segment)) {
            const auto* object = current->as_object();
            if (object == nullptr) return std::nullopt;
            const auto found = object->find(key->key);
            if (found == object->end()) return std::nullopt;
            current = &found->second;
            continue;
        }
        const auto* position = std::get_if<PathIndex>(&segment);
        if (position == nullptr) return std::nullopt;
        const auto* array = current->as_array();
        if (array == nullptr || position->index >= array->size()) return std::nullopt;
        current = &(*array)[position->index];
    }
    return *current;
}

support::Expected<Json> get_path(const Json& root, const Path& path) {
    auto value = try_get_path(root, path);
    if (!value) return std::unexpected(path_error(path));
    return std::move(*value);
}

support::Expected<Json> set_path(const Json& root, const Path& path, Json value, std::size_t offset) {
    if (offset >= path.size()) return value;
    const auto& segment = path[offset];
    const bool last = offset + 1 == path.size();
    if (const auto* key = std::get_if<PathKey>(&segment)) {
        const auto* object = root.as_object();
        if (object == nullptr) return std::unexpected(path_error(path));
        Json::Object updated = *object;
        if (last) {
            updated.insert_or_assign(key->key, std::move(value));
            return Json{std::move(updated)};
        }
        const auto found = updated.find(key->key);
        if (found == updated.end()) return std::unexpected(path_error(path));
        auto child = set_path(found->second, path, std::move(value), offset + 1);
        if (!child) return std::unexpected(std::move(child.error()));
        found->second = std::move(*child);
        return Json{std::move(updated)};
    }
    const auto* position = std::get_if<PathIndex>(&segment);
    if (position == nullptr) return std::unexpected(path_error(path));
    const auto* array = root.as_array();
    if (array == nullptr) return std::unexpected(path_error(path));
    if (position->index > array->size()) {
        return std::unexpected(type_error("unsafe path segment: " + std::to_string(position->index)));
    }
    Json::Array updated = *array;
    if (last) {
        if (position->index == updated.size())
            updated.push_back(std::move(value));
        else
            updated[position->index] = std::move(value);
        return Json{std::move(updated)};
    }
    if (position->index == updated.size()) return std::unexpected(path_error(path));
    auto child = set_path(updated[position->index], path, std::move(value), offset + 1);
    if (!child) return std::unexpected(std::move(child.error()));
    updated[position->index] = std::move(*child);
    return Json{std::move(updated)};
}

support::Expected<Json> erase_path(const Json& root, const Path& path, bool& changed, std::size_t offset) {
    if (offset >= path.size()) return std::unexpected(path_error(path));
    const auto& segment = path[offset];
    const bool last = offset + 1 == path.size();
    if (const auto* key = std::get_if<PathKey>(&segment)) {
        const auto* object = root.as_object();
        if (object == nullptr) return std::unexpected(path_error(path));
        Json::Object updated = *object;
        if (last) {
            const auto found = updated.find(key->key);
            if (found == updated.end()) {
                changed = false;
                return root;
            }
            updated.erase(found);
            changed = true;
            return Json{std::move(updated)};
        }
        const auto found = updated.find(key->key);
        if (found == updated.end()) return std::unexpected(path_error(path));
        bool child_changed = false;
        auto child = erase_path(found->second, path, child_changed, offset + 1);
        if (!child) return std::unexpected(std::move(child.error()));
        found->second = std::move(*child);
        changed = child_changed;
        return Json{std::move(updated)};
    }
    const auto* position = std::get_if<PathIndex>(&segment);
    if (position == nullptr) return std::unexpected(path_error(path));
    const auto* array = root.as_array();
    if (array == nullptr) return std::unexpected(path_error(path));
    if (position->index >= array->size()) return std::unexpected(path_error(path));
    Json::Array updated = *array;
    if (last) {
        updated.erase(updated.begin() + static_cast<std::ptrdiff_t>(position->index));
        changed = true;
        return Json{std::move(updated)};
    }
    bool child_changed = false;
    auto child = erase_path(updated[position->index], path, child_changed, offset + 1);
    if (!child) return std::unexpected(std::move(child.error()));
    updated[position->index] = std::move(*child);
    changed = child_changed;
    return Json{std::move(updated)};
}

support::Expected<Json> append_string_path(const Json& root, const Path& path, std::string_view text) {
    auto current = get_path(root, path);
    if (!current) return std::unexpected(std::move(current.error()));
    const auto* string = current->as_string();
    if (string == nullptr) return std::unexpected(path_error(path));
    std::string appended = *string;
    appended += text;
    return set_path(root, path, Json{std::move(appended)});
}

support::Expected<Json> truncate_string_path(const Json& root, const Path& path, std::size_t count) {
    auto current = get_path(root, path);
    if (!current) return std::unexpected(std::move(current.error()));
    const auto* string = current->as_string();
    if (string == nullptr) return std::unexpected(path_error(path));
    auto truncated = truncate_front_utf16(*string, count);
    if (!truncated) return std::unexpected(std::move(truncated.error()));
    return set_path(root, path, Json{std::move(*truncated)});
}

support::Expected<Json> splice_path(const Json& root,
        const Path& path,
        std::size_t index,
        std::size_t remove,
        const std::vector<Json>& items,
        std::size_t& effective_index,
        std::size_t& effective_remove) {
    auto target = get_path(root, path);
    if (!target) return std::unexpected(std::move(target.error()));
    const auto* array = target->as_array();
    if (array == nullptr) return std::unexpected(path_error(path));
    effective_index = std::min(index, array->size());
    effective_remove = std::min(remove, array->size() - effective_index);
    Json::Array updated = *array;
    updated.erase(updated.begin() + static_cast<std::ptrdiff_t>(effective_index),
            updated.begin() + static_cast<std::ptrdiff_t>(effective_index + effective_remove));
    updated.insert(updated.begin() + static_cast<std::ptrdiff_t>(effective_index), items.begin(), items.end());
    if (path.empty()) return Json{std::move(updated)};
    return set_path(root, path, Json{std::move(updated)});
}

support::Expected<Json> move_path(const Json& root, const Path& path, const std::vector<std::size_t>& permutation) {
    auto target = get_path(root, path);
    if (!target) return std::unexpected(std::move(target.error()));
    const auto* array = target->as_array();
    if (array == nullptr) return std::unexpected(path_error(path));
    if (array->size() != permutation.size()) return std::unexpected(path_error(path));
    Json::Array updated;
    updated.reserve(array->size());
    for (const std::size_t index : permutation) {
        if (index >= array->size()) return std::unexpected(type_error("m permutation is not a bijection"));
        updated.push_back((*array)[index]);
    }
    if (path.empty()) return Json{std::move(updated)};
    return set_path(root, path, Json{std::move(updated)});
}

} // namespace detail

namespace {

/// The one sequential applier behind both `apply` entry points. It keeps the
/// caller's value and the adopted root apart: a root replacement is returned to
/// the caller, never written into the caller's value.
class Applier {
public:
    explicit Applier(Json& target) : target_(target), root_(target) {}

    [[nodiscard]] support::ExpectedVoid step(const Op& op) {
        if (auto valid = validate_op(op); !valid) return valid;
        auto next = apply_immutable(root_, OpBatch{op});
        if (!next) return std::unexpected(std::move(next.error()));
        root_ = std::move(*next);
        if (is_replace(op)) {
            root_replaced_ = true;
            return {};
        }
        if (!root_replaced_) target_ = root_;
        return {};
    }

    [[nodiscard]] support::Expected<Json> finish() { return root_; }

private:
    /// Borrowed from the caller; the applier never outlives one apply call.
    Json& target_;
    Json root_;
    bool root_replaced_{false};
};

} // namespace

bool is_replace(const Op& op) { return std::holds_alternative<ReplaceOp>(op); }

bool is_base(const OpBatch& ops) { return !ops.empty() && is_replace(ops.front()); }

support::ExpectedVoid validate_op(const Op& op) {
    return std::visit(
            [](const auto& value) -> support::ExpectedVoid {
                using Operation = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Operation, ReplaceOp>) {
                    return {};
                } else if constexpr (std::is_same_v<Operation, SpliceOp>) {
                    return detail::validate_safe_path(value.path);
                } else if constexpr (std::is_same_v<Operation, MoveOp>) {
                    if (auto safe = detail::validate_safe_path(value.path); !safe) return safe;
                    return detail::validate_permutation(value.permutation);
                } else {
                    if (value.path.empty()) return std::unexpected(type_error("path is empty"));
                    return detail::validate_safe_path(value.path);
                }
            },
            op);
}

support::ExpectedVoid validate_batch(const OpBatch& ops) {
    for (const auto& op : ops) {
        if (auto valid = validate_op(op); !valid) return valid;
    }
    return {};
}

support::Expected<Op> decode_op(const Json& tuple) {
    const auto* fields = tuple.as_array();
    if (fields == nullptr || fields->empty()) return std::unexpected(type_error("op is not a tuple"));
    const auto* verb = (*fields)[0].as_string();
    if (verb == nullptr)
        return std::unexpected(type_error(std::format("unknown op verb: {}", frozen_string((*fields)[0]))));
    const std::size_t arity = fields->size();
    if (*verb == "r") {
        if (arity != 2) return std::unexpected(type_error("r arity"));
        return Op{ReplaceOp{.value = (*fields)[1]}};
    }
    if (*verb == "s") {
        if (arity != 3) return std::unexpected(type_error("s arity"));
        auto path = decode_path((*fields)[1], false);
        if (!path) return std::unexpected(std::move(path.error()));
        return Op{SetOp{.path = std::move(*path), .value = (*fields)[2]}};
    }
    if (*verb == "d") {
        if (arity != 2) return std::unexpected(type_error("d arity"));
        auto path = decode_path((*fields)[1], false);
        if (!path) return std::unexpected(std::move(path.error()));
        return Op{DeleteOp{.path = std::move(*path)}};
    }
    if (*verb == "a") {
        if (arity != 3 || (*fields)[2].as_string() == nullptr) return std::unexpected(type_error("a shape"));
        auto path = decode_path((*fields)[1], false);
        if (!path) return std::unexpected(std::move(path.error()));
        return Op{AppendOp{.path = std::move(*path), .text = *(*fields)[2].as_string()}};
    }
    if (*verb == "t") {
        if (arity != 3) return std::unexpected(type_error("t shape"));
        const auto count = decoded_size((*fields)[2]);
        if (!count.has_value()) return std::unexpected(type_error("t shape"));
        auto path = decode_path((*fields)[1], false);
        if (!path) return std::unexpected(std::move(path.error()));
        return Op{TruncateOp{.path = std::move(*path), .count = *count}};
    }
    if (*verb == "p") {
        if (arity != 5) return std::unexpected(type_error("p arity"));
        auto path = decode_path((*fields)[1], true);
        if (!path) return std::unexpected(std::move(path.error()));
        const auto index = decoded_size((*fields)[2]);
        if (!index.has_value()) return std::unexpected(type_error("p index"));
        const auto remove = decoded_size((*fields)[3]);
        if (!remove.has_value()) return std::unexpected(type_error("p remove"));
        const auto* encoded_items = (*fields)[4].as_array();
        if (encoded_items == nullptr) return std::unexpected(type_error("p items"));
        std::vector<Json> items{encoded_items->begin(), encoded_items->end()};
        return Op{SpliceOp{
                .path = std::move(*path),
                .index = *index,
                .remove = *remove,
                .items = std::move(items),
        }};
    }
    if (*verb == "m") {
        if (arity != 3) return std::unexpected(type_error("m arity"));
        auto path = decode_path((*fields)[1], true);
        if (!path) return std::unexpected(std::move(path.error()));
        const auto* encoded = (*fields)[2].as_array();
        if (encoded == nullptr) return std::unexpected(type_error("m permutation is not an array"));
        std::vector<std::size_t> permutation;
        permutation.reserve(encoded->size());
        for (const Json& entry : *encoded) {
            const auto index = decoded_size(entry);
            if (!index.has_value()) {
                return std::unexpected(type_error("m permutation is not a bijection"));
            }
            permutation.push_back(*index);
        }
        if (auto valid = detail::validate_permutation(permutation); !valid) {
            return std::unexpected(std::move(valid.error()));
        }
        return Op{MoveOp{.path = std::move(*path), .permutation = std::move(permutation)}};
    }
    return std::unexpected(type_error(std::format("unknown op verb: {}", *verb)));
}

support::Expected<OpBatch> decode_ops(const Json& tuples) {
    const auto* entries = tuples.as_array();
    if (entries == nullptr) return std::unexpected(type_error("op batch is not an array"));
    OpBatch ops;
    ops.reserve(entries->size());
    for (const Json& tuple : *entries) {
        auto op = decode_op(tuple);
        if (!op) return std::unexpected(std::move(op.error()));
        ops.push_back(std::move(*op));
    }
    return ops;
}

support::Expected<Json> apply_immutable(const Json& target, const OpBatch& ops) {
    if (auto valid = validate_batch(ops); !valid) return std::unexpected(std::move(valid.error()));
    Json root = target;
    for (const auto& op : ops) {
        auto result = std::visit(
                [&root](const auto& value) -> support::Expected<Json> {
                    using Operation = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Operation, ReplaceOp>) {
                        return value.value;
                    } else if constexpr (std::is_same_v<Operation, SetOp>) {
                        return detail::set_path(root, value.path, value.value);
                    } else if constexpr (std::is_same_v<Operation, DeleteOp>) {
                        bool changed = false;
                        return detail::erase_path(root, value.path, changed);
                    } else if constexpr (std::is_same_v<Operation, AppendOp>) {
                        return detail::append_string_path(root, value.path, value.text);
                    } else if constexpr (std::is_same_v<Operation, TruncateOp>) {
                        return detail::truncate_string_path(root, value.path, value.count);
                    } else if constexpr (std::is_same_v<Operation, SpliceOp>) {
                        std::size_t effective_index = 0;
                        std::size_t effective_remove = 0;
                        return detail::splice_path(root,
                                value.path,
                                value.index,
                                value.remove,
                                value.items,
                                effective_index,
                                effective_remove);
                    } else {
                        return detail::move_path(root, value.path, value.permutation);
                    }
                },
                op);
        if (!result) return std::unexpected(std::move(result.error()));
        root = std::move(*result);
    }
    return root;
}

support::Expected<Json> apply_immutable_batches(const Json& target, const std::vector<OpBatch>& batches) {
    Json root = target;
    for (const auto& batch : batches) {
        auto result = apply_immutable(root, batch);
        if (!result) return std::unexpected(std::move(result.error()));
        root = std::move(*result);
    }
    return root;
}

support::Expected<Json> apply(Json& target, const OpBatch& ops) {
    Applier applier{target};
    for (const auto& op : ops) {
        if (auto stepped = applier.step(op); !stepped) return std::unexpected(std::move(stepped.error()));
    }
    return applier.finish();
}

support::Expected<Json> apply(Json& target, const Json& decoded_tuples) {
    const auto* entries = decoded_tuples.as_array();
    if (entries == nullptr) return std::unexpected(type_error("op batch is not an array"));
    Applier applier{target};
    for (const Json& tuple : *entries) {
        auto decoded = decode_op(tuple);
        if (!decoded) return std::unexpected(std::move(decoded.error()));
        if (auto stepped = applier.step(*decoded); !stepped) {
            return std::unexpected(std::move(stepped.error()));
        }
    }
    return applier.finish();
}

std::size_t overlap(
        std::string_view a, std::string_view b, std::size_t scan, std::size_t probe, std::size_t max_candidates) {
    if (a.empty() || b.empty() || scan == 0) return 0;
    const auto a_units = decode_utf8(a);
    const auto b_units = decode_utf8(b);
    if (!a_units || !b_units) return 0;
    return overlap_units(*a_units, *b_units, scan, probe, max_candidates);
}

} // namespace cch::chord
