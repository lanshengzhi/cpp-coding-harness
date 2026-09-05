#include "coding_agent/tui/InteractiveView.hpp"

#include <cch/tui/TruncatedText.hpp>
#include <cch/support/Error.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {

namespace {
using interactive_view_detail::trim_editor_submission;
using interactive_view_detail::user_bash_editor_mode;
using interactive_view_detail::editor_text_after_interrupt;
using interactive_view_detail::thinking_border_token_for;
using interactive_view_detail::queued_editor_text;
} // namespace

InteractiveView::InteractiveView(InteractiveViewOptions options)
    : keybindings_(std::move(options.keybindings)), on_invalidate_(std::move(options.on_invalidate)),
      action_sink_(std::move(options.action_sink)), footer_data_source_(std::move(options.footer_data_source)),
      user_bash_available_(options.user_bash_available),
      header_(*options.theme, keybindings_, options.user_bash_available, options.clipboard_paste_available),
      resources_(*options.theme), chat_(*options.theme, keybindings_), footer_(*options.theme), theme_(options.theme),
      editor_(
              cch::tui::EditorOptions{
                      .keybindings = keybindings_->get(),
                      .autocomplete_debounce_timer = std::move(options.autocomplete_debounce_timer),
                      .render_request =
                              [this, upstream = std::move(options.render_request)]() mutable -> support::ExpectedVoid {
                          invoke_invalidate();
                          if (upstream) return upstream();
                          return {};
                      },
              },
              [this](std::string text) -> support::ExpectedVoid {
                  // Editor submission clears before invoking its submit sink;
                  // keep that notification on the sampled text's revision.
                  if (!text.empty()) ++editor_revision_;
                  invoke_invalidate();
                  return {};
              },
              [this](std::string text) -> support::ExpectedVoid {
                  emit_action(
                          SubmitAction{
                                  .request =
                                          EditorSubmissionRequest{
                                                  .text = std::move(text),
                                                  .editor_revision = editor_revision_,
                                          },
                                  .submission = InputSubmission::Ordinary,
                          },
                          "Native TUI submit action failed");
                  return {};
              }) {
    chat_.set_hide_thinking_block(options.hide_thinking_block);
    chat_.set_output_pad(options.output_pad);
    editor_.set_autocomplete_provider(std::move(options.autocomplete_provider));
}

void InteractiveView::initialize(const AgentSessionSnapshot& snapshot) {
    std::lock_guard lock(mutex_);
    chat_.initialize(snapshot);
}

void InteractiveView::apply_render_settings(bool hide_thinking_block, std::size_t output_pad) {
    std::lock_guard lock(mutex_);
    chat_.set_hide_thinking_block(hide_thinking_block);
    chat_.set_output_pad(output_pad);
}

void InteractiveView::set_autocomplete_provider(
    std::unique_ptr<cch::tui::AutocompleteProvider> provider) {
    std::lock_guard lock(mutex_);
    editor_.set_autocomplete_provider(std::move(provider));
}

void InteractiveView::set_keybindings(
    std::shared_ptr<const cch::tui::KeybindingRegistry> registry) {
    std::lock_guard lock(mutex_);
    keybindings_->replace(registry);
    editor_.set_keybindings(std::move(registry));
}

void InteractiveView::apply_event(const agent::AgentLifecycleEvent& event) {
    std::lock_guard lock(mutex_);
    chat_.apply_event(event);
}

void InteractiveView::append_committed_message(ai::MessageVariant message) {
    std::lock_guard lock(mutex_);
    chat_.append_committed_message(std::move(message));
}

void InteractiveView::clear_transcript() {
    std::lock_guard lock(mutex_);
    chat_.clear();
}

void InteractiveView::append_frontend_message(std::string text) {
    std::lock_guard lock(mutex_);
    chat_.append_frontend_message(std::move(text));
}

void InteractiveView::append_diagnostic(std::string text) {
    std::lock_guard lock(mutex_);
    chat_.append_diagnostic(std::move(text));
}

void InteractiveView::append_warning(std::string text) {
    std::lock_guard lock(mutex_);
    chat_.append_warning(std::move(text));
}

void InteractiveView::append_trust_warning(std::string text) {
    std::lock_guard lock(mutex_);
    chat_.append_trust_warning(std::move(text));
}

void InteractiveView::append_status_message(std::string text) {
    std::lock_guard lock(mutex_);
    chat_.append_status_message(std::move(text));
}

void InteractiveView::show_status_working(std::string message) {
    std::lock_guard lock(mutex_);
    // pi `setWorkingVisible`: an already-active Working indicator is
    // kept (per-message re-shows must not restart the loader).
    if (status_indicator_ != nullptr &&
        status_indicator_->kind() == StatusIndicator::Kind::Working) {
        return;
    }
    replace_status_indicator(
        StatusIndicator::Kind::Working,
        working_status_message(std::move(message)));
}

void InteractiveView::show_status_compaction(std::string_view reason) {
    std::lock_guard lock(mutex_);
    replace_status_indicator(
        StatusIndicator::Kind::Compaction,
        compaction_status_message(keybindings_->registry(), reason));
}

void InteractiveView::show_status_retry(int attempt, int max_attempts, int seconds) {
    std::lock_guard lock(mutex_);
    replace_status_indicator(
        StatusIndicator::Kind::Retry,
        retry_status_message(keybindings_->registry(), attempt, max_attempts, seconds));
}

void InteractiveView::set_status_retry_message(int attempt, int max_attempts, int seconds) {
    std::lock_guard lock(mutex_);
    if (status_indicator_ == nullptr ||
        status_indicator_->kind() != StatusIndicator::Kind::Retry) {
        return;
    }
    status_indicator_->set_message(
        retry_status_message(keybindings_->registry(), attempt, max_attempts, seconds));
}

void InteractiveView::set_loaded_resources_data(LoadedResources::Data data) {
    std::lock_guard lock(mutex_);
    resources_.set_data(std::move(data));
}

void InteractiveView::clear_status_indicator() {
    std::lock_guard lock(mutex_);
    status_indicator_.reset();
}

void InteractiveView::replace_status_indicator(StatusIndicator::Kind kind, std::string message) {
    status_indicator_ = std::make_unique<StatusIndicator>(
        kind,
        *theme_,
        [this]() -> support::ExpectedVoid {
            // The status indicator's loader render requests flow through the
            // view's separate coalescible invalidate sink (not the action
            // seam); a failing render request is a callback diagnostic.
            if (!on_invalidate_) return {};
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
            try {
#endif
                on_invalidate_();
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
            } catch (...) {
                record_callback_error(
                    "Native TUI status indicator render request failed");
            }
#endif
            return {};
        },
        std::move(message));
}

void InteractiveView::set_editor_replacement(std::shared_ptr<cch::tui::Component> component) {
    std::lock_guard lock(mutex_);
    editor_replacement_ = std::move(component);
    if (editor_replacement_) {
        if (auto* focusable = dynamic_cast<cch::tui::Focusable*>(editor_replacement_.get())) {
            focusable->set_focused(editor_.focused());
        }
    }
}

void InteractiveView::restore_editor() {
    std::lock_guard lock(mutex_);
    editor_replacement_.reset();
}

void InteractiveView::append_user_bash_diagnostic(std::string text) {
    std::lock_guard lock(mutex_);
    chat_.append_user_bash_diagnostic(std::move(text));
}

void InteractiveView::restore_submitted_text(const std::string& text) {
    std::lock_guard lock(mutex_);
    restore_editor_text({text});
}

void InteractiveView::clear_pending_bash(const EditorInterruptRequest& request) {
    std::lock_guard lock(mutex_);
    if (editor_revision_ == request.editor_revision) {
        editor_.set_text({});
        return;
    }
    editor_.set_text(editor_text_after_interrupt(
        request.pending_bash_text,
        editor_.expanded_text()));
}

void InteractiveView::insert_editor_text(std::string text) {
    std::lock_guard lock(mutex_);
    editor_.insert_text_at_cursor(std::move(text));
}

std::string InteractiveView::editor_text() const {
    std::lock_guard lock(mutex_);
    return editor_.text();
}

std::string InteractiveView::editor_expanded_text() const {
    std::lock_guard lock(mutex_);
    return editor_.expanded_text();
}

void InteractiveView::set_editor_text(std::string text) {
    std::lock_guard lock(mutex_);
    editor_.set_text(std::move(text));
}

void InteractiveView::restore_queued_text(const std::vector<std::string>& messages) {
    std::lock_guard lock(mutex_);
    restore_editor_text(messages);
}

void InteractiveView::set_pending_input(const agent::AgentInputQueues& queues) {
    std::lock_guard lock(mutex_);
    pending_steering_.clear();
    pending_follow_up_.clear();
    for (const auto& message : queues.steering.messages) {
        pending_steering_.push_back(
            queued_editor_text(message).value_or("[unsupported queued input]"));
    }
    for (const auto& message : queues.follow_up.messages) {
        pending_follow_up_.push_back(
            queued_editor_text(message).value_or("[unsupported queued input]"));
    }
}

void InteractiveView::set_user_bash_progress(runtime::UserBashProgress progress) {
    std::lock_guard lock(mutex_);
    if (!pending_bash_) {
        pending_bash_ = std::make_unique<BashExecutionComponent>(
            *theme_,
            keybindings_,
            progress.command,
            progress.exclude_from_context);
        pending_bash_->start_loader([this]() -> support::ExpectedVoid {
            if (!on_invalidate_) return {};
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
            try {
#endif
                on_invalidate_();
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
            } catch (...) {
            }
#endif
            return {};
        });
        last_bash_output_size_ = 0;
        bash_outcome_set_ = false;
    }
    if (progress.output.size() > last_bash_output_size_) {
        pending_bash_->append_output(progress.output.substr(last_bash_output_size_));
    }
    last_bash_output_size_ = progress.output.size();
    if (progress.awaiting_commitment && !bash_outcome_set_) {
        bash_outcome_set_ = true;
        pending_bash_->set_complete(
            progress.exit_code,
            progress.cancelled,
            progress.truncated,
            progress.full_output_path);
    }
    pending_bash_->set_expanded(chat_.tools_expanded());
}

void InteractiveView::clear_user_bash_progress() {
    std::lock_guard lock(mutex_);
    pending_bash_.reset();
    last_bash_output_size_ = 0;
    bash_outcome_set_ = false;
}

void InteractiveView::commit_user_bash(ai::MessageVariant message) {
    std::lock_guard lock(mutex_);
    pending_bash_.reset();
    last_bash_output_size_ = 0;
    bash_outcome_set_ = false;
    chat_.append_committed_message(std::move(message));
}

support::Expected<cch::tui::RenderResult> InteractiveView::render(std::size_t width) {
    std::lock_guard lock(mutex_);
    if (callback_error_) return std::unexpected(*callback_error_);

    // Header (keybinding hints), the loaded-resources block, pending-
    // messages, status, and footer render first so the editor and
    // autocomplete capacities account for their fixed rows (pi's dock
    // below the chat).
    std::vector<std::string> header_lines;
    if (auto header = header_.render(width); !header) {
        return std::unexpected(header.error());
    } else {
        header_lines = std::move(header->lines);
    }
    std::vector<std::string> resources_lines;
    if (auto resources = resources_.render(width); !resources) {
        return std::unexpected(resources.error());
    } else {
        resources_lines = std::move(resources->lines);
    }
    // Status and footer containers are part of the composition: the
    // status container holds the active Working/Compaction/Retry
    // indicator (two rows: one spacer + the loader line, pi's Loader) or
    // the two-row IdleStatus; the footer renders pi's two-line layout
    // from the live footer data source.
    std::vector<std::string> status_lines;
    std::vector<std::string> footer_lines;
    if (footer_data_source_) {
        current_footer_data_ = footer_data_source_();
    }
    if (status_indicator_) {
        if (auto rendered = status_indicator_->render(width); !rendered) {
            return std::unexpected(rendered.error());
        } else {
            status_lines = std::move(rendered->lines);
        }
    } else {
        // pi's IdleStatus: the two-row empty status container.
        auto idle = idle_status_.render(width);
        if (!idle) return std::unexpected(idle.error());
        status_lines = std::move(idle->lines);
    }
    footer_.set_data(current_footer_data_);
    if (auto rendered = footer_.render(width); !rendered) {
        return std::unexpected(rendered.error());
    } else {
        footer_lines = std::move(rendered->lines);
    }

    std::vector<std::string> pending_lines;
    pending_lines.reserve(pending_steering_.size() + pending_follow_up_.size() + 3);
    if (pending_bash_) {
        // One Bash block while it is pending; it becomes an ordinary chat
        // entry through the same component after commitment.
        pending_bash_->set_expanded(chat_.tools_expanded());
        auto block = pending_bash_->render(width);
        if (!block) return std::unexpected(block.error());
        pending_lines.insert(
            pending_lines.end(),
            std::make_move_iterator(block->lines.begin()),
            std::make_move_iterator(block->lines.end()));
    }
    for (const auto& message : pending_steering_) {
        cch::tui::TruncatedText item{"Steering: " + message};
        if (auto rendered = item.render(width); !rendered) {
            return std::unexpected(rendered.error());
        } else if (!rendered->lines.empty()) {
            pending_lines.push_back(std::move(rendered->lines.front()));
        }
    }
    for (const auto& message : pending_follow_up_) {
        cch::tui::TruncatedText item{"Follow-up: " + message};
        if (auto rendered = item.render(width); !rendered) {
            return std::unexpected(rendered.error());
        } else if (!rendered->lines.empty()) {
            pending_lines.push_back(std::move(rendered->lines.front()));
        }
    }
    if (!pending_lines.empty()) {
        const auto hint = keybindings_->registry().key_text("app.message.dequeue");
        cch::tui::TruncatedText item{std::format(
            "↳ {} to edit all queued messages",
            hint.empty() ? "Unbound" : format_key_text(hint, true))};
        if (auto rendered = item.render(width); !rendered) {
            return std::unexpected(rendered.error());
        } else if (!rendered->lines.empty()) {
            pending_lines.push_back(std::move(rendered->lines.front()));
        }
    }
    // The dock (pending + status + editor + footer) and a minimum chat
    // slice stay visible on every terminal; the loaded-resources block
    // yields the space it needs. pi's transcript scroll keeps the dock
    // fixed while the loaded-resources content scrolls above the chat, so
    // a huge startup diagnostic (e.g. an invalid `--theme` document's
    // full missing-color-token list, #425) must not push the footer and
    // editor off a small screen and freeze the boot view.
    constexpr std::size_t kMinDockEditorRows = 3;
    constexpr std::size_t kMinChatRows = 3;
    const auto dock_budget = pending_lines.size() + status_lines.size() +
        kMinDockEditorRows + footer_lines.size();
    const auto top_budget = available_rows_ > dock_budget + kMinChatRows
        ? available_rows_ - dock_budget - kMinChatRows
        : 0;
    const auto resources_budget =
        top_budget > header_lines.size() ? top_budget - header_lines.size() : 0;
    if (resources_lines.size() > resources_budget) {
        resources_lines.resize(resources_budget);
    }
    const auto fixed_rows =
        header_lines.size() + resources_lines.size() + pending_lines.size() +
        status_lines.size() + footer_lines.size();

    std::vector<std::string> editor_lines;
    if (editor_replacement_) {
        // pi's editorContainer swap: the login dialog/selector renders in
        // the editor slot with no autocomplete rows.
        if (auto replaced = editor_replacement_->render(width); !replaced) {
            return std::unexpected(replaced.error());
        } else {
            editor_lines = std::move(replaced->lines);
        }
    } else {
        editor_.set_available_height(available_rows_ > fixed_rows ? available_rows_ - fixed_rows : 0);
        // The editor enters Bash mode as soon as the trimmed input begins
        // with `!` (where User Bash dispatch is available). The border color
        // follows the same transition: bash mode uses the bashMode token,
        // otherwise the thinking-level token (pi `updateEditorBorderColor`
        // `getBashModeBorderColor` / `getThinkingBorderColor`).
        const auto thinking_border_token = thinking_border_token_for(
            current_footer_data_.thinking_level);
        cch::tui::EditorTheme editor_theme;
        if (unsubmitted_bash_mode()) {
            editor_theme.text = theme_->foreground_hook(ThemeToken::BashMode);
            editor_theme.border = theme_->foreground_hook(ThemeToken::BashMode);
        } else {
            editor_theme.text = theme_->editor_theme().text;
            editor_theme.border = theme_->foreground_hook(thinking_border_token);
        }
        editor_.set_theme(std::move(editor_theme));
        if (auto editor = editor_.render(width); !editor) {
            return std::unexpected(editor.error());
        } else {
            editor_lines = std::move(editor->lines);
        }
    }
    // The full conversation passes through to the terminal's native
    // scrollback (pi TuiMainScreen): only the dock (pending/status/
    // editor/autocomplete/footer) stays fixed on screen, and earlier chat
    // lines scroll away into the terminal's scroll history as the
    // conversation grows past one screen. Inline images keep their
    // absolute buffer rows so they scroll with the content that produced
    // them (fork-B image-follows-content).
    cch::tui::RenderResult chat_result;
    if (auto rendered = chat_.render(width); !rendered) {
        return std::unexpected(rendered.error());
    } else {
        chat_result = std::move(*rendered);
    }

    cch::tui::RenderResult transcript_result;
    transcript_result.lines = std::move(header_lines);
    transcript_result.lines.insert(
        transcript_result.lines.end(),
        std::make_move_iterator(resources_lines.begin()),
        std::make_move_iterator(resources_lines.end()));
    for (auto& image : chat_result.images) {
        image.region.row += transcript_result.lines.size();
        transcript_result.images.push_back(std::move(image));
    }
    transcript_result.lines.insert(
        transcript_result.lines.end(),
        std::make_move_iterator(chat_result.lines.begin()),
        std::make_move_iterator(chat_result.lines.end()));
    transcript_result.lines.insert(
        transcript_result.lines.end(),
        std::make_move_iterator(pending_lines.begin()),
        std::make_move_iterator(pending_lines.end()));
    transcript_result.lines.insert(
        transcript_result.lines.end(),
        std::make_move_iterator(status_lines.begin()),
        std::make_move_iterator(status_lines.end()));
    editor_row_offset_ = transcript_result.lines.size();
    transcript_result.lines.insert(transcript_result.lines.end(),
            std::make_move_iterator(editor_lines.begin()),
            std::make_move_iterator(editor_lines.end()));
    transcript_result.lines.insert(
        transcript_result.lines.end(),
        std::make_move_iterator(footer_lines.begin()),
        std::make_move_iterator(footer_lines.end()));
    return transcript_result;
}

void InteractiveView::invalidate() {
    std::lock_guard lock(mutex_);
    editor_.invalidate();
    if (editor_replacement_) editor_replacement_->invalidate();
}

cch::tui::InputAdmissionOutcome InteractiveView::handle_input(const cch::tui::InputEventVariant& input) {
    std::lock_guard lock(mutex_);
    if (editor_replacement_) {
        // pi routes every key to the focused dialog/selector; app-level
        // bindings resume when the editor is restored. pi's TUI
        // re-renders after each input event, so the view invalidates.
        if (auto* handler = dynamic_cast<cch::tui::InputHandler*>(editor_replacement_.get())) {
            static_cast<void>(handler->handle_input(input));
            invoke_invalidate();
        }
        return cch::tui::InputAdmissionOutcome::Consumed;
    }
    const auto* key = std::get_if<cch::tui::KeyEvent>(&input);
    if (key != nullptr && key->type != cch::tui::KeyEventType::Release) {
        // One registry reference for the whole dispatch cascade (the
        // shared slot, ADR 0035); `replace` is serialized under this
        // view mutex.
        const auto& keys = keybindings_->registry();
        if (keys.matches(*key, "app.exit") && editor_.expanded_text().empty()) {
            emit_action(ExitAction{}, "Native TUI exit action failed");
            return cch::tui::InputAdmissionOutcome::Consumed;
        }
        if (keys.matches(*key, "app.interrupt")) {
            if (keys.matches(*key, "tui.select.cancel")) {
                if (editor_.handle_input(input) == cch::tui::InputAdmissionOutcome::Consumed) {
                    return cch::tui::InputAdmissionOutcome::Consumed;
                }
            }
            // Autocomplete cancellation stays in the view. Interrupt
            // precedence is pi's onEscape chain and owns every later
            // decision; it receives Bash mode as it existed at key-press
            // time.
            emit_action(
                InterruptAction{
                    EditorInterruptRequest{
                        .pending_bash_text = editor_.expanded_text(),
                        .editor_revision = editor_revision_,
                        .pending_bash = unsubmitted_bash_mode(),
                    },
                },
                "Native TUI interrupt action failed");
            return cch::tui::InputAdmissionOutcome::Consumed;
        }
        if (keys.matches(*key, "app.message.followUp")) {
            invoke_follow_up();
            return cch::tui::InputAdmissionOutcome::Consumed;
        }
        if (keys.matches(*key, "app.clipboard.pasteImage")) {
            emit_action(ClipboardPasteAction{}, "Native TUI clipboard action failed");
            return cch::tui::InputAdmissionOutcome::Consumed;
        }
        if (keys.matches(*key, "app.message.dequeue")) {
            emit_action(DequeueAction{}, "Native TUI dequeue action failed");
            return cch::tui::InputAdmissionOutcome::Consumed;
        }
        if (keys.matches(*key, "app.clear")) {
            // Toolkit-only editor state: no application action is emitted.
            editor_.set_text({});
            return cch::tui::InputAdmissionOutcome::Consumed;
        }
        if (keys.matches(*key, "app.suspend")) {
            emit_action(SuspendAction{}, "Native TUI suspend action failed");
            return cch::tui::InputAdmissionOutcome::Consumed;
        }
        if (keys.matches(*key, "app.editor.external")) {
            emit_action(ExternalEditorAction{}, "Native TUI external editor action failed");
            return cch::tui::InputAdmissionOutcome::Consumed;
        }
        if (keys.matches(*key, "app.tools.expand")) {
            chat_.toggle_tool_output();
            header_.set_expanded(chat_.tools_expanded());
            resources_.set_expanded(chat_.tools_expanded());
            invoke_invalidate();
            return cch::tui::InputAdmissionOutcome::Consumed;
        }
        if (keys.matches(*key, "app.thinking.toggle")) {
            emit_action(ToggleThinkingAction{}, "Native TUI thinking toggle action failed");
            return cch::tui::InputAdmissionOutcome::Consumed;
        }
        // pi's main-editor `app.model.*` / `app.thinking.cycle` bindings:
        // the cycle actions and the model selector post to the executor
        // like every session-touching action.
        if (keys.matches(*key, "app.model.cycleForward")) {
            emit_action(
                CycleModelAction{ModelCycleDirection::Forward},
                "Native TUI model cycle action failed");
            return cch::tui::InputAdmissionOutcome::Consumed;
        }
        if (keys.matches(*key, "app.model.cycleBackward")) {
            emit_action(
                CycleModelAction{ModelCycleDirection::Backward},
                "Native TUI model cycle action failed");
            return cch::tui::InputAdmissionOutcome::Consumed;
        }
        if (keys.matches(*key, "app.model.select")) {
            emit_action(SelectModelAction{}, "Native TUI model selector action failed");
            return cch::tui::InputAdmissionOutcome::Consumed;
        }
        if (keys.matches(*key, "app.thinking.cycle")) {
            emit_action(CycleThinkingAction{}, "Native TUI thinking cycle action failed");
            return cch::tui::InputAdmissionOutcome::Consumed;
        }
        // pi `app.session.*`: recognized-but-unbound actions (defaultKeys
        // []) — a user-assigned keybinding triggers the flow; the
        // selector-scoped `app.session.*` bindings are matched inside the
        // SessionSelectorComponent itself.
        if (keys.matches(*key, "app.session.resume")) {
            emit_action(ResumeSessionAction{}, "Native TUI session resume action failed");
            return cch::tui::InputAdmissionOutcome::Consumed;
        }
        if (keys.matches(*key, "app.session.fork")) {
            emit_action(ForkSessionAction{}, "Native TUI session fork action failed");
            return cch::tui::InputAdmissionOutcome::Consumed;
        }
        if (keys.matches(*key, "app.session.new")) {
            emit_action(NewSessionAction{}, "Native TUI new-session action failed");
            return cch::tui::InputAdmissionOutcome::Consumed;
        }
        if (keys.matches(*key, "app.session.tree")) {
            emit_action(OpenTreeSelectorAction{}, "Native TUI tree selector action failed");
            return cch::tui::InputAdmissionOutcome::Consumed;
        }
        // pi's main-editor `app.message.copy` binding (P14: the tree
        // selector matches the same action through the shared registry).
        if (keys.matches(*key, "app.message.copy")) {
            emit_action(CopyLastMessageAction{}, "Native TUI copy action failed");
            return cch::tui::InputAdmissionOutcome::Consumed;
        }
    }
    const auto before_cursor = editor_.cursor();
    const auto before_text = editor_.text();
    const auto outcome = editor_.handle_input(input);
    // Text changes already notify through the editor's change sink. Cursor-only
    // navigation has no change notification, but it still changes the visible
    // fake/hardware cursor and must schedule the same repaint as pi.
    if (outcome == cch::tui::InputAdmissionOutcome::Consumed && before_text == editor_.text() &&
            before_cursor != editor_.cursor()) {
        invoke_invalidate();
    }
    return outcome;
}

void InteractiveView::set_focused(bool focused) {
    std::lock_guard lock(mutex_);
    if (editor_replacement_) {
        if (auto* focusable = dynamic_cast<cch::tui::Focusable*>(editor_replacement_.get())) {
            focusable->set_focused(focused);
            return;
        }
    }
    editor_.set_focused(focused);
}

bool InteractiveView::focused() const {
    std::lock_guard lock(mutex_);
    if (editor_replacement_) {
        if (auto* focusable = dynamic_cast<cch::tui::Focusable*>(editor_replacement_.get())) {
            return focusable->focused();
        }
    }
    return editor_.focused();
}

std::optional<cch::tui::CursorPosition> InteractiveView::cursor_location() const {
    std::lock_guard lock(mutex_);
    std::optional<cch::tui::CursorPosition> cursor;
    if (editor_replacement_) {
        if (auto* focusable = dynamic_cast<cch::tui::Focusable*>(editor_replacement_.get())) {
            cursor = focusable->cursor_location();
        }
    } else {
        cursor = editor_.cursor_location();
    }
    if (cursor) {
        // Return the true buffer-relative row; Tui::render() clamps it to
        // the visible viewport when positioning the hardware cursor (pi
        // positionHardwareCursor tracks the scroll rather than scrolling
        // the terminal).
        cursor->row += editor_row_offset_;
    }
    return cursor;
}

void InteractiveView::set_available_height(std::size_t rows) {
    std::lock_guard lock(mutex_);
    available_rows_ = std::max<std::size_t>(1, rows);
}

void InteractiveView::record_callback_error(
    std::string message,
    std::string detail) {
    callback_error_ = support::make_error(
        support::ErrorCode::Unknown,
        std::move(message),
        std::move(detail));
}

void InteractiveView::emit_action(ViewAction action, std::string_view failure_message) {
    if (!action_sink_) return;
    if (auto result = action_sink_(std::move(action)); !result) {
        record_callback_error(std::string(failure_message), result.error().message);
    }
}

void InteractiveView::invoke_invalidate() {
    if (!on_invalidate_) return;
    on_invalidate_();
}

void InteractiveView::invoke_follow_up() {
    auto text = trim_editor_submission(editor_.expanded_text());
    if (text.empty()) return;
    // pi handleFollowUp: the accepted text enters editor history before
    // the editor clears and the follow-up admission is posted, matching
    // the Enter path where the editor records the submission itself.
    editor_.add_to_history(text);
    editor_.set_text({});
    emit_action(
        SubmitAction{
            .request = EditorSubmissionRequest{
                .text = std::move(text),
                .editor_revision = editor_revision_,
            },
            .submission = InputSubmission::FollowUp,
        },
        "Native TUI follow-up action failed");
}

void InteractiveView::restore_editor_text(const std::vector<std::string>& messages) {
    std::string restored;
    for (const auto& message : messages) {
        if (message.empty()) continue;
        if (!restored.empty()) restored += "\n\n";
        restored += message;
    }
    auto current = editor_.expanded_text();
    if (!current.empty()) {
        if (!restored.empty()) restored += "\n\n";
        restored += current;
    }
    editor_.set_text(std::move(restored));
}

bool InteractiveView::unsubmitted_bash_mode() const {
    return user_bash_editor_mode(editor_.expanded_text(), user_bash_available_);
}

} // namespace cch::coding_agent::tui
