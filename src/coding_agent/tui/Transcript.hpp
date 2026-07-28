#pragma once

#include <cch/agent/AgentEvent.hpp>
#include <cch/coding_agent/AgentSessionSnapshot.hpp>
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
    void append_diagnostic(std::string text);
    void toggle_tool_output();
    void toggle_thinking();

    [[nodiscard]] util::Expected<std::vector<std::string>> render(std::size_t width) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::coding_agent::tui
