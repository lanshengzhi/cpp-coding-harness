#include "RuntimeServices.hpp"

#include "../../../include/cch/ai/ProviderRegistry.hpp"
#include "../../../include/cch/coding_agent/SkillLoader.hpp"
#include "../../../include/cch/tools/ToolFactories.hpp"
#include "../../harness/WorkspaceFileSystem.hpp"

#include <iostream>
#include <utility>
#include <vector>

namespace cch::coding_agent::runtime {

util::Expected<RuntimeServices> make_runtime_services(const RuntimeServicesConfig& config) {
    auto provider_registry = ai::make_default_provider_registry();
    if (!provider_registry) {
        return std::unexpected(provider_registry.error());
    }

    ai::ProviderFactoryContext provider_context;
    provider_context.model = config.model;
    provider_context.base_url = config.base_url;
    provider_context.api_key_env = config.api_key_env;
    auto client = provider_registry->create(config.provider_name, provider_context);
    if (!client) {
        return std::unexpected(client.error());
    }

    RuntimeServices services;
    services.client = std::move(*client);
    services.env = std::make_shared<harness::AsyncLocalExecutionEnv>(
        config.workspace,
        config.enable_bash,
        std::vector<std::string>{config.api_key_env});
    if (auto added = services.tools.add(tools::make_async_read_file_tool(services.env)); !added) {
        return std::unexpected(added.error());
    }
    if (auto added = services.tools.add(tools::make_async_write_file_tool(services.env)); !added) {
        return std::unexpected(added.error());
    }
    if (auto added = services.tools.add(tools::make_async_edit_file_tool(services.env)); !added) {
        return std::unexpected(added.error());
    }
    if (auto added = services.tools.add(tools::make_async_bash_tool(services.env)); !added) {
        return std::unexpected(added.error());
    }

    // Load skills from configured directories.
    if (!config.skill_dirs.empty()) {
        auto fs = harness::WorkspaceFileSystem::create(config.workspace);
        if (fs.has_value()) {
            services.skill_load_result = loadSkills(*fs, config.skill_dirs);

            if (config.print_skill_diagnostics) {
                for (const auto& diag : services.skill_load_result.diagnostics) {
                    std::cerr << "[skill:warn] ";
                    switch (diag.code) {
                    case SkillDiagnosticCode::file_info_failed:
                        std::cerr << "file_info_failed";
                        break;
                    case SkillDiagnosticCode::list_failed:
                        std::cerr << "list_failed";
                        break;
                    case SkillDiagnosticCode::read_failed:
                        std::cerr << "read_failed";
                        break;
                    case SkillDiagnosticCode::parse_failed:
                        std::cerr << "parse_failed";
                        break;
                    case SkillDiagnosticCode::invalid_metadata:
                        std::cerr << "invalid_metadata";
                        break;
                    case SkillDiagnosticCode::duplicate_name:
                        std::cerr << "duplicate_name";
                        break;
                    }
                    std::cerr << ": " << diag.message << " (" << diag.path << ")\n";
                }
            }
        }
    }

    return services;
}

} // namespace cch::coding_agent::runtime
