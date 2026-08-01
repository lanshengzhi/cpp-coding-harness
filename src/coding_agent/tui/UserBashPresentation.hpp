#pragma once

#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Loader.hpp>
#include <cch/util/Error.hpp>
#include "coding_agent/runtime/UserBash.hpp"
#include "coding_agent/tui/BashBlock.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cch::coding_agent::tui {

/// One conversion seam for User Bash presentation: the live pending block and
/// committed or resumed transcript entries flow through the same
/// BashBlockView, so the running/completed distinction lives here instead of
/// at each render call site.
[[nodiscard]] BashBlockView user_bash_block_view(
    const runtime::UserBashProgress& progress);
[[nodiscard]] BashBlockView user_bash_block_view(
    const ai::BashExecutionMessage& message);

/// Builds the loader shown beneath a still-running block. The caller owns
/// styling and invalidation; the pending presentation owns when one exists.
using UserBashLoaderFactory =
    std::move_only_function<std::unique_ptr<cch::tui::Loader>(
        bool exclude_from_context)>;

/// Live User Bash pending presentation: one progress value plus the loader
/// lifecycle derived from it. A loader exists exactly while the execution is
/// still running; it stops once the outcome awaits commitment and disappears
/// when the block is cleared for its committed transcript entry.
class PendingUserBashPresentation final {
public:
    explicit PendingUserBashPresentation(UserBashLoaderFactory make_loader);

    void update(runtime::UserBashProgress progress);
    void clear();
    [[nodiscard]] bool active() const;

    /// Renders the pending block (plus the loader while running) through the
    /// same render_bash_block presentation used by committed entries.
    [[nodiscard]] util::Expected<std::vector<std::string>> render(
        const LiveTheme& theme,
        const cch::tui::KeybindingRegistry& keybindings,
        bool expanded,
        std::size_t width);

private:
    UserBashLoaderFactory make_loader_;
    std::optional<runtime::UserBashProgress> progress_;
    std::unique_ptr<cch::tui::Loader> loader_;
};

} // namespace cch::coding_agent::tui
