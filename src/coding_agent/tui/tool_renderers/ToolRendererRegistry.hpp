#pragma once

#include "coding_agent/tui/tool_renderers/ToolRenderer.hpp"

#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace cch::coding_agent::tui {

/// pi `core/tools/renderers/index.ts`: a map from tool name to a
/// call/result pair, plus the fallback pair every unregistered name takes.
///
/// The registry is an application-layer construct (ADR 0053): it lives in the
/// coding-agent frontend and nothing under `src/agent/` names it.
class ToolRendererRegistry final {
public:
    /// A registry carrying only the fallback pair.
    ToolRendererRegistry();

    ToolRendererRegistry(ToolRendererRegistry&&) noexcept;
    ToolRendererRegistry& operator=(ToolRendererRegistry&&) noexcept;
    ~ToolRendererRegistry();

    ToolRendererRegistry(const ToolRendererRegistry&) = delete;
    ToolRendererRegistry& operator=(const ToolRendererRegistry&) = delete;

    /// pi `createAllToolRenderers()`: the fallback plus one registered pair per
    /// built-in tool that has a renderer.
    [[nodiscard]] static ToolRendererRegistry make_default();

    /// Register or replace one tool's pair. Registering a pair with an empty
    /// half leaves that half to the fallback (pi's `??` precedence).
    void register_renderer(std::string tool_name, ToolRenderer renderer);

    /// The pair registered for `tool_name`, or the fallback pair when the
    /// name carries none. The reference stays valid for the registry's
    /// lifetime; the host takes each half from it, falling back per half
    /// exactly as pi's `updateDisplay` does. The halves are handed out
    /// mutable because `std::move_only_function::operator()` is not const.
    [[nodiscard]] ToolRenderer& lookup(std::string_view tool_name);

    /// The fallback pair itself: the presentation of a tool that has no
    /// renderer.
    [[nodiscard]] ToolRenderer& fallback() { return fallback_; }

private:
    std::map<std::string, ToolRenderer, std::less<>> renderers_;
    ToolRenderer fallback_;
};

} // namespace cch::coding_agent::tui
