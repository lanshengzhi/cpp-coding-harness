#pragma once

#include <cch/codemode/Sandbox.hpp>
#include <cch/durable/Workflow.hpp>

#include <memory>

namespace cch::durable {

class Environment;

/// Install durable codemode over an owned reusable sandbox. Scripts can call
/// the request's tools; model-provider execution is not introduced here. When
/// `environment` is supplied, token-budget spill files and script images are
/// written through its temporary-directory and write contract; without one
/// they keep the frozen explicit no-execution-environment result.
[[nodiscard]] Extension codemode_extension(
        std::unique_ptr<codemode::CodemodeSandbox> sandbox, std::shared_ptr<Environment> environment = nullptr);

/// The rewindable conversation store (frozen pi `CodemodeStoreDoc`).
[[nodiscard]] DocumentDefinition codemode_store_document();

} // namespace cch::durable
