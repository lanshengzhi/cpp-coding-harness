#pragma once

#include <cch/ai/Message.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Container.hpp>
#include <cch/tui/Image.hpp>
#include <cch/util/Error.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cch::tui {
class KeybindingRegistry;
} // namespace cch::tui

namespace cch::coding_agent::tui {

class LiveTheme;

/// pi `tool-execution.ts` subset: one model tool call rendered in a padded
/// box whose background transitions pending → success/error, with the
/// `edit` tool carrying pi's diff renderer (`edit <path>` header, result
/// diff in `toolDiff*` colors) and other tools using the generic
/// title + arguments + output fallback. Result images render inline.
class ToolExecutionComponent final : public cch::tui::Component {
public:
    /// The theme and keybindings must outlive this component.
    ToolExecutionComponent(
        const LiveTheme& theme,
        const cch::tui::KeybindingRegistry& keybindings,
        std::string tool_name,
        std::string tool_call_id,
        std::string arguments_json);
    ~ToolExecutionComponent() override;

    ToolExecutionComponent(const ToolExecutionComponent&) = delete;
    ToolExecutionComponent& operator=(const ToolExecutionComponent&) = delete;

    void update_args(std::string arguments_json);
    /// Settle the tool with its execution outcome (pi `updateResult`).
    void update_result(ai::ToolResultMessage result, bool is_partial = false);
    void set_expanded(bool expanded);

    [[nodiscard]] util::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override;

private:
    struct ImageSlot;

    void rebuild();

    const LiveTheme& theme_; // must outlive this component.
    const cch::tui::KeybindingRegistry& keybindings_; // must outlive this component.
    std::string tool_name_;
    std::string tool_call_id_;
    std::string arguments_json_;
    std::optional<ai::ToolResultMessage> result_;
    bool is_partial_{true};
    bool expanded_{false};
    cch::tui::Box box_;
    // In content order so multi-image results render in source order.
    std::vector<std::unique_ptr<ImageSlot>> image_slots_;
};

} // namespace cch::coding_agent::tui
