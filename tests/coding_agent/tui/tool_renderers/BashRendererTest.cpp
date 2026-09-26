#include "coding_agent/tui/tool_renderers/BashRenderer.hpp"
#include "coding_agent/tui/tool_renderers/ToolRendererRegistry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string_view>

using namespace cch;

// Placeholder owned by #826. The seam contract this file inherits is
// that `bash` is registered by name in the default registry and carries
// both renderer halves; #826 replaces this case with the per-tool
// screen-text assertions.
TEST_CASE("the default registry resolves the bash renderer pair by name",
        "[coding_agent][tui][tool-renderers][issue824][spec]") {
    auto registry = coding_agent::tui::ToolRendererRegistry::make_default();
    auto& renderer = registry.lookup(std::string_view{"bash"});
    CHECK(static_cast<bool>(renderer.render_call));
    CHECK(static_cast<bool>(renderer.render_result));
}
