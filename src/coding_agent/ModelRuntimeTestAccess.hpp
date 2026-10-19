#pragma once

#include <cch/coding_agent/ModelRuntime.hpp>

#include <memory>
#include <utility>

namespace cch::coding_agent {

/// Private test construction keeps transport injection out of Owner Interfaces.
struct ModelRuntimeTestAccess final {
    [[nodiscard]] static std::shared_ptr<ModelRuntime> create(
            std::shared_ptr<ai::Models> models, std::filesystem::path agent_dir) {
        return ModelRuntime::from_models(std::move(models), std::move(agent_dir));
    }
};

} // namespace cch::coding_agent
