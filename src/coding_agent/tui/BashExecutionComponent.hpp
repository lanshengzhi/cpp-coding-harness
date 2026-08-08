#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Loader.hpp>
#include <cch/util/Error.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace cch::tui {
class KeybindingRegistry;
} // namespace cch::tui

namespace cch::coding_agent::tui {

class LiveTheme;

/// pi `bash-execution.ts`: one User Bash block — a `$ command` header styled
/// by model-context inclusion, a muted output preview derived from the last
/// 20 logical lines and bounded to 20 visual lines unless expanded, the
/// running loader, and concise cancellation/exit/truncation status with
/// effective keybinding hints. Renders the border frame around the content
/// and is shared by the live pending block and committed/resumed entries.
class BashExecutionComponent final : public cch::tui::Component {
public:
    /// The theme and keybindings must outlive this component.
    BashExecutionComponent(
        const LiveTheme& theme,
        const cch::tui::KeybindingRegistry& keybindings,
        std::string command,
        bool exclude_from_context);
    ~BashExecutionComponent() override;

    BashExecutionComponent(const BashExecutionComponent&) = delete;
    BashExecutionComponent& operator=(const BashExecutionComponent&) = delete;

    /// Feed output chunks; ANSI sequences are stripped and line endings
    /// normalized (pi `appendOutput`).
    void append_output(std::string chunk);
    /// Terminal outcome (pi `setComplete`). Running keeps the loader.
    void set_complete(
        std::optional<int> exit_code,
        bool cancelled,
        bool truncated,
        std::optional<std::string> full_output_path);
    void set_expanded(bool expanded);
    /// Starts the running loader; the request_render sink drives the owning
    /// view's invalidation. Ignored once the outcome is set.
    void start_loader(cch::tui::RenderRequestSink request_render);

    [[nodiscard]] util::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override;

private:
    const LiveTheme& theme_; // must outlive this component.
    const cch::tui::KeybindingRegistry& keybindings_; // must outlive this component.
    std::string command_;
    std::string output_;
    bool exclude_from_context_{false};
    bool running_{true};
    std::optional<int> exit_code_;
    bool cancelled_{false};
    bool truncated_{false};
    std::optional<std::string> full_output_path_;
    bool expanded_{false};
    std::unique_ptr<cch::tui::Loader> loader_;
};

} // namespace cch::coding_agent::tui
