#include "RuntimeServices.hpp"

#include "../../../include/cch/ai/ProviderRegistry.hpp"
#include "../../../include/cch/tools/ToolFactories.hpp"

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
    services.tools.add(tools::make_async_read_file_tool(services.env));
    services.tools.add(tools::make_async_write_file_tool(services.env));
    services.tools.add(tools::make_async_edit_file_tool(services.env));
    services.tools.add(tools::make_async_bash_tool(services.env));
    return services;
}

} // namespace cch::coding_agent::runtime
