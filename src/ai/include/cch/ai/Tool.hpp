#pragma once

#include <cch/support/JsonValue.hpp>

#include <map>
#include <optional>
#include <string>

namespace cch::ai {

/// pi `Tool.constrainedSampling` (`packages/ai/src/types.ts`): the provider
/// constrains the tool-input generation to one of these grammars instead of
/// free-form text. Today the only member is grammar-constrained sampling with
/// provider-keyed grammar variants (pi `{ type: "grammar", variants }`); a
/// provider key that is absent for the active path carries no constraint.
struct ConstrainedSampling {
    /// Provider key (`openai_lark` in pi) → grammar source.
    std::map<std::string, std::string> variants{};
};

struct Tool {
    std::string name{};
    std::string description{};
    cch::support::JsonValue parameters{};
    /// pi `Tool.constrainedSampling`; absent means unconstrained tool input.
    std::optional<ConstrainedSampling> constrained_sampling{};
};

/// pi `ToolReference` (`packages/ai/src/types.ts` at f07218c4, tag `v0.87.1`):
/// the name of a tool a transcript's `SystemMessage.toolsRemoved` stops
/// declaring. It carries no definition, only the identity.
struct ToolReference {
    std::string name{};
};

} // namespace cch::ai
