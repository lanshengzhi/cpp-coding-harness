#pragma once

#include <cch/support/JsonValue.hpp>

#include <optional>
#include <string>

namespace cch::ai {

struct Tool {
    std::string name{};
    std::string description{};
    cch::support::JsonValue parameters{};
    /// pi `ToolDefinition.outputSchema`: the machine-readable result schema a
    /// tool declares. `std::nullopt` when the tool declares none, so every
    /// existing tool keeps its behavior unchanged.
    std::optional<cch::support::JsonValue> output_schema{};
};

/// pi `ToolReference` (`packages/ai/src/types.ts` at f07218c4, tag `v0.87.1`):
/// the name of a tool a transcript's `SystemMessage.toolsRemoved` stops
/// declaring. It carries no definition, only the identity.
struct ToolReference {
    std::string name{};
};

} // namespace cch::ai
