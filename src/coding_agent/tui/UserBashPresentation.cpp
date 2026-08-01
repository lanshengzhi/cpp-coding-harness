#include "coding_agent/tui/UserBashPresentation.hpp"

#include <iterator>
#include <utility>

namespace cch::coding_agent::tui {

BashBlockView user_bash_block_view(const runtime::UserBashProgress& progress) {
    return BashBlockView{
        .command = progress.command,
        .output = progress.output,
        .exclude_from_context = progress.exclude_from_context,
        // An execution whose completed outcome awaits commitment already
        // shows the final block form; only a still-running execution
        // withholds its status lines.
        .running = !progress.awaiting_commitment,
        .exit_code = progress.exit_code,
        .cancelled = progress.cancelled,
        .truncated = progress.truncated,
        .full_output_path = progress.full_output_path,
    };
}

BashBlockView user_bash_block_view(const ai::BashExecutionMessage& message) {
    return BashBlockView{
        .command = message.command,
        .output = message.output,
        .exclude_from_context = message.exclude_from_context,
        .running = false,
        .exit_code = message.exit_code,
        .cancelled = message.cancelled,
        .truncated = message.truncated,
        .full_output_path = message.full_output_path,
    };
}

PendingUserBashPresentation::PendingUserBashPresentation(
    UserBashLoaderFactory make_loader)
    : make_loader_(std::move(make_loader)) {}

void PendingUserBashPresentation::update(runtime::UserBashProgress progress) {
    if (!progress.awaiting_commitment) {
        if (!loader_ && make_loader_) {
            loader_ = make_loader_(progress.exclude_from_context);
        }
    } else if (loader_) {
        loader_->stop();
        loader_.reset();
    }
    progress_ = std::move(progress);
}

void PendingUserBashPresentation::clear() {
    if (loader_) {
        loader_->stop();
        loader_.reset();
    }
    progress_.reset();
}

bool PendingUserBashPresentation::active() const {
    return progress_.has_value();
}

util::Expected<std::vector<std::string>> PendingUserBashPresentation::render(
    const LiveTheme& theme,
    const cch::tui::KeybindingRegistry& keybindings,
    bool expanded,
    std::size_t width) {
    std::vector<std::string> lines;
    if (!progress_) return lines;
    auto block = render_bash_block(
        theme,
        keybindings,
        user_bash_block_view(*progress_),
        expanded,
        width);
    if (!block) return std::unexpected(block.error());
    lines = std::move(*block);
    if (!progress_->awaiting_commitment && loader_) {
        auto loader_lines = loader_->render(width);
        if (!loader_lines) return std::unexpected(loader_lines.error());
        lines.insert(
            lines.end(),
            std::make_move_iterator(loader_lines->lines.begin()),
            std::make_move_iterator(loader_lines->lines.end()));
    }
    return lines;
}

} // namespace cch::coding_agent::tui
