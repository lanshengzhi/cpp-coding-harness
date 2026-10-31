#pragma once

#include "coding_agent/extensions/ExtensionTool.hpp"
#include "coding_agent/extensions/ExtensionToolSource.hpp"
#include <cch/codemode/Sandbox.hpp>

#include <filesystem>
#include <memory>

namespace cch::coding_agent::extensions {

class CodemodeCatalogState;

/// The codemode Extension Tool Source (spec #882, ticket #885): it yields pi's
/// single model-facing inline `codemode` tool. There is no on-disk declaration
/// face — scripts are written inline in the model's tool call, exactly as pi's
/// `packages/coding-agent/src/extensions/codemode/` does. The source produces
/// one passive Tool; the sandbox guest module loads on the first execute, not
/// at assembly (pi's lazy `loadCodemodeExecutor`), so a session that never
/// calls codemode does not touch the guest.
///
/// `catalog` is the session's live loadout state (pi `CodemodeToolOptions`'
/// `getToolNamespace`/`getToolGuidelines` closures and `ctx.tools`): the script
/// surface and the discovery globals resolve against the same exposure-filtered
/// callable set the catalog description lists. Without one (the unit-test
/// seam), every registered tool except `codemode` is callable and ungrouped.
class CodemodeToolSource final : public ExtensionToolSource {
public:
    explicit CodemodeToolSource(std::filesystem::path guest_wasm_path = codemode::default_codemode_guest_wasm_path(),
            std::shared_ptr<const CodemodeCatalogState> catalog = nullptr);

    /// Contribute the one `codemode` tool. Loading never touches the guest
    /// module: a missing or invalid guest surfaces as the tool call's explicit
    /// error, never as an assembly failure for a session that does not use it.
    [[nodiscard]] support::Expected<std::vector<ExtensionTool>> load_tools() override;

private:
    std::filesystem::path guest_wasm_path_;
    std::shared_ptr<const CodemodeCatalogState> catalog_;
};

} // namespace cch::coding_agent::extensions
