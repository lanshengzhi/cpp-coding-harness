#pragma once

// Shared reader for the registered pi-v1.3.0 chord audit evidence families
// (#1202 receipt boundary). A chord comparison anchors to the frozen
// observation by loading the registered artifact and comparing its own
// `inputs` and `expected` fields, so a frozen change moves the C++ side's
// expectations with it. Every claim keeps its discriminating comparison: the
// helper only centralizes the read and the typed member access, never the
// scenario semantics.

#include <cch/chord/Json.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include "support/Json.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace cch::tests {

/// The frozen `inputs`/`expected` pair of one named scenario inside a
/// registered `fixtures/pi-v1.3.0/<family>/<artifact>` bundle. A missing or
/// malformed artifact, or an unknown scenario, reads as `std::nullopt`: the
/// expectations cannot be read at all, which is a broken fixture rather than
/// a test outcome.
class FrozenChordScenario {
public:
    /// Load the scenario with this exact frozen name from the named
    /// registered family and artifact.
    [[nodiscard]] static std::optional<FrozenChordScenario> load(
            std::string_view family, std::string_view artifact, std::string_view scenario_name) {
        const auto path = std::filesystem::path{CCH_SOURCE_DIR} / "fixtures" / "pi-v1.3.0" / family / artifact;
        std::ifstream input(path, std::ios::binary);
        std::ostringstream buffer;
        buffer << input.rdbuf();
        auto parsed = support::read_json(buffer.str());
        if (!parsed) {
            return std::nullopt;
        }
        const auto* root = parsed->get_if<support::JsonValue::object_t>();
        if (root == nullptr) {
            return std::nullopt;
        }
        const auto scenarios_entry = root->find("scenarios");
        if (scenarios_entry == root->end()) {
            return std::nullopt;
        }
        const auto* scenarios = scenarios_entry->second.get_if<support::JsonValue::array_t>();
        if (scenarios == nullptr) {
            return std::nullopt;
        }
        for (const support::JsonValue& candidate : *scenarios) {
            const auto* object = candidate.get_if<support::JsonValue::object_t>();
            if (object == nullptr) {
                continue;
            }
            const auto name = object->find("name");
            if (name == object->end() || !name->second.holds<std::string>()) {
                continue;
            }
            if (name->second.get<std::string>() != scenario_name) {
                continue;
            }
            const auto expected = object->find("expected");
            if (expected == object->end()) {
                return std::nullopt;
            }
            // A scenario without inputs is still a scenario: the capture
            // records them only where they discriminate.
            const auto inputs = object->find("inputs");
            return FrozenChordScenario{
                    inputs == object->end() ? support::JsonValue{support::JsonValue::object_t{}} : inputs->second,
                    expected->second};
        }
        return std::nullopt;
    }

    /// The frozen `inputs` object of the scenario.
    [[nodiscard]] const support::JsonValue& inputs() const noexcept { return inputs_; }

    /// The frozen `expected` object of the scenario.
    [[nodiscard]] const support::JsonValue& expected() const noexcept { return expected_; }

    /// The frozen `expected` member with this name.
    [[nodiscard]] const support::JsonValue* member(std::string_view name) const {
        const auto* object = expected_.get_if<support::JsonValue::object_t>();
        if (object == nullptr) {
            return nullptr;
        }
        const auto found = object->find(std::string{name});
        return found == object->end() ? nullptr : &found->second;
    }

    /// The frozen `expected` member with this name, when it is a string.
    [[nodiscard]] std::optional<std::string> string_member(std::string_view name) const {
        const support::JsonValue* value = member(name);
        if (value == nullptr) {
            return std::nullopt;
        }
        const auto* text = value->get_if<std::string>();
        return text == nullptr ? std::nullopt : std::optional<std::string>{*text};
    }

    /// The frozen `expected` member with this name, when it is a number.
    [[nodiscard]] std::optional<double> number_member(std::string_view name) const {
        const support::JsonValue* value = member(name);
        if (value == nullptr) {
            return std::nullopt;
        }
        const auto* number = value->get_if<double>();
        return number == nullptr ? std::nullopt : std::optional<double>{*number};
    }

    /// The frozen `expected` member with this name, when it is a boolean.
    [[nodiscard]] std::optional<bool> boolean_member(std::string_view name) const {
        const support::JsonValue* value = member(name);
        if (value == nullptr) {
            return std::nullopt;
        }
        const auto* flag = value->get_if<bool>();
        return flag == nullptr ? std::nullopt : std::optional<bool>{*flag};
    }

    /// The frozen `expected` member with this name, when it is an object.
    [[nodiscard]] const support::JsonValue::object_t* object_member(std::string_view name) const {
        const support::JsonValue* value = member(name);
        return value == nullptr ? nullptr : value->get_if<support::JsonValue::object_t>();
    }

    /// The frozen `expected` member with this name, when it is an array.
    [[nodiscard]] const support::JsonValue::array_t* array_member(std::string_view name) const {
        const support::JsonValue* value = member(name);
        return value == nullptr ? nullptr : value->get_if<support::JsonValue::array_t>();
    }

    /// The frozen `inputs` member with this name.
    [[nodiscard]] const support::JsonValue* inputs_member(std::string_view name) const {
        const auto* object = inputs_.get_if<support::JsonValue::object_t>();
        if (object == nullptr) {
            return nullptr;
        }
        const auto found = object->find(std::string{name});
        return found == object->end() ? nullptr : &found->second;
    }

    /// The frozen `inputs` member with this name, when it is a string.
    [[nodiscard]] std::optional<std::string> inputs_string(std::string_view name) const {
        const support::JsonValue* value = inputs_member(name);
        if (value == nullptr) {
            return std::nullopt;
        }
        const auto* text = value->get_if<std::string>();
        return text == nullptr ? std::nullopt : std::optional<std::string>{*text};
    }

    /// The frozen `inputs` member with this name, when it is a number.
    [[nodiscard]] std::optional<double> inputs_number(std::string_view name) const {
        const support::JsonValue* value = inputs_member(name);
        if (value == nullptr) {
            return std::nullopt;
        }
        const auto* number = value->get_if<double>();
        return number == nullptr ? std::nullopt : std::optional<double>{*number};
    }

private:
    FrozenChordScenario(support::JsonValue inputs, support::JsonValue expected)
        : inputs_(std::move(inputs)), expected_(std::move(expected)) {}

    support::JsonValue inputs_;
    support::JsonValue expected_;
};

/// Compare one immutable Chord revision with a frozen JSON value through the
/// canonical text form (object member order is not part of a JSON value), so
/// the C++ side is compared against the artifact's own bytes, not a
/// re-derived literal.
[[nodiscard]] inline bool chord_json_equals(const chord::Json& value, const support::JsonValue& expected) {
    auto actual_text = support::canonical_json(value.to_support());
    auto expected_text = support::canonical_json(expected);
    if (!actual_text || !expected_text) {
        return false;
    }
    return *actual_text == *expected_text;
}

} // namespace cch::tests
