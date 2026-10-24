#include "StorageEncoding.hpp"

#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>
#include <variant>

namespace cch::durable {
namespace {

[[nodiscard]] support::Expected<chord::Json> decode_json(const support::JsonValue& value) {
    return chord::Json::from_support(value);
}

[[nodiscard]] support::Error corruption(std::string message) {
    return support::make_error(support::ErrorCode::Validation, std::move(message));
}

} // namespace

std::optional<std::int64_t> nonnegative_integer(const support::JsonValue& value) {
    const auto* number = value.get_if<double>();
    if (number == nullptr || !std::isfinite(*number) || *number < 0 ||
            *number > static_cast<double>(std::numeric_limits<std::int64_t>::max()) || std::trunc(*number) != *number) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(*number);
}

support::JsonValue number(std::int64_t value) { return support::JsonValue{static_cast<double>(value)}; }

support::JsonValue number(std::size_t value) { return support::JsonValue{static_cast<double>(value)}; }

support::JsonValue path_value(const chord::Path& path) {
    support::JsonValue::array_t result;
    result.reserve(path.size());
    for (const chord::PathSegment& segment : path) {
        if (const auto* key = std::get_if<chord::PathKey>(&segment)) {
            result.emplace_back(key->key);
        } else {
            result.emplace_back(number(std::get<chord::PathIndex>(segment).index));
        }
    }
    return support::JsonValue{std::move(result)};
}

support::JsonValue op_value(const chord::Op& op) {
    return std::visit(
            [](const auto& operation) -> support::JsonValue {
                using T = std::decay_t<decltype(operation)>;
                if constexpr (std::is_same_v<T, chord::ReplaceOp>) {
                    return support::JsonValue{
                            support::JsonValue::array_t{support::JsonValue{"r"}, operation.value.to_support()}};
                } else if constexpr (std::is_same_v<T, chord::SetOp>) {
                    return support::JsonValue{support::JsonValue::array_t{
                            support::JsonValue{"s"}, path_value(operation.path), operation.value.to_support()}};
                } else if constexpr (std::is_same_v<T, chord::DeleteOp>) {
                    return support::JsonValue{
                            support::JsonValue::array_t{support::JsonValue{"d"}, path_value(operation.path)}};
                } else if constexpr (std::is_same_v<T, chord::AppendOp>) {
                    return support::JsonValue{support::JsonValue::array_t{
                            support::JsonValue{"a"}, path_value(operation.path), support::JsonValue{operation.text}}};
                } else if constexpr (std::is_same_v<T, chord::TruncateOp>) {
                    return support::JsonValue{support::JsonValue::array_t{
                            support::JsonValue{"t"}, path_value(operation.path), number(operation.count)}};
                } else if constexpr (std::is_same_v<T, chord::SpliceOp>) {
                    support::JsonValue::array_t items;
                    items.reserve(operation.items.size());
                    for (const chord::Json& item : operation.items) {
                        items.push_back(item.to_support());
                    }
                    return support::JsonValue{support::JsonValue::array_t{support::JsonValue{"p"},
                            path_value(operation.path),
                            number(operation.index),
                            number(operation.remove),
                            support::JsonValue{std::move(items)}}};
                } else {
                    support::JsonValue::array_t permutation;
                    permutation.reserve(operation.permutation.size());
                    for (const std::size_t index : operation.permutation) {
                        permutation.push_back(number(index));
                    }
                    return support::JsonValue{support::JsonValue::array_t{support::JsonValue{"m"},
                            path_value(operation.path),
                            support::JsonValue{std::move(permutation)}}};
                }
            },
            op);
}

support::JsonValue ops_value(const chord::OpBatch& ops) {
    support::JsonValue::array_t encoded;
    encoded.reserve(ops.size());
    for (const chord::Op& op : ops) {
        encoded.push_back(op_value(op));
    }
    return support::JsonValue{std::move(encoded)};
}

support::Expected<chord::Path> decode_path(const support::JsonValue& value) {
    const auto* array = value.get_if<support::JsonValue::array_t>();
    if (array == nullptr) {
        return std::unexpected(corruption("Invalid path"));
    }
    chord::Path path;
    path.reserve(array->size());
    for (const support::JsonValue& segment : *array) {
        if (const auto* key = segment.get_if<std::string>()) {
            path.push_back(chord::path_key(*key));
            continue;
        }
        const auto index = nonnegative_integer(segment);
        if (!index.has_value() || static_cast<std::uint64_t>(*index) >
                                          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            return std::unexpected(corruption("Invalid path"));
        }
        path.push_back(chord::path_index(static_cast<std::size_t>(*index)));
    }
    return path;
}

support::Expected<chord::OpBatch> decode_ops(const support::JsonValue& value) {
    const auto* array = value.get_if<support::JsonValue::array_t>();
    if (array == nullptr) {
        return std::unexpected(corruption("Invalid document operations"));
    }
    chord::OpBatch ops;
    ops.reserve(array->size());
    for (const support::JsonValue& encoded : *array) {
        const auto* tuple = encoded.get_if<support::JsonValue::array_t>();
        if (tuple == nullptr || tuple->empty()) {
            return std::unexpected(corruption("Invalid document operation"));
        }
        const auto* verb = tuple->front().get_if<std::string>();
        if (verb == nullptr) {
            return std::unexpected(corruption("Invalid document operation"));
        }
        if (*verb == "r" && tuple->size() == 2) {
            auto replacement = decode_json((*tuple)[1]);
            if (!replacement) {
                return std::unexpected(std::move(replacement.error()));
            }
            ops.push_back(chord::ReplaceOp{.value = std::move(*replacement)});
        } else if (*verb == "s" && tuple->size() == 3) {
            auto path = decode_path((*tuple)[1]);
            auto item = decode_json((*tuple)[2]);
            if (!path || !item) {
                return std::unexpected(corruption("Invalid document operation"));
            }
            ops.push_back(chord::SetOp{.path = std::move(*path), .value = std::move(*item)});
        } else if (*verb == "d" && tuple->size() == 2) {
            auto path = decode_path((*tuple)[1]);
            if (!path) {
                return std::unexpected(std::move(path.error()));
            }
            ops.push_back(chord::DeleteOp{.path = std::move(*path)});
        } else if (*verb == "a" && tuple->size() == 3) {
            auto path = decode_path((*tuple)[1]);
            const auto* text = (*tuple)[2].get_if<std::string>();
            if (!path || text == nullptr) {
                return std::unexpected(corruption("Invalid document operation"));
            }
            ops.push_back(chord::AppendOp{.path = std::move(*path), .text = *text});
        } else if (*verb == "t" && tuple->size() == 3) {
            auto path = decode_path((*tuple)[1]);
            const auto count = nonnegative_integer((*tuple)[2]);
            if (!path || !count ||
                    static_cast<std::uint64_t>(*count) >
                            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                return std::unexpected(corruption("Invalid document operation"));
            }
            ops.push_back(chord::TruncateOp{.path = std::move(*path), .count = static_cast<std::size_t>(*count)});
        } else if (*verb == "p" && tuple->size() == 5) {
            auto path = decode_path((*tuple)[1]);
            const auto index = nonnegative_integer((*tuple)[2]);
            const auto remove = nonnegative_integer((*tuple)[3]);
            const auto* items = (*tuple)[4].get_if<support::JsonValue::array_t>();
            if (!path || !index || !remove || items == nullptr) {
                return std::unexpected(corruption("Invalid document operation"));
            }
            std::vector<chord::Json> decoded_items;
            decoded_items.reserve(items->size());
            for (const support::JsonValue& item : *items) {
                auto decoded = decode_json(item);
                if (!decoded) {
                    return std::unexpected(std::move(decoded.error()));
                }
                decoded_items.push_back(std::move(*decoded));
            }
            ops.push_back(chord::SpliceOp{
                    .path = std::move(*path),
                    .index = static_cast<std::size_t>(*index),
                    .remove = static_cast<std::size_t>(*remove),
                    .items = std::move(decoded_items),
            });
        } else if (*verb == "m" && tuple->size() == 3) {
            auto path = decode_path((*tuple)[1]);
            const auto* items = (*tuple)[2].get_if<support::JsonValue::array_t>();
            if (!path || items == nullptr) {
                return std::unexpected(corruption("Invalid document operation"));
            }
            std::vector<std::size_t> permutation;
            permutation.reserve(items->size());
            for (const support::JsonValue& item : *items) {
                const auto index = nonnegative_integer(item);
                if (!index.has_value() || static_cast<std::uint64_t>(*index) >
                                                  static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                    return std::unexpected(corruption("Invalid document operation"));
                }
                permutation.push_back(static_cast<std::size_t>(*index));
            }
            ops.push_back(chord::MoveOp{
                    .path = std::move(*path),
                    .permutation = std::move(permutation),
            });
        } else {
            return std::unexpected(corruption("Unknown document operation verb"));
        }
    }
    if (auto valid = chord::validate_batch(ops); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    return ops;
}

} // namespace cch::durable
