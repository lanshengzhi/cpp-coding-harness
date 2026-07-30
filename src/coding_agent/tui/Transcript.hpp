#pragma once

#include <cch/agent/AgentEvent.hpp>
#include <cch/ai/Message.hpp>
#include <cch/coding_agent/AgentSessionSnapshot.hpp>
#include <cch/tui/Component.hpp>
#include <cch/util/Error.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace cch::tui {
class KeybindingRegistry;
} // namespace cch::tui

namespace cch::coding_agent::tui {

class LiveTheme;

/// Local presentation reducer for one ordered Agent Session transcript.
/// It copies shared message values and keeps collapse/stream/tool state private
/// to the Native TUI.
class Transcript final {
public:
    /// The theme must outlive this Transcript.
    explicit Transcript(
        const LiveTheme& theme,
        const cch::tui::KeybindingRegistry& keybindings);
    Transcript(Transcript&&) noexcept;
    Transcript& operator=(Transcript&&) noexcept;
    ~Transcript();

    Transcript(const Transcript&) = delete;
    Transcript& operator=(const Transcript&) = delete;

    void initialize(const AgentSessionSnapshot& snapshot);
    void apply_event(const agent::AgentLifecycleEvent& event);
    /// Append one runtime-confirmed passive message that has no lifecycle event.
    void append_committed_message(ai::MessageVariant message);
    void clear();
    void append_frontend_message(std::string text);
    void append_diagnostic(std::string text);
    void toggle_tool_output();
    void toggle_thinking();
    /// Whether tool and User Bash output currently renders in full.
    [[nodiscard]] bool tools_expanded() const;

    [[nodiscard]] util::Expected<cch::tui::RenderResult> render(std::size_t width) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::coding_agent::tui
