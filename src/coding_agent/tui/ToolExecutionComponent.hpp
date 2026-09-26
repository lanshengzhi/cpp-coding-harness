#pragma once

#include "coding_agent/tui/SharedKeybindings.hpp"
#include "coding_agent/tui/tool_renderers/ToolRendererRegistry.hpp"

#include <cch/ai/Message.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Container.hpp>
#include <cch/tui/Image.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cch::coding_agent::tui {

class LiveTheme;

/// pi `tool-execution.ts`: one model tool call rendered in a padded box whose
/// background transitions pending → success/error. The call title and the
/// result body come from the tool's renderer pair, resolved by name through
/// the application-layer `ToolRendererRegistry`; a tool with no renderer takes
/// the registry's fallback. Result images render inline.
class ToolExecutionComponent final : public cch::tui::Component {
public:
    /// The theme must outlive this component; keybindings resolve through
    /// the shared slot (ADR 0035, #418). `cwd` is the session workspace the
    /// tool execution runs in (pi `ToolRenderContext.cwd`). The renderer table
    /// is passed in the way pi's `ToolExecutionComponent` constructor receives
    /// the registered tool definition (`tool-execution.ts:81`); the default
    /// is the built-in table.
    ToolExecutionComponent(const LiveTheme& theme,
            std::shared_ptr<const SharedKeybindings> keybindings,
            std::string tool_name,
            std::string tool_call_id,
            std::string arguments_json,
            std::string cwd,
            ToolRendererRegistry registry = ToolRendererRegistry::make_default());
    ~ToolExecutionComponent() override;

    ToolExecutionComponent(const ToolExecutionComponent&) = delete;
    ToolExecutionComponent& operator=(const ToolExecutionComponent&) = delete;

    void update_args(std::string arguments_json);
    /// Settle the tool with its execution outcome (pi `updateResult`).
    void update_result(ai::ToolResultMessage result, bool is_partial = false);
    void set_expanded(bool expanded);
    /// pi `markExecutionStarted`: the agent began running the tool. Stamps the
    /// execution clock once, so a renderer can measure how long the tool ran.
    void mark_execution_started();
    /// pi `setArgsComplete`: the streamed arguments stopped growing.
    void set_args_complete();

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override;

private:
    struct ImageSlot;

    void rebuild();
    void rebuild_image_slots();
    /// The exact parse of the accumulated argument text, an empty object while
    /// the arguments are still incomplete.
    [[nodiscard]] support::JsonValue parsed_arguments() const;
    /// One box child per renderer half, the way pi adds one component per
    /// renderer call.
    void append_rendered(const ToolRenderedText& rendered);

    const LiveTheme& theme_; // must outlive this component.
    /// The shared keybinding slot (ADR 0035); the strong reference keeps
    /// the registry alive for every render.
    std::shared_ptr<const SharedKeybindings> keybindings_;
    std::string tool_name_;
    std::string tool_call_id_;
    std::string arguments_json_;
    std::string cwd_;
    std::optional<ai::ToolResultMessage> result_;
    bool is_partial_{true};
    bool expanded_{false};
    bool args_complete_{false};
    /// The execution-start and result-settlement instants pi keeps in the
    /// renderer state; nullopt until the transition happens.
    std::optional<ai::TimestampMs> started_at_ms_{std::nullopt};
    std::optional<ai::TimestampMs> ended_at_ms_{std::nullopt};
    /// pi `state.cachedWidth`: the width the last render measured at, so a
    /// renderer that folds to the frame width is recomputed when that width
    /// changes (pi's `if (state.cachedLines === undefined || state.cachedWidth !== width)`).
    /// Nullopt until the first render, which keeps the first paint a width
    /// change rather than a fold at the default.
    std::optional<std::size_t> last_rendered_width_{std::nullopt};
    /// The resolved `app.tools.expand` key text and the hint built from it,
    /// refreshed per rebuild so a `/reload` shows the new binding.
    std::string expand_key_;
    std::string expand_hint_;
    ToolRendererRegistry registry_;
    cch::tui::Box box_;
    // In content order so multi-image results render in source order.
    std::vector<std::unique_ptr<ImageSlot>> image_slots_;
};

} // namespace cch::coding_agent::tui
