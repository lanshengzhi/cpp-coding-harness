#pragma once

#include <cch/coding_agent/PromptTemplate.hpp>
#include <cch/coding_agent/Skill.hpp>

#include <string>
#include <vector>

namespace cch::coding_agent::prompt {

/// Expand one user prompt through pi's expansion chain (the deleted
/// `PromptProcessor`'s single behavior, ADR 0036 G4; pi `agent-session.ts`
/// `prompt()`): the skill command expands first (reading the skill file at
/// invocation time), then prompt templates. Disabled when
/// `expand_prompt_templates` is false.
[[nodiscard]] std::string expand_prompt_input(
    std::string text,
    const std::vector<Skill>& skills,
    const std::vector<PromptTemplate>& templates,
    bool expand_prompt_templates);

} // namespace cch::coding_agent::prompt
