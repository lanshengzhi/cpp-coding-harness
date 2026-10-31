#pragma once

#include <cch/durable/Records.hpp>
#include <cch/durable/Session.hpp>
#include <cch/support/JsonValue.hpp>

#include <cstddef>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace cch::tests::durable {

/// The frozen expectation of one `audit-1063` scenario, read from
/// `fixtures/pi-v1.3.0/audit-1063/session-transactions.json`.
///
/// The test asserts against the captured frozen artifact rather than against
/// values invented in C++: every expectation below is the artifact's own
/// `expected` object for the named scenario, so a change in the frozen
/// behavior changes the expectations the C++ side must meet.
class FrozenScenario {
public:
    /// Load the scenario with this exact frozen name from the default
    /// `audit-1063` artifact.
    [[nodiscard]] static std::optional<FrozenScenario> load(std::string_view scenario_name);

    /// Load one scenario from the named repository-relative artifact.
    [[nodiscard]] static std::optional<FrozenScenario> load(
            std::string_view artifact_path, std::string_view scenario_name);

    /// The frozen `inputs` object of the scenario.
    [[nodiscard]] const support::JsonValue& inputs() const noexcept { return inputs_; }

    /// The frozen `expected` object of the scenario.
    [[nodiscard]] const support::JsonValue& expected() const noexcept { return expected_; }

    /// The frozen `expected` member with this name.
    [[nodiscard]] const support::JsonValue* member(std::string_view name) const;

    /// The frozen `expected` member with this name, when it is a string.
    [[nodiscard]] std::optional<std::string> string_member(std::string_view name) const;

    /// The frozen `expected` member with this name, when it is a number.
    [[nodiscard]] std::optional<double> number_member(std::string_view name) const;

    /// The frozen `expected` member with this name, when it is an object.
    [[nodiscard]] const support::JsonValue::object_t* object_member(std::string_view name) const;

    /// The frozen `expected` member with this name, when it is an array.
    [[nodiscard]] const support::JsonValue::array_t* array_member(std::string_view name) const;

private:
    explicit FrozenScenario(support::JsonValue inputs, support::JsonValue expected)
        : inputs_(std::move(inputs)), expected_(std::move(expected)) {}

    support::JsonValue inputs_;
    support::JsonValue expected_;
};

/// The frozen artifact of the `audit-1063` family, parsed once per process.
[[nodiscard]] const support::JsonValue& frozen_artifact();

/// The frozen artifact at the repository-relative path, parsed once per
/// process.
[[nodiscard]] const support::JsonValue& frozen_artifact(std::string_view artifact_path);

/// Convert one immutable Chord revision into the passive JSON value, so it can
/// be compared with the frozen artifact's own values.
[[nodiscard]] support::JsonValue to_json(const chord::Json& value);

/// Compare one immutable Chord revision with a frozen JSON value.
[[nodiscard]] bool json_equals(const chord::Json& value, const support::JsonValue& expected);

/// Compare one immutable Chord revision with a frozen JSON object.
[[nodiscard]] bool json_equals(const chord::Json& value, const support::JsonValue::object_t& expected);

} // namespace cch::tests::durable
