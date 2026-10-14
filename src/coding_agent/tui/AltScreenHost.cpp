#include "coding_agent/tui/AltScreenHost.hpp"

#include <cch/support/Error.hpp>

#include <type_traits>
#include <utility>

namespace cch::coding_agent::tui {

AltScreenHost::AltScreenHost(cch::tui::Terminal& terminal,
        TuiActionSink& action_sink,
        const std::size_t& action_generation,
        SelectionCopyHook feedback)
    : alt_screen_(terminal), action_sink_(action_sink), action_generation_(action_generation),
      feedback_(std::move(feedback)) {
    alt_screen_.set_selection_copy_sink([this](std::string_view text) { return copy_selection(text); });
}

void AltScreenHost::set_copy_on_select(bool enabled) { alt_screen_.set_copy_on_select(enabled); }

bool AltScreenHost::copy_on_select() const { return alt_screen_.copy_on_select(); }

bool AltScreenHost::has_active_selection() const { return alt_screen_.has_active_selection(); }

support::Expected<bool> AltScreenHost::copy_active_selection_to_clipboard() {
    return alt_screen_.copy_active_selection_to_clipboard();
}

bool AltScreenHost::copy_selection_for_keyboard() {
    if (copy_on_select() || !has_active_selection()) return false;
    static_cast<void>(copy_active_selection_to_clipboard());
    return true;
}

support::ExpectedVoid AltScreenHost::copy_selection(std::string_view text) {
    if (!action_sink_) {
        auto result = support::make_error(support::ErrorCode::Unknown, "Clipboard unavailable");
        const ClipboardCopyResult copy_result{.copied = false, .error = result.message};
        if (feedback_) feedback_(copy_result);
        return std::unexpected(std::move(result));
    }

    auto action_result = action_sink_(action_generation_, TuiActionVariant{WriteClipboardAction{std::string{text}}});
    if (!action_result) {
        auto error = action_result.error();
        const ClipboardCopyResult copy_result{.copied = false, .error = error.message};
        if (feedback_) feedback_(copy_result);
        return std::unexpected(std::move(error));
    }

    const auto* outcome = std::get_if<ClipboardCopyResult>(&*action_result);
    if (outcome == nullptr) {
        auto error = support::make_error(support::ErrorCode::Unknown, "Clipboard unavailable");
        const ClipboardCopyResult copy_result{.copied = false, .error = error.message};
        if (feedback_) feedback_(copy_result);
        return std::unexpected(std::move(error));
    }

    if (feedback_) feedback_(*outcome);
    if (!outcome->copied) {
        return std::unexpected(support::make_error(support::ErrorCode::Unknown, outcome->error));
    }
    return {};
}

} // namespace cch::coding_agent::tui
