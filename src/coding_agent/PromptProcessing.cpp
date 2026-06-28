#include "../../include/cch/coding_agent/PromptProcessing.hpp"

#include "../../include/cch/coding_agent/CommandRegistry.hpp"
#include "../../include/cch/coding_agent/PromptProcessingPipeline.hpp"
#include "../../include/cch/coding_agent/PromptTemplate.hpp"
#include "../../include/cch/coding_agent/PromptTemplateExpander.hpp"
#include "../../include/cch/coding_agent/Skill.hpp"
#include "../../include/cch/coding_agent/SkillExpander.hpp"
#include "harness/WorkspaceFileSystem.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent {

std::string expand_skill_command(
    std::string_view input,
    const std::vector<Skill>& skills,
    const harness::WorkspaceFileSystem& fs) {
    return SkillExpander{skills, fs}.expand_and_print(input);
}

SkillExpansionResult expand_skill_command_silent(
    std::string_view input,
    const std::vector<Skill>& skills) {
    const harness::WorkspaceFileSystem default_fs{};
    return SkillExpander{skills, default_fs}.expand(input);
}

PromptProcessingResult process_prompt(
    std::string_view raw_input,
    const std::vector<PromptTemplate>& templates,
    CommandRegistry& registry,
    const CommandContext& ctx) {
    const std::vector<Skill> empty_skills_vec{};
    const harness::WorkspaceFileSystem default_fs{};
    SkillExpander empty_skills{empty_skills_vec, default_fs};
    PromptProcessingPipeline pipeline{
        PromptTemplateExpander{templates},
        registry,
        empty_skills};
    return pipeline.process(raw_input, ctx);
}

PromptProcessingResult process_prompt(
    std::string_view raw_input,
    const std::vector<PromptTemplate>& templates,
    CommandRegistry& registry,
    const CommandContext& ctx,
    const std::vector<Skill>& skills,
    const harness::WorkspaceFileSystem& fs) {
    PromptProcessingPipeline pipeline{
        PromptTemplateExpander{templates},
        registry,
        SkillExpander{skills, fs}};
    return pipeline.process(raw_input, ctx);
}

} // namespace cch::coding_agent
