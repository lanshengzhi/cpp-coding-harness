#pragma once

#include "coding_agent/extensions/ExtensionTool.hpp"
#include "coding_agent/extensions/ExtensionToolSource.hpp"

#include <filesystem>

namespace cch::coding_agent::extensions {

/// The codemode guest module path the sandbox loads when no explicit path is
/// given: `$PIKE_CODEMODE_WASM` when set, else the committed fixture
/// (`<source tree>/fixtures/codemode/quickjs/quickjs.wasm`, spec #865 / ticket
/// #874). An installation that does not ship the fixture must set the variable.
[[nodiscard]] std::filesystem::path default_codemode_guest_wasm_path();

/// The codemode Extension Tool Source (spec #865, #870, #874): it turns each
/// project-local codemode declaration (`<workspace>/.pi/codemode/*.json`) into
/// an extension-provided Tool whose execute runs the declared script inside the
/// wasm sandbox and returns its value and output. The script source is parsed
/// and validated at load; a bad declaration fails the load explicitly. This
/// source never reaches the Agent: it produces passive Tool values only.
class CodemodeToolSource final : public ExtensionToolSource {
public:
    explicit CodemodeToolSource(std::filesystem::path workspace,
            std::filesystem::path guest_wasm_path = default_codemode_guest_wasm_path());

    /// Load every declaration under `codemode_declaration_directory(workspace)`.
    /// An invalid declaration, an unreadable declaration, or an unreadable
    /// source is a typed Validation error; no declaration is skipped silently.
    /// A project with no declarations contributes no tools and never loads the
    /// guest module.
    [[nodiscard]] support::Expected<std::vector<ExtensionTool>> load_tools() override;

private:
    std::filesystem::path workspace_;
    std::filesystem::path guest_wasm_path_;
};

} // namespace cch::coding_agent::extensions
