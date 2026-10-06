#pragma once

#include "coding_agent/extensions/ExtensionTool.hpp"
#include "coding_agent/extensions/ExtensionToolSource.hpp"

#include <filesystem>

namespace cch::coding_agent::extensions {

/// The codemode Extension Tool Source (spec #865, #870): it turns each
/// project-local codemode declaration (`<workspace>/.pi/codemode/*.json`) into
/// an extension-provided Tool so a declared script tool joins the session's
/// tool surface. The script source is parsed and validated at load; #870 does
/// not execute it — the Tool's execute reports an explicit Validation error
/// until the sandbox slice (#874) wires execution. This source never reaches
/// the Agent: it produces passive Tool values only.
class CodemodeToolSource final : public ExtensionToolSource {
public:
    explicit CodemodeToolSource(std::filesystem::path workspace);

    /// Load every declaration under `codemode_declaration_directory(workspace)`.
    /// An invalid declaration, an unreadable declaration, or an unreadable
    /// source is a typed Validation error; no declaration is skipped silently.
    [[nodiscard]] support::Expected<std::vector<ExtensionTool>> load_tools() override;

private:
    std::filesystem::path workspace_;
};

} // namespace cch::coding_agent::extensions
