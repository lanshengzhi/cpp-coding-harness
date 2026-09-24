#pragma once

#include <cch/support/JsonValue.hpp>

#include <string>

namespace cch::ai {

struct Tool {
    std::string name{};
    std::string description{};
    cch::support::JsonValue parameters{};
};

/// pi `ToolReference` (`packages/ai/src/types.ts` at f07218c4, tag `v0.87.1`):
/// the name of a tool a transcript's `SystemMessage.toolsRemoved` stops
/// declaring. It carries no definition, only the identity.
struct ToolReference {
    std::string name{};
};

} // namespace cch::ai
