#pragma once

#include "coding_agent/extensions/ExtensionTool.hpp"

#include <cch/support/Error.hpp>

#include <vector>

namespace cch::coding_agent::extensions {

/// The Extension Tool Source (spec #865): the one seam through which external
/// capabilities (MCP servers, codemode scripts) contribute Agent Tools. A
/// source yields extension-provided Tools; `load_extension_tools` collects
/// them and `register_extension_tools` converts them into `cch::agent::Tool`
/// values and registers them in the session's ToolRegistry at assembly time,
/// before the registry moves into the Agent.
///
/// A source never reaches the Agent: it produces passive Tool values only, and
/// the Agent's ordinary execution path runs them (no execution semantic lives
/// here).
class ExtensionToolSource {
public:
    virtual ~ExtensionToolSource() = default;

    /// The tools this source contributes. A source that cannot supply its
    /// tools reports through the shared error channel rather than contributing
    /// a partial or empty loadout silently.
    [[nodiscard]] virtual support::Expected<std::vector<ExtensionTool>> load_tools() = 0;
};

} // namespace cch::coding_agent::extensions
