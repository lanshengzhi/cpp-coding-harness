#pragma once

#include <cch/coding_agent/PromptTemplate.hpp>
#include <cch/coding_agent/Skill.hpp>

#include <string>
#include <vector>

namespace cch::coding_agent::prompt {

struct AgentPrompt {
    std::string text;
};

class PromptProcessor final {
public:
    explicit PromptProcessor(std::vector<Skill> skills, std::vector<PromptTemplate> templates);

    [[nodiscard]] AgentPrompt process(
        std::string input,
        bool expand_templates);

    [[nodiscard]] const std::vector<Skill>& skills() const { return skills_; }
    [[nodiscard]] const std::vector<PromptTemplate>& templates() const { return templates_; }

private:
    std::vector<Skill> skills_;
    std::vector<PromptTemplate> templates_;
};

} // namespace cch::coding_agent::prompt
