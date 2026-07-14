#pragma once

#include <cch/coding_agent/CommandRegistry.hpp>
#include <cch/coding_agent/PromptTemplate.hpp>
#include <cch/coding_agent/Skill.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cch::coding_agent::prompt {

struct PromptResources {
    CommandRegistry commands;
    std::vector<Skill> skills;
    std::vector<PromptTemplate> templates;
};

struct AgentPrompt {
    std::string text;
};

struct CommandHandled {
    std::string code;
    std::string feedback;
    bool shutdown_requested{false};
};

using PromptProcessingOutcome = std::variant<AgentPrompt, CommandHandled>;

namespace detail {

[[nodiscard]] std::optional<std::string> try_expand_skill(
    std::string_view input,
    const std::vector<Skill>& skills);

[[nodiscard]] std::optional<std::string> try_expand_prompt_template(
    std::string_view input,
    const std::vector<PromptTemplate>& templates);

/// Borrowed orchestration used only by the legacy public free-function wrappers.
[[nodiscard]] PromptProcessingOutcome process_prompt_borrowed(
    std::string input,
    CommandRegistry& commands,
    const std::vector<Skill>& skills,
    const std::vector<PromptTemplate>& templates,
    CommandContext context);

} // namespace detail

class PromptProcessor final {
public:
    explicit PromptProcessor(PromptResources resources);

    [[nodiscard]] PromptProcessingOutcome process(
        std::string input,
        CommandContext context);

    [[nodiscard]] const std::vector<Skill>& skills() const { return skills_; }
    [[nodiscard]] const std::vector<PromptTemplate>& templates() const { return templates_; }

private:
    CommandRegistry commands_;
    std::vector<Skill> skills_;
    std::vector<PromptTemplate> templates_;
};

} // namespace cch::coding_agent::prompt
