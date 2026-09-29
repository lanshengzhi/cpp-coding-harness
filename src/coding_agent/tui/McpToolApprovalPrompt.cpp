#include "coding_agent/tui/McpToolApprovalPrompt.hpp"

#include "DynamicBorder.hpp"
#include "KeybindingHints.hpp"
#include "Theme.hpp"

#include <cch/tui/Text.hpp>

#include <cch/support/Error.hpp>

#include <format>
#include <string>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

/// The rows of the question. Consent is per call, so the only two answers are
/// this call and not this call.
[[nodiscard]] std::vector<cch::tui::SelectItem> approval_items() {
    return std::vector<cch::tui::SelectItem>{
            cch::tui::SelectItem{
                    .value = std::string{McpToolApprovalPromptComponent::allow_value()},
                    .label = "Allow this call",
                    .description = "run it once on the Upstream MCP Server",
            },
            cch::tui::SelectItem{
                    .value = std::string{McpToolApprovalPromptComponent::deny_value()},
                    .label = "Deny this call",
                    .description = "do not run it; the model is told the call was refused",
            },
    };
}

/// The call's identity, one line per fact: the name the model called, and the
/// Server Id and Upstream tool name the call actually targets.
[[nodiscard]] std::string call_identity(const coding_agent::McpToolApprovalRequest& request) {
    return std::format(
            "Tool: {}\nUpstream: {} · {}", request.qualified_tool_name, request.server_id, request.tool_name);
}

} // namespace

McpToolApprovalPromptComponent::McpToolApprovalPromptComponent(const LiveTheme& theme,
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
        coding_agent::McpToolApprovalRequest request,
        McpToolApprovalAllowSink on_allow,
        McpToolApprovalDenySink on_deny,
        McpToolApprovalCancelSink on_cancel)
    : theme_(theme), keybindings_(std::move(keybindings)), request_(std::move(request)), on_allow_(std::move(on_allow)),
      on_deny_(std::move(on_deny)), on_cancel_(std::move(on_cancel)),
      // The list presentation lives in the shared SelectList; the sinks only
      // re-enter this component, which outlives neither.
      select_list_(approval_items(),
              cch::tui::SelectListOptions{
                      .theme = theme_.select_list_theme(),
                      .on_select = [this](const cch::tui::SelectItem& item) -> support::ExpectedVoid {
                          if (item.value == allow_value()) {
                              if (on_allow_) on_allow_();
                          } else if (on_deny_) {
                              on_deny_();
                          }
                          return {};
                      },
                      .on_cancel = [this]() -> support::ExpectedVoid {
                          if (on_cancel_) on_cancel_();
                          return {};
                      },
                      .keybindings = keybindings_,
                      .wrap_navigation = false,
              }) {}

support::Expected<cch::tui::RenderResult> McpToolApprovalPromptComponent::render(std::size_t width) {
    cch::tui::RenderResult result;
    const auto append = [&result, width](cch::tui::Component& component) -> support::ExpectedVoid {
        auto rendered = component.render(width);
        if (!rendered) return std::unexpected(rendered.error());
        for (auto& line : rendered->lines)
            result.lines.push_back(std::move(line));
        return {};
    };

    DynamicBorder top_border(theme_.foreground_hook(ThemeToken::Border));
    if (auto appended = append(top_border); !appended) return std::unexpected(appended.error());
    {
        cch::tui::Text spacer("", 1, 0);
        if (auto appended = append(spacer); !appended) return std::unexpected(appended.error());
    }
    {
        cch::tui::Text title(theme_.foreground(ThemeToken::Accent, "Allow this tool call?"), 0, 0);
        if (auto appended = append(title); !appended) return std::unexpected(appended.error());
    }
    // The whole question: which tool, on which Upstream, with which
    // arguments. The approval is about this exact call, so the call's own
    // arguments are shown rather than summarized.
    {
        cch::tui::Text identity(theme_.foreground(ThemeToken::Text, call_identity(request_)), 0, 0);
        if (auto appended = append(identity); !appended) return std::unexpected(appended.error());
    }
    {
        cch::tui::Text arguments(theme_.foreground(ThemeToken::Muted, "Arguments: " + request_.arguments_json), 0, 0);
        if (auto appended = append(arguments); !appended) return std::unexpected(appended.error());
    }
    {
        cch::tui::Text spacer("", 1, 0);
        if (auto appended = append(spacer); !appended) return std::unexpected(appended.error());
    }

    cursor_row_offset_ = result.lines.size();
    {
        auto rendered = select_list_.render(width);
        if (!rendered) return std::unexpected(rendered.error());
        for (auto& line : rendered->lines)
            result.lines.push_back(std::move(line));
    }
    {
        cch::tui::Text spacer("", 1, 0);
        if (auto appended = append(spacer); !appended) return std::unexpected(appended.error());
    }
    {
        cch::tui::Text hint(
                theme_.foreground(ThemeToken::Muted,
                        std::string{"  "} + format_key_text(keybindings_->key_text("tui.select.confirm"), true) +
                                " to choose · " + format_key_text(keybindings_->key_text("tui.select.cancel"), true) +
                                " to dismiss (nothing is remembered)"),
                0,
                0);
        if (auto appended = append(hint); !appended) return std::unexpected(appended.error());
    }
    DynamicBorder bottom_border(theme_.foreground_hook(ThemeToken::Border));
    if (auto appended = append(bottom_border); !appended) return std::unexpected(appended.error());
    return result;
}

void McpToolApprovalPromptComponent::invalidate() {}

cch::tui::InputAdmissionOutcome McpToolApprovalPromptComponent::handle_input(const cch::tui::InputEventVariant& input) {
    const auto* key = std::get_if<cch::tui::KeyEvent>(&input);
    if (key != nullptr && key->type == cch::tui::KeyEventType::Release) {
        return cch::tui::InputAdmissionOutcome::Unhandled;
    }
    // Navigation, confirm, and dismissal all belong to the SelectList.
    static_cast<void>(select_list_.handle_input(input));
    return cch::tui::InputAdmissionOutcome::Consumed;
}

void McpToolApprovalPromptComponent::set_focused(bool focused) {
    focused_ = focused;
    select_list_.set_focused(focused);
}

bool McpToolApprovalPromptComponent::focused() const { return focused_; }

std::optional<cch::tui::CursorPosition> McpToolApprovalPromptComponent::cursor_location() const {
    const auto cursor = select_list_.cursor_location();
    if (!cursor) return std::nullopt;
    return cch::tui::CursorPosition{
            .column = cursor->column,
            .row = cursor->row + cursor_row_offset_,
    };
}

} // namespace cch::coding_agent::tui
