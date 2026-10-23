#include "durable/FrozenExpectation.hpp"

#include "support/Json.hpp"

#include <cstddef>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tests::durable {

namespace {

[[nodiscard]] std::string read_artifact_text(std::string_view artifact_path) {
    std::ifstream stream(std::string{CCH_SOURCE_DIR} + "/" + std::string{artifact_path});
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

} // namespace

const support::JsonValue& frozen_artifact() {
    return frozen_artifact("fixtures/pi-v1.3.0/audit-1063/session-transactions.json");
}

const support::JsonValue& frozen_artifact(std::string_view artifact_path) {
    static auto cache = std::map<std::string, support::JsonValue>{};
    const auto found = cache.find(std::string{artifact_path});
    if (found != cache.end()) {
        return found->second;
    }
    auto parsed = support::read_json(read_artifact_text(artifact_path));
    if (!parsed) {
        // A missing or malformed frozen artifact is a broken fixture, not a
        // test outcome: the expectations cannot be read at all.
        std::terminate();
    }
    return cache.emplace(std::string{artifact_path}, std::move(*parsed)).first->second;
}

std::optional<FrozenScenario> FrozenScenario::load(std::string_view scenario_name) {
    return load("fixtures/pi-v1.3.0/audit-1063/session-transactions.json", scenario_name);
}

std::optional<FrozenScenario> FrozenScenario::load(std::string_view artifact_path, std::string_view scenario_name) {
    const support::JsonValue::object_t* root = frozen_artifact(artifact_path).get_if<support::JsonValue::object_t>();
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
    for (const support::JsonValue& scenario : *scenarios) {
        const auto* object = scenario.get_if<support::JsonValue::object_t>();
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
        return FrozenScenario{expected->second};
    }
    return std::nullopt;
}

const support::JsonValue* FrozenScenario::member(std::string_view name) const {
    const auto* object = expected_.get_if<support::JsonValue::object_t>();
    if (object == nullptr) {
        return nullptr;
    }
    const auto found = object->find(std::string{name});
    return found == object->end() ? nullptr : &found->second;
}

std::optional<std::string> FrozenScenario::string_member(std::string_view name) const {
    const support::JsonValue* value = member(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto* text = value->get_if<std::string>();
    return text == nullptr ? std::nullopt : std::optional<std::string>{*text};
}

std::optional<double> FrozenScenario::number_member(std::string_view name) const {
    const support::JsonValue* value = member(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto* number = value->get_if<double>();
    return number == nullptr ? std::nullopt : std::optional<double>{*number};
}

const support::JsonValue::object_t* FrozenScenario::object_member(std::string_view name) const {
    const support::JsonValue* value = member(name);
    return value == nullptr ? nullptr : value->get_if<support::JsonValue::object_t>();
}

const support::JsonValue::array_t* FrozenScenario::array_member(std::string_view name) const {
    const support::JsonValue* value = member(name);
    return value == nullptr ? nullptr : value->get_if<support::JsonValue::array_t>();
}

support::JsonValue to_json(const chord::Json& value) { return value.to_support(); }

bool json_equals(const chord::Json& value, const support::JsonValue& expected) {
    // `support::JsonValue` has no equality operator, so the two values are
    // compared through their canonical JSON text. Object key order is not
    // part of a JSON value: `write_json` emits object members in sorted key
    // order, so the texts compare by value.
    auto actual_text = support::write_json(value.to_support());
    auto expected_text = support::write_json(expected);
    if (!actual_text || !expected_text) {
        return false;
    }
    return *actual_text == *expected_text;
}

bool json_equals(const chord::Json& value, const support::JsonValue::object_t& expected) {
    return json_equals(value, support::JsonValue{expected});
}

} // namespace cch::tests::durable
