#pragma once

#include <cch/codemode/Sandbox.hpp>
#include <cch/durable/Workflow.hpp>

#include <memory>

namespace cch::durable {

/// Install durable codemode over an owned reusable sandbox. Scripts can call
/// the request's tools; model-provider execution is not introduced here.
[[nodiscard]] Extension codemode_extension(std::unique_ptr<codemode::CodemodeSandbox> sandbox);

/// The rewindable conversation store (frozen pi `CodemodeStoreDoc`).
[[nodiscard]] DocumentDefinition codemode_store_document();

} // namespace cch::durable
