#pragma once

#include "coding_agent/extensions/ExtensionToolSource.hpp"
#include "coding_agent/extensions/tool_search/ToolSearch.hpp"

#include <functional>
#include <vector>

namespace cch::coding_agent::extensions {

class ToolSearchToolSource final : public ExtensionToolSource {
public:
    using Search = std::function<std::vector<ToolSearchCandidate>(std::string_view, std::size_t)>;
    using Activate = std::function<void(std::vector<std::string>)>;
    ToolSearchToolSource(Search search, Activate activate);
    [[nodiscard]] support::Expected<std::vector<ExtensionTool>> load_tools() override;

private:
    Search search_;
    Activate activate_;
};

} // namespace cch::coding_agent::extensions
