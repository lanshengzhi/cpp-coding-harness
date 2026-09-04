#pragma once

// The Native TUI main-screen composition (pi `interactive-mode.ts` main
// screen): header hints, loaded-resources block, chat, pending messages,
// status, editor, and footer, stacked in pi's order with the chat absorbing
// the flexible space. It emits one closed `ViewAction` value to the owning
// `InteractiveEngine` through one move-only `ViewActionSink`; render-state
// invalidation stays a separate coalescible request.
//
// Repository-private `cch_coding_agent` implementation header: not part of an
// Owner Interface, not installed, never exported.

#include "coding_agent/tui/InteractiveViewActions.hpp"

#include "coding_agent/tui/BashExecutionComponent.hpp"
#include "coding_agent/tui/ChatContainer.hpp"
#include "coding_agent/tui/Footer.hpp"
#include "coding_agent/tui/KeybindingHints.hpp"
#include "coding_agent/tui/LoadedResources.hpp"
#include "coding_agent/tui/SharedKeybindings.hpp"
#include "coding_agent/tui/StatusIndicator.hpp"
#include "coding_agent/tui/Theme.hpp"

#include "coding_agent/runtime/UserBash.hpp"
#include <cch/agent/AgentEvent.hpp>
#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/coding_agent/AgentSessionSnapshot.hpp>
#include <cch/tui/Autocomplete.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Editor.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/support/Error.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::coding_agent::tui {

/// The view's coalescible render request; kept separate from the action seam.
using InvalidateSink = std::move_only_function<void()>;

namespace interactive_view_detail {

/// Trims ASCII whitespace from both ends of one editor submission.
[[nodiscard]] inline std::string trim_editor_submission(std::string text) {
    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char value) {
        return std::isspace(value) != 0;
    });
    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char value) {
        return std::isspace(value) != 0;
    }).base();
    if (first >= last) return {};
    return {first, last};
}

/// Bash mode is the unsubmitted editor state whose trimmed text begins with
/// `!`; it exists only where User Bash dispatch is available.
[[nodiscard]] inline bool user_bash_editor_mode(
    std::string text,
    bool user_bash_available) {
    return user_bash_available &&
        trim_editor_submission(std::move(text)).starts_with('!');
}

/// The text the interrupt flow should restore when it clears the pending
/// Bash block: the current text minus the sampled prefix/suffix the editor
/// kept while the request was in flight.
[[nodiscard]] inline std::string editor_text_after_interrupt(
    std::string_view sampled_text,
    std::string_view current_text) {
    std::size_t prefix = 0;
    while (prefix < sampled_text.size() && prefix < current_text.size() &&
        sampled_text[prefix] == current_text[prefix]) {
        ++prefix;
    }

    std::size_t suffix = 0;
    while (suffix < sampled_text.size() - prefix &&
        suffix < current_text.size() - prefix &&
        sampled_text[sampled_text.size() - suffix - 1] ==
            current_text[current_text.size() - suffix - 1]) {
        ++suffix;
    }
    return std::string{current_text.substr(
        prefix,
        current_text.size() - prefix - suffix)};
}

/// pi `theme.ts` `getThinkingBorderColor`: the editor border token for a
/// thinking-level wire name ("off".."max"); unknown levels fall back to
/// `thinkingOff` like pi's default branch.
[[nodiscard]] inline ThemeToken thinking_border_token_for(std::string_view level) {
    if (level == "minimal") return ThemeToken::ThinkingMinimal;
    if (level == "low") return ThemeToken::ThinkingLow;
    if (level == "medium") return ThemeToken::ThinkingMedium;
    if (level == "high") return ThemeToken::ThinkingHigh;
    if (level == "xhigh") return ThemeToken::ThinkingXhigh;
    if (level == "max") return ThemeToken::ThinkingMax;
    return ThemeToken::ThinkingOff;
}

/// The editor-restorable text of one queued user message, or nullopt when the
/// message has content the editor cannot restore (images etc.).
[[nodiscard]] inline std::optional<std::string> queued_editor_text(
    const ai::MessageVariant& message) {
    const auto* user = std::get_if<ai::UserMessage>(&message);
    if (user == nullptr) {
        return std::nullopt;
    }
    std::string text;
    if (const auto* value = std::get_if<std::string>(&user->content)) {
        text = *value;
    } else {
        const auto& blocks = std::get<std::vector<ai::Content>>(user->content);
        if (std::any_of(
                blocks.begin(),
                blocks.end(),
                [](const auto& block) {
                    return !std::holds_alternative<ai::TextContent>(block);
                })) {
            return std::nullopt;
        }
        text = ai::text_from_content(blocks);
    }
    if (text.empty()) return std::nullopt;
    return text;
}

/// The editor-restorable texts of every queued steering and follow-up
/// message, in queue order; an error when any message carries content the
/// editor cannot restore.
[[nodiscard]] inline support::Expected<std::vector<std::string>> queued_editor_texts(
    const agent::AgentInputQueues& queues) {
    std::vector<std::string> restored;
    restored.reserve(
        queues.steering.messages.size() + queues.follow_up.messages.size());
    const auto append = [&restored](const auto& messages) {
        for (const auto& message : messages) {
            auto text = queued_editor_text(message);
            if (!text) return false;
            restored.push_back(std::move(*text));
        }
        return true;
    };
    if (!append(queues.steering.messages) || !append(queues.follow_up.messages)) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "queued input contains content that the editor cannot restore"));
    }
    return restored;
}

} // namespace interactive_view_detail

/// Assembly options for the main-screen composition. The one `action_sink`
/// carries every application-level view action; `on_invalidate` stays a
/// separate coalescible render request (ADR 0040).
struct InteractiveViewOptions {
    std::shared_ptr<SharedKeybindings> keybindings;
    InvalidateSink on_invalidate;
    ViewActionSink action_sink;
    /// Whether the main-screen keybinding hint should advertise clipboard
    /// image paste. This remains independent from the application action sink.
    bool clipboard_paste_available{false};
    /// Footer data source (pi footer.ts render inputs); polled on every
    /// render by the view's footer container. Installed by the state; the
    /// source must not re-enter the view.
    std::move_only_function<FooterData()> footer_data_source;
    bool hide_thinking_block{false};
    std::size_t output_pad{0};
    bool user_bash_available{false};
    std::unique_ptr<cch::tui::AutocompleteProvider> autocomplete_provider;
    std::unique_ptr<cch::tui::AutocompleteDebounceTimer> autocomplete_debounce_timer;
    cch::tui::EditorRenderRequestSink autocomplete_render_request;
    /// Must outlive the view: controller-owned live theme.
    const LiveTheme* theme{nullptr};
};

/// The pi main-screen composition. Emits one closed `ViewAction` per
/// application-level keybinding; toolkit-only state (editor text, clear,
/// tool-output expansion) stays inside the view.
class InteractiveView final
    : public cch::tui::Component,
      public cch::tui::InputHandler,
      public cch::tui::Focusable,
      public cch::tui::ViewportAware {
public:
    explicit InteractiveView(InteractiveViewOptions options);
    InteractiveView(InteractiveView&&) = delete;
    InteractiveView& operator=(InteractiveView&&) = delete;
    ~InteractiveView() override = default;
    InteractiveView(const InteractiveView&) = delete;
    InteractiveView& operator=(const InteractiveView&) = delete;

    void initialize(const AgentSessionSnapshot& snapshot);

    void apply_render_settings(bool hide_thinking_block, std::size_t output_pad);

    /// Replace the editor's autocomplete provider (pi
    /// `setupAutocompleteProvider` after a settings change).
    void set_autocomplete_provider(
        std::unique_ptr<cch::tui::AutocompleteProvider> provider);

    /// `/reload` keybinding re-catalog (pi `KeybindingsManager.reload()` →
    /// shared-manager mutation, ADR 0035): swap the shared slot every durable
    /// component observes and rebind the editor's snapshot. Serialized under
    /// the view mutex so concurrent render/input on the terminal thread never
    /// observes a torn registry.
    void set_keybindings(
        std::shared_ptr<const cch::tui::KeybindingRegistry> registry);

    void apply_event(const agent::AgentLifecycleEvent& event);
    void append_committed_message(ai::MessageVariant message);
    void clear_transcript();
    void append_frontend_message(std::string text);
    void append_diagnostic(std::string text);
    void append_warning(std::string text);
    void append_trust_warning(std::string text);
    void append_status_message(std::string text);

    /// pi `showStatusIndicator` surface over the status container: the
    /// active indicator replaces the previous one (Working/Compaction on
    /// accent, Retry on warning) and animates through the TUI Loader.
    void show_status_working(std::string message = "Working...");

    /// pi `RetryStatusIndicator` countdown tick: rewrite the retry message
    /// without replacing the loader.
    void show_status_compaction(std::string_view reason);
    void show_status_retry(int attempt, int max_attempts, int seconds);
    void set_status_retry_message(int attempt, int max_attempts, int seconds);

    /// pi `showLoadedResources`: replace the loaded-resources block (the
    /// startup container between the header and the chat). A no-resources
    /// data renders zero lines.
    void set_loaded_resources_data(LoadedResources::Data data);

    /// pi `clearStatusIndicator`: back to the two-row idle status.
    void clear_status_indicator();

    /// The login presentation's editor slot (pi's `editorContainer` swap):
    /// while a replacement is set it renders and receives input in place of
    /// the editor, exactly like pi's focused dialog/selector.
    void set_editor_replacement(std::shared_ptr<cch::tui::Component> component);
    void restore_editor();

    void append_user_bash_diagnostic(std::string text);
    void restore_submitted_text(const std::string& text);
    void clear_pending_bash(const EditorInterruptRequest& request);
    void insert_editor_text(std::string text);

    /// The raw editor text (pi `editor.getText()` — the tree navigation
    /// pre-fill and fork flows check emptiness before replacing it).
    [[nodiscard]] std::string editor_text() const;

    /// The expanded editor text (pi `editor.getExpandedText()` — the
    /// external-editor flow sends the expanded content like pi).
    [[nodiscard]] std::string editor_expanded_text() const;

    /// Replace the whole editor content (pi `editor.setText` — the fork
    /// flow's `selectedText` pre-fill).
    void set_editor_text(std::string text);

    void restore_queued_text(const std::vector<std::string>& messages);
    void set_pending_input(const agent::AgentInputQueues& queues);
    void set_user_bash_progress(runtime::UserBashProgress progress);
    void clear_user_bash_progress();

    /// Replaces the pending block with its committed transcript entry in one
    /// step, so the clear-pending-before-append ordering cannot drift apart
    /// at call sites.
    void commit_user_bash(ai::MessageVariant message);

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override;
    cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant& input) override;
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override;
    void set_available_height(std::size_t rows) override;

private:
    void record_callback_error(
        std::string message,
        std::string detail = {});

    /// Emit one closed main-screen action through the action seam. A returned
    /// failure is recorded in `callback_error_` and surfaces through the next
    /// render (the pre-seam observable behavior); the sink is `noexcept`, so
    /// this path never throws.
    void emit_action(ViewAction action, std::string_view failure_message);

    /// Fire the coalescible render request (the separate non-action path).
    void invoke_invalidate();

    /// Replace the active status indicator (pi `showStatusIndicator`
    /// disposes the previous one first); the loader's render requests flow
    /// through the view's invalidate sink.
    void replace_status_indicator(StatusIndicator::Kind kind, std::string message);
    void invoke_follow_up();
    void restore_editor_text(const std::vector<std::string>& messages);
    [[nodiscard]] bool unsubmitted_bash_mode() const;

    std::shared_ptr<SharedKeybindings> keybindings_;
    InvalidateSink on_invalidate_;
    ViewActionSink action_sink_;
    /// Footer data source (pi footer.ts render inputs); polled on every
    /// render by the view's footer container. Installed by the state; the
    /// source must not re-enter the view.
    std::move_only_function<FooterData()> footer_data_source_;
    bool user_bash_available_{false};
    std::optional<support::Error> callback_error_;
    mutable std::mutex mutex_;
    // pi's main-screen containers.
    KeybindingHints header_;
    /// The loaded-resources startup block (pi's `loadedResourcesContainer`,
    /// between the header and the chat; #418).
    LoadedResources resources_;
    ChatContainer chat_;
    Footer footer_;
    const LiveTheme* theme_; // must outlive the view: controller-owned live theme.
    cch::tui::Editor editor_;
    std::size_t editor_revision_{0};
    // The status container's active indicator (pi's statusContainer child);
    // null renders the two-row IdleStatus.
    std::unique_ptr<StatusIndicator> status_indicator_;
    IdleStatus idle_status_;
    // The latest footer data (polled from the data source during render);
    // also feeds the editor border thinking token.
    FooterData current_footer_data_;
    std::vector<std::string> pending_steering_;
    std::vector<std::string> pending_follow_up_;
    // The live pending User Bash block (pi's pendingMessagesContainer).
    std::unique_ptr<BashExecutionComponent> pending_bash_;
    // The login presentation's editor-slot occupant (pi's editorContainer
    // swap); null renders the ordinary editor.
    std::shared_ptr<cch::tui::Component> editor_replacement_;
    std::size_t last_bash_output_size_{0};
    bool bash_outcome_set_{false};
    std::size_t available_rows_{24};
    std::size_t editor_row_offset_{0};
};

} // namespace cch::coding_agent::tui
