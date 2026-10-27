#pragma once

#include <cch/support/Error.hpp>
#include <cch/support/JsonObject.hpp>
#include <cch/support/JsonValue.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cch::chord {

/// Immutable Chord JSON revision value. A copy shares container storage, and
/// every operation returns a new value, so an adopted revision or a prepared
/// base never changes. This is the C++ representation of the frozen Chord
/// `JsonValue` under its trusted immutable-transfer contract.
class Json {
public:
    using Array = std::vector<Json>;
    /// Members keep parse/insertion order (#1149), matching the frozen pi
    /// `pi.system` section order the durable prompt planner reads.
    using Object = support::JsonObject<Json>;
    using Storage = std::variant<std::nullptr_t,
            bool,
            double,
            std::string,
            std::shared_ptr<const Array>,
            std::shared_ptr<const Object>>;

    Json() noexcept = default;
    Json(std::nullptr_t) noexcept {}
    Json(bool value) : storage_(value) {}
    Json(double value) : storage_(value) {}
    Json(int value) : storage_(static_cast<double>(value)) {}
    Json(std::string value) : storage_(std::move(value)) {}
    Json(const char* value) : storage_(std::string(value)) {}
    Json(Array value);
    Json(Object value);

    [[nodiscard]] bool is_null() const noexcept;
    [[nodiscard]] const bool* as_boolean() const noexcept;
    [[nodiscard]] const double* as_number() const noexcept;
    [[nodiscard]] const std::string* as_string() const noexcept;
    [[nodiscard]] const Array* as_array() const noexcept;
    [[nodiscard]] const Object* as_object() const noexcept;

    /// Deep equality; object key order is not part of a JSON value.
    [[nodiscard]] bool operator==(const Json& other) const;

    /// Whether this value and `other` are the same immutable container. This
    /// is the copy-on-write ownership check, not deep equality: primitive
    /// values never share, and distinct equal containers do not.
    [[nodiscard]] bool shares_container_with(const Json& other) const noexcept;

    /// Convert a support value into the immutable Chord representation.
    /// Rejects a non-finite number, the one value support can represent that
    /// strict JSON cannot.
    [[nodiscard]] static support::Expected<Json> from_support(const support::JsonValue& value);

    /// Detached support representation of this value.
    [[nodiscard]] support::JsonValue to_support() const;

private:
    Storage storage_{nullptr};
};

} // namespace cch::chord
