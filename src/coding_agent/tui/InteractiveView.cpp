#include "coding_agent/tui/InteractiveView.hpp"

#include "coding_agent/tui/ChatViewport.hpp"

#include <cch/tui/Container.hpp>
#include <cch/tui/TruncatedText.hpp>
#include <cch/support/Error.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <memory>
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
      editor_(cch::tui::EditorOptions{
                      .keybindings = keybindings_->get(),
                      .keybinding_context = keybindings_->context(),
                      .autocomplete_debounce_timer = std::move(options.autocomplete_debounce_timer),
                      .render_request =
                              [this, upstream = std::move(options.render_request)]() mutable -> support::ExpectedVoid {
                          invoke_invalidate();
                          if (upstream) return upstream();
                          return {};
                      },
                      // pi CustomEditor: the default editor opts into the
                      // embedded working status (embedWorkingStatus: true);
                      // the border hook resolves the active indicator at
                      // render time. The editor is a member of this view, so
                      // the stored hook's `this` referent outlives every
                      // operation.
                      .top_border_override = [this](std::size_t width, std::size_t hidden_line_count)
                              -> support::Expected<std::optional<std::string>> {
                          return render_embedded_status_border(width, hidden_line_count);
                      },
              },
              make_editor_change_sink(),
              make_editor_submit_sink()) {
    chat_.set_hide_thinking_block(options.hide_thinking_block);
    chat_.set_output_pad(options.output_pad);
    autocomplete_provider_factory_ = std::move(options.autocomplete_provider_factory);
    editor_.set_autocomplete_provider(std::move(options.autocomplete_provider));
    fullscreen_ = options.fullscreen;
}

void InteractiveView::initialize(const AgentSessionSnapshot& snapshot) { chat_.initialize(snapshot); }

void InteractiveView::reconcile_snapshot(const AgentSessionSnapshot& snapshot) { chat_.reconcile_snapshot(snapshot); }

void InteractiveView::apply_render_settings(bool hide_thinking_block, std::size_t output_pad) {
    chat_.set_hide_thinking_block(hide_thinking_block);
    chat_.set_output_pad(output_pad);
}

void InteractiveView::set_autocomplete_provider(std::unique_ptr<cch::tui::AutocompleteProvider> provider) {
    // pi setupAutocompleteProvider: the default editor takes the new provider
    // and, when a custom editor is active, that editor receives its own.
    editor_.set_autocomplete_provider(std::move(provider));
    apply_custom_editor_autocomplete();
}

void InteractiveView::set_keybindings(std::shared_ptr<const cch::tui::KeybindingRegistry> registry,
        std::shared_ptr<const std::vector<HotkeyHelpRow>> help) {
    keybindings_->replace(registry, help);
    editor_.set_keybindings(std::move(registry));
}

void InteractiveView::apply_event(const agent::AgentLifecycleEvent& event) { chat_.apply_event(event); }

void InteractiveView::clear_transcript() { chat_.clear(); }

void InteractiveView::append_frontend_message(std::string text) { chat_.append_frontend_message(std::move(text)); }

void InteractiveView::append_diagnostic(std::string text) { chat_.append_diagnostic(std::move(text)); }

void InteractiveView::append_warning(std::string text) { chat_.append_warning(std::move(text)); }

void InteractiveView::append_trust_warning(std::string text) { chat_.append_trust_warning(std::move(text)); }

void InteractiveView::append_status_message(std::string text) { chat_.append_status_message(std::move(text)); }

void InteractiveView::show_status_working(std::string message) {
    // pi `setWorkingVisible`: an already-active Working indicator is
    // kept (per-message re-shows must not restart the loader).
    if (status_indicator_ != nullptr &&
        status_indicator_->kind() == StatusIndicator::Kind::Working) {
        return;
    }
    // pi showWorkingStatusIndicator: the embedded Working indicator colors
    // its spinner and message with the editor's border color (the live
    // thinking-level border hook; pi's colorFn closure reads the border at
    // render time).
    replace_status_indicator(StatusIndicator::Kind::Working,
            working_status_message(std::move(message)),
            cch::tui::TextStyleHook{[this](std::string text) -> std::string {
                // The editor is a member of this view; the hook fires during
                // render and its referent outlives the loader.
                return status_border_style()(std::move(text));
            }});
}

void InteractiveView::show_status_compaction(std::string_view reason) {
    replace_status_indicator(
        StatusIndicator::Kind::Compaction,
        compaction_status_message(keybindings_->registry(), reason));
}

void InteractiveView::show_status_retry(int attempt, int max_attempts, int seconds) {
    replace_status_indicator(
        StatusIndicator::Kind::Retry,
        retry_status_message(keybindings_->registry(), attempt, max_attempts, seconds));
}

void InteractiveView::set_status_retry_message(int attempt, int max_attempts, int seconds) {
    if (status_indicator_ == nullptr ||
        status_indicator_->kind() != StatusIndicator::Kind::Retry) {
        return;
    }
    status_indicator_->set_message(
        retry_status_message(keybindings_->registry(), attempt, max_attempts, seconds));
}

void InteractiveView::set_loaded_resources_data(LoadedResources::Data data) { resources_.set_data(std::move(data)); }

void InteractiveView::clear_status_indicator() { status_indicator_.reset(); }

void InteractiveView::replace_status_indicator(
        StatusIndicator::Kind kind, std::string message, cch::tui::TextStyleHook working_color) {
    status_indicator_ = std::make_unique<StatusIndicator>(
            kind,
            *theme_,
            [this]() -> support::ExpectedVoid {
                // The status indicator's loader render requests flow through the
                // view's separate coalescible invalidate sink (not the action
                // seam); a failing render request is a callback diagnostic.
                if (!on_invalidate_) return {};
                on_invalidate_();
                return {};
            },
            std::move(message),
            std::move(working_color));
}

support::Expected<std::optional<std::string>> InteractiveView::render_embedded_status_border(
        std::size_t width, std::size_t hidden_line_count) {
    // pi showStatusIndicator: the default editor embeds every indicator; an
    // editor-slot replacement (pi's non-opted custom editors) keeps the
    // standalone status container. The default border fallback (pi
    // super.renderTopBorder) stays inside the border composition (nullopt).
    if (status_indicator_ == nullptr || editor_replacement_ != nullptr || custom_editor_ != nullptr) {
        return std::nullopt;
    }
    auto border_style = status_border_style();
    return embedded_status_top_border(*status_indicator_, border_style, width, hidden_line_count);
}

cch::tui::TextStyleHook InteractiveView::status_border_style() {
    // pi updateEditorBorderColor: bash mode keeps the bashMode token,
    // otherwise the thinking-level border token (the same hook the editor
    // border renders with).
    return theme_->foreground_hook(current_border_token());
}

ThemeToken InteractiveView::current_border_token() const {
    return unsubmitted_bash_mode() ? ThemeToken::BashMode
                                   : thinking_border_token_for(current_footer_data_.thinking_level);
}

void InteractiveView::set_editor_replacement(std::shared_ptr<cch::tui::Component> component) {
    editor_replacement_ = std::move(component);
    if (editor_replacement_) {
        if (auto* focusable = dynamic_cast<cch::tui::Focusable*>(editor_replacement_.get())) {
            focusable->set_focused(active_editor().focused());
        }
    }
}

std::shared_ptr<cch::tui::Component> InteractiveView::editor_replacement() const { return editor_replacement_; }

void InteractiveView::restore_editor() { editor_replacement_.reset(); }

void InteractiveView::set_custom_editor(std::shared_ptr<cch::tui::EditorComponent> editor) {
    if (editor == nullptr) {
        // pi restore path: the default editor takes over the custom editor's
        // current text and focus, so restoring never loses an in-progress
        // prompt. A second restore is a no-op.
        if (custom_editor_ == nullptr) return;
        auto text = custom_editor_->text();
        const bool focused = custom_editor_->focused();
        custom_editor_.reset();
        presented_border_token_.reset();
        editor_.set_text(std::move(text));
        editor_.set_focused(focused);
        invoke_invalidate();
        return;
    }
    auto text = active_editor().text();
    const bool focused = active_editor().focused();
    // pi setCustomEditorComponent: the custom editor receives the default
    // editor's submit/change callbacks and current text. The built-in editor
    // keeps its own callbacks for the restore path; only the active editor
    // receives input, so a replaced editor never admits a second submission.
    editor->set_on_change(make_editor_change_sink());
    editor->set_on_submit(make_editor_submit_sink());
    custom_editor_ = std::move(editor);
    custom_editor_->set_text(std::move(text));
    custom_editor_->set_focused(focused);
    apply_editor_options();
    invoke_invalidate();
}

cch::tui::EditorChangeSink InteractiveView::make_editor_change_sink() {
    return [this](std::string text) -> support::ExpectedVoid {
        if (!text.empty()) ++editor_revision_;
        presented_bash_mode_ = user_bash_editor_mode(active_expanded_text(), user_bash_available_);
        refresh_custom_editor_border();
        invoke_invalidate();
        return {};
    };
}

cch::tui::EditorSubmitSink InteractiveView::make_editor_submit_sink() {
    return [this](std::string text) -> support::ExpectedVoid {
        add_active_history(text);
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
    };
}

std::string InteractiveView::active_expanded_text() const {
    const auto& editor = active_editor();
    if (const auto* expanded = dynamic_cast<const cch::tui::EditorExpandedTextSupport*>(&editor)) {
        return expanded->expanded_text();
    }
    return editor.text();
}

void InteractiveView::add_active_history(std::string text) {
    if (auto* history = dynamic_cast<cch::tui::EditorHistorySupport*>(&active_editor())) {
        history->add_to_history(std::move(text));
    }
}

void InteractiveView::set_active_text(std::string text) { active_editor().set_text(std::move(text)); }

void InteractiveView::apply_editor_options() {
    if (custom_editor_ == nullptr) return;
    if (auto* border = dynamic_cast<cch::tui::EditorBorderStyleSupport*>(custom_editor_.get())) {
        presented_border_token_ = current_border_token();
        border->set_border_style(status_border_style());
    }
    if (auto* padding = dynamic_cast<cch::tui::EditorPaddingSupport*>(custom_editor_.get())) {
        padding->set_padding_x(editor_.padding_x());
    }
    if (auto* limit = dynamic_cast<cch::tui::EditorAutocompleteMaxVisibleSupport*>(custom_editor_.get())) {
        limit->set_autocomplete_max_visible(editor_.autocomplete_max_visible());
    }
    apply_custom_editor_autocomplete();
}

void InteractiveView::refresh_custom_editor_border() {
    if (custom_editor_ == nullptr) return;
    const auto token = current_border_token();
    if (presented_border_token_ == token) return;
    if (auto* border = dynamic_cast<cch::tui::EditorBorderStyleSupport*>(custom_editor_.get())) {
        presented_border_token_ = token;
        border->set_border_style(status_border_style());
    }
}

void InteractiveView::apply_custom_editor_autocomplete() {
    if (custom_editor_ == nullptr || !autocomplete_provider_factory_) return;
    if (auto* autocomplete = dynamic_cast<cch::tui::EditorAutocompleteSupport*>(custom_editor_.get())) {
        autocomplete->set_autocomplete_provider(autocomplete_provider_factory_());
    }
}

void InteractiveView::append_user_bash_diagnostic(std::string text) {
    chat_.append_user_bash_diagnostic(std::move(text));
}

void InteractiveView::restore_submitted_text(const std::string& text) { restore_editor_text({text}); }

void InteractiveView::clear_pending_bash(const EditorInterruptRequest& request) {
    if (editor_revision_ == request.editor_revision) {
        set_active_text({});
        return;
    }
    set_active_text(editor_text_after_interrupt(request.pending_bash_text, active_expanded_text()));
}

void InteractiveView::insert_editor_text(std::string text) {
    // pi `insertTextAtCursor?.`: an editor without the capability ignores the
    // insertion instead of requiring inheritance from Editor.
    if (auto* insert = dynamic_cast<cch::tui::EditorInsertSupport*>(&active_editor())) {
        insert->insert_text_at_cursor(std::move(text));
    }
}

std::string InteractiveView::editor_text() const { return active_editor().text(); }

std::string InteractiveView::editor_expanded_text() const { return active_expanded_text(); }

void InteractiveView::set_editor_text(std::string text) { set_active_text(std::move(text)); }

void InteractiveView::restore_queued_text(const std::vector<std::string>& messages) { restore_editor_text(messages); }

void InteractiveView::set_pending_input(const agent::AgentInputQueues& queues) {
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
    if (!pending_bash_) {
        pending_bash_ = std::make_unique<BashExecutionComponent>(
            *theme_,
            keybindings_,
            progress.command,
            progress.exclude_from_context);
        pending_bash_->start_loader([this]() -> support::ExpectedVoid {
            if (!on_invalidate_) return {};
            on_invalidate_();
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
    pending_bash_.reset();
    last_bash_output_size_ = 0;
    bash_outcome_set_ = false;
}

namespace {

/// One fullscreen region whose content belongs to the view: a layout-root
/// child rendering through one of the view's region methods. It owns no view
/// state, so the document, pending, status and footer regions and the dock's
/// editor (the view itself) all describe the same instances the main screen
/// composes.
class ViewRegion final : public cch::tui::Component {
public:
    using Render = std::move_only_function<support::Expected<cch::tui::RenderResult>(std::size_t)>;

    explicit ViewRegion(Render render) : render_(std::move(render)) {}

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        return render_(width);
    }

    void invalidate() override {}

private:
    Render render_;
};

} // namespace

support::Expected<cch::tui::RenderResult> InteractiveView::render(std::size_t width) {
    if (callback_error_) return std::unexpected(*callback_error_);
    if (fullscreen_) {
        // The dock's editor entry of the layout root: the document, pending,
        // status and footer regions render through their own root components,
        // and the dock allocation bounds this region. The editor keeps its own
        // terminal bound (`set_fullscreen_terminal_rows`), pushed by the engine
        // before every fullscreen frame.
        editor_row_offset_ = 0;
        auto lines = render_editor_region(width, std::nullopt);
        if (!lines) return std::unexpected(lines.error());
        cch::tui::RenderResult result;
        result.lines = std::move(*lines);
        return result;
    }
    return render_main_screen(width);
}

support::Expected<std::vector<std::string>> InteractiveView::render_header_region(std::size_t width) {
    auto header = header_.render(width);
    if (!header) return std::unexpected(header.error());
    return std::move(header->lines);
}

support::Expected<std::vector<std::string>> InteractiveView::render_resources_region(
        std::size_t width, std::optional<std::size_t> budget) {
    auto resources = resources_.render(width);
    if (!resources) return std::unexpected(resources.error());
    auto lines = std::move(resources->lines);
    if (budget.has_value() && lines.size() > *budget) {
        lines.resize(*budget);
    }
    return lines;
}

support::Expected<cch::tui::RenderResult> InteractiveView::render_chat_region(
        std::size_t width, std::size_t preceding_rows) {
    auto rendered = chat_.render(width);
    if (!rendered) return std::unexpected(rendered.error());
    cch::tui::RenderResult chat = std::move(*rendered);
    for (auto& image : chat.images) {
        image.region.row += preceding_rows;
    }
    return chat;
}

support::Expected<cch::tui::RenderResult> InteractiveView::render_document_region(std::size_t width) {
    // Deliberately unbounded: the transcript viewport measures this component
    // without a height bound and paints the visible window, so bounding the
    // document to `available_rows_` here would destroy transcript rows the
    // viewport can scroll to. The header, the loaded-resources block and the
    // chat are all part of the scrollable document.
    auto header_lines = render_header_region(width);
    if (!header_lines) return std::unexpected(header_lines.error());
    auto resources_lines = render_resources_region(width, std::nullopt);
    if (!resources_lines) return std::unexpected(resources_lines.error());
    auto chat = render_chat_region(width, header_lines->size() + resources_lines->size());
    if (!chat) return std::unexpected(chat.error());

    cch::tui::RenderResult document;
    document.lines = std::move(*header_lines);
    document.lines.insert(document.lines.end(),
            std::make_move_iterator(resources_lines->begin()),
            std::make_move_iterator(resources_lines->end()));
    document.images = std::move(chat->images);
    document.lines.insert(document.lines.end(),
            std::make_move_iterator(chat->lines.begin()),
            std::make_move_iterator(chat->lines.end()));
    return document;
}

support::Expected<std::vector<std::string>> InteractiveView::render_pending_region(std::size_t width) {
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
    return pending_lines;
}

support::Expected<std::vector<std::string>> InteractiveView::render_status_region(std::size_t width) {
    // Status and footer containers are part of the composition: the status
    // container holds the active Working/Compaction/Retry indicator (two
    // rows: one spacer + the loader line, pi's Loader); the footer renders
    // pi's two-line layout from the live footer data source. While the
    // default editor renders, the active indicator embeds into the editor's
    // top border instead (pi v0.87.1 embedWorkingStatus) and the container
    // contributes zero rows; an editor-slot replacement keeps the standalone
    // indicator rows (pi's non-opted custom editors). At idle the container
    // is the two-row IdleStatus in Regular and empty in Fullscreen (the
    // fullscreen dock's blank row is the widget region's default spacer).
    //
    // The status region is the first region of either composition whose render
    // reads the footer data, and it precedes the editor in both the dock order
    // and the main-screen order, so polling here feeds the editor border and
    // the footer of the same frame.
    if (footer_data_source_) {
        current_footer_data_ = footer_data_source_();
    }
    std::vector<std::string> status_lines;
    if (status_indicator_ != nullptr && editor_replacement_ == nullptr && custom_editor_ == nullptr) {
        // Embedded: the spinner renders in the editor border (the sink at
        // editor render); the standalone block stays empty.
    } else if (status_indicator_ != nullptr) {
        if (auto rendered = status_indicator_->render(width); !rendered) {
            return std::unexpected(rendered.error());
        } else {
            status_lines = std::move(rendered->lines);
        }
    } else if (!fullscreen_) {
        // pi's IdleStatus: the two-row empty status container. It belongs to
        // the Regular composition. In Fullscreen the frozen status container
        // is empty at idle: pi re-adds IdleStatus only in Regular mode under
        // clear-on-shrink, so the blank row above the editor in the frozen
        // dock is the widget region's default `Spacer(1)` instead.
        auto idle = idle_status_.render(width);
        if (!idle) return std::unexpected(idle.error());
        status_lines = std::move(idle->lines);
    }
    return status_lines;
}

support::Expected<std::vector<std::string>> InteractiveView::render_footer_region(std::size_t width) {
    footer_.set_data(current_footer_data_);
    auto rendered = footer_.render(width);
    if (!rendered) return std::unexpected(rendered.error());
    return std::move(rendered->lines);
}

support::Expected<std::vector<std::string>> InteractiveView::render_editor_region(
        std::size_t width, std::optional<MainScreenEditorBudget> regular_budget) {
    std::vector<std::string> editor_lines;
    if (editor_replacement_) {
        // pi's editorContainer swap: the login dialog/selector renders in
        // the editor slot with no autocomplete rows.
        if (auto replaced = editor_replacement_->render(width); !replaced) {
            return std::unexpected(replaced.error());
        } else {
            editor_lines = std::move(replaced->lines);
        }
        if (regular_budget.has_value()) {
            // Height-bound the swap-in dialog (#607): pending/status/footer
            // keep their rows and the minimum chat slice stays visible, so an
            // oversized dialog is cropped from the top. Its input and selection
            // rows remain at the physical bottom instead of growing past the
            // terminal height. Regular-only: the fullscreen dock allocation
            // bounds the region instead.
            if (editor_lines.size() > regular_budget->replacement_rows) {
                editor_lines.erase(editor_lines.begin(), editor_lines.end() - regular_budget->replacement_rows);
            }
        }
    } else if (custom_editor_ != nullptr) {
        // pi updateEditorBorderColor: the custom editor's border follows the
        // live thinking-level / Bash-mode token, and the hook is rewritten
        // only when that token changes.
        refresh_custom_editor_border();
        // pi's custom editor renders in the editor slot with its own borders
        // and autocomplete rows; the built-in editor's dynamic theme hook and
        // embedded status border stay with the built-in editor.
        if (auto rendered = custom_editor_->render(width); !rendered) {
            return std::unexpected(rendered.error());
        } else {
            editor_lines = std::move(rendered->lines);
        }
    } else {
        if (regular_budget.has_value()) {
            // Main-screen budget (Regular-only): the editor's visible lines
            // yield to the fixed rows above and below it. Fullscreen leaves the
            // bound at the terminal rows the engine pushed
            // (`set_fullscreen_terminal_rows`).
            editor_.set_available_height(regular_budget->editor_rows);
        }
        // The editor enters Bash mode as soon as the trimmed input begins
        // with `!` (where User Bash dispatch is available). The border color
        // follows the same transition: bash mode uses the bashMode token,
        // otherwise the thinking-level token (pi `updateEditorBorderColor`
        // `getBashModeBorderColor` / `getThinkingBorderColor`). pi's
        // `EditorTheme` (pi-tui editor.ts) carries no text color, so the
        // editor text renders at the terminal default foreground in both
        // modes; only the border is themed.
        const auto thinking_border_token = thinking_border_token_for(
            current_footer_data_.thinking_level);
        cch::tui::EditorTheme editor_theme;
        editor_theme.select_list = theme_->select_list_theme();
        if (unsubmitted_bash_mode()) {
            editor_theme.border = theme_->foreground_hook(ThemeToken::BashMode);
        } else {
            editor_theme.border = theme_->foreground_hook(thinking_border_token);
        }
        presented_bash_mode_ = unsubmitted_bash_mode();
        editor_.set_theme(std::move(editor_theme));
        if (auto editor = editor_.render(width); !editor) {
            return std::unexpected(editor.error());
        } else {
            editor_lines = std::move(editor->lines);
        }
    }
    return editor_lines;
}

support::Expected<cch::tui::RenderResult> InteractiveView::render_main_screen(std::size_t width) {
    // Header (keybinding hints), the loaded-resources block, status, footer and
    // pending-messages render first so the editor and autocomplete capacities
    // account for their fixed rows below the chat.
    auto header_lines = render_header_region(width);
    if (!header_lines) return std::unexpected(header_lines.error());
    auto status_lines = render_status_region(width);
    if (!status_lines) return std::unexpected(status_lines.error());
    auto footer_lines = render_footer_region(width);
    if (!footer_lines) return std::unexpected(footer_lines.error());
    auto pending_lines = render_pending_region(width);
    if (!pending_lines) return std::unexpected(pending_lines.error());

    // Pending messages, status, editor, footer, and a minimum chat slice
    // stay visible on every terminal; the loaded-resources block yields the
    // space they need. pi's transcript scroll keeps these rows fixed while
    // the loaded-resources content scrolls above the chat, so
    // a huge startup diagnostic (e.g. an invalid `--theme` document's
    // full missing-color-token list, #425) must not push the footer and
    // editor off a small screen and freeze the boot view.
    //
    // This whole budgeting block belongs to the Regular main-screen path:
    // Fullscreen replaces it with the layout engine's allocation of the
    // two-region viewport, so nothing here runs for a fullscreen frame.
    constexpr std::size_t kMinEditorRows = 3;
    constexpr std::size_t kMinChatRows = 3;
    const auto fixed_budget = pending_lines->size() + status_lines->size() + kMinEditorRows + footer_lines->size();
    const auto top_budget =
            available_rows_ > fixed_budget + kMinChatRows ? available_rows_ - fixed_budget - kMinChatRows : 0;
    const auto resources_budget = top_budget > header_lines->size() ? top_budget - header_lines->size() : 0;
    auto resources_lines = render_resources_region(width, resources_budget);
    if (!resources_lines) return std::unexpected(resources_lines.error());

    const auto fixed_rows = header_lines->size() + resources_lines->size() + pending_lines->size() +
                            status_lines->size() + footer_lines->size();
    MainScreenEditorBudget editor_budget;
    editor_budget.editor_rows = available_rows_ > fixed_rows ? available_rows_ - fixed_rows : 0;
    const auto fixed_rest_rows = pending_lines->size() + status_lines->size() + footer_lines->size();
    editor_budget.replacement_rows =
            available_rows_ > fixed_rest_rows + kMinChatRows ? available_rows_ - fixed_rest_rows - kMinChatRows : 1;
    auto editor_lines = render_editor_region(width, editor_budget);
    if (!editor_lines) return std::unexpected(editor_lines.error());

    // The full conversation passes through to the terminal's native
    // scrollback (pi TuiMainScreen): pending/status/editor/autocomplete/
    // footer stay fixed on screen, and earlier chat lines scroll away into
    // the terminal's scroll history as the
    // conversation grows past one screen. Inline images keep their
    // absolute buffer rows so they scroll with the content that produced
    // them (fork-B image-follows-content).
    auto chat = render_chat_region(width, header_lines->size() + resources_lines->size());
    if (!chat) return std::unexpected(chat.error());

    cch::tui::RenderResult transcript_result;
    transcript_result.lines = std::move(*header_lines);
    transcript_result.lines.insert(transcript_result.lines.end(),
            std::make_move_iterator(resources_lines->begin()),
            std::make_move_iterator(resources_lines->end()));
    transcript_result.images = std::move(chat->images);
    transcript_result.lines.insert(transcript_result.lines.end(),
            std::make_move_iterator(chat->lines.begin()),
            std::make_move_iterator(chat->lines.end()));

    transcript_result.lines.insert(transcript_result.lines.end(),
            std::make_move_iterator(pending_lines->begin()),
            std::make_move_iterator(pending_lines->end()));
    transcript_result.lines.insert(transcript_result.lines.end(),
            std::make_move_iterator(status_lines->begin()),
            std::make_move_iterator(status_lines->end()));

    editor_row_offset_ = transcript_result.lines.size();

    transcript_result.lines.insert(transcript_result.lines.end(),
            std::make_move_iterator(editor_lines->begin()),
            std::make_move_iterator(editor_lines->end()));
    transcript_result.lines.insert(transcript_result.lines.end(),
            std::make_move_iterator(footer_lines->begin()),
            std::make_move_iterator(footer_lines->end()));

    return transcript_result;
}

support::Expected<ChatViewport> InteractiveView::make_fullscreen_root(
        std::unique_ptr<InteractiveView> view, cch::tui::ScrollbarMode scrollbar) {
    auto* const raw = view.get();
    ChatViewportOptions options;
    // The document is one region: header hints, the loaded-resources block and
    // the chat, all unbounded so the transcript viewport measures the whole
    // conversation.
    options.document =
            std::make_unique<ViewRegion>([raw](std::size_t width) { return raw->render_document_region(width); });
    // The dock regions render through the view's own instances, so the frame
    // layout and the main screen compose the same pending/status/footer state.
    auto line_region = [raw](auto region_method) {
        return std::make_unique<ViewRegion>(
                [raw, region_method](std::size_t width) -> support::Expected<cch::tui::RenderResult> {
                    auto lines = (raw->*region_method)(width);
                    if (!lines) return std::unexpected(lines.error());
                    cch::tui::RenderResult result;
                    result.lines = std::move(*lines);
                    return result;
                });
    };
    options.pending_messages = line_region(&InteractiveView::render_pending_region);
    options.status = line_region(&InteractiveView::render_status_region);
    options.footer = line_region(&InteractiveView::render_footer_region);
    // pi `renderWidgets()`: an empty widgets-above container holds a default
    // `Spacer(1)` (`spacerWhenEmpty: true`), so every frozen fullscreen dock
    // carries one blank row between the status and the editor. Pike has no
    // extension widget surface yet; the spacer is the region's only content.
    options.widgets_above = std::make_unique<cch::tui::Spacer>(1);
    // The dock's editor entry is the view itself: the engine focuses this
    // instance, and every editor operation (`set_autocomplete_provider`,
    // `initialize`/`reconcile_snapshot`, `set_pending_input`) reaches it
    // through the root.
    options.editor = std::move(view);
    options.scrollbar = scrollbar;
    // The frozen call site passes `theme.fg("scrollbarThumb", …)`. The theme
    // carries no `scrollbarTrack` token yet, so the track keeps the toolkit's
    // pi dim-gray default and only the thumb follows the live theme.
    options.scrollbar_thumb_style = [raw](std::string_view text) {
        return raw->theme_->foreground(ThemeToken::ScrollbarThumb, std::string{text});
    };

    auto viewport = create_chat_viewport(std::move(options));
    if (!viewport) return std::unexpected(viewport.error());
    return std::move(*viewport);
}

void InteractiveView::set_fullscreen_terminal_rows(std::size_t rows) {
    const auto bounded = std::max<std::size_t>(1, rows);
    // pi's editor reads `tui.terminal.rows` at render time and caps its visible
    // lines at `max(5, floor(rows * 0.3))`; the dock allocation is the
    // viewport bound. `available_rows_` stays the Regular budget and is never
    // rewritten here.
    editor_.set_terminal_rows(bounded);
    editor_.set_available_height(bounded);
    if (auto* viewport = dynamic_cast<cch::tui::ViewportAware*>(custom_editor_.get())) {
        viewport->set_available_height(bounded);
    }
}

void InteractiveView::invalidate() {
    editor_.invalidate();
    if (custom_editor_) custom_editor_->invalidate();
    if (editor_replacement_) editor_replacement_->invalidate();
}

cch::tui::InputAdmissionOutcome InteractiveView::handle_input(const cch::tui::InputEventVariant& input) {
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
        // shared slot, ADR 0035); `replace` is serialized on the
        // event loop.
        const auto& keys = keybindings_->registry();
        if (keys.matches(*key, "app.exit") && active_expanded_text().empty()) {
            emit_action(ExitAction{}, "Native TUI exit action failed");
            return cch::tui::InputAdmissionOutcome::Consumed;
        }
        if (keys.matches(*key, "app.interrupt")) {
            if (keys.matches(*key, "tui.select.cancel")) {
                if (active_editor().handle_input(input) == cch::tui::InputAdmissionOutcome::Consumed) {
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
                                    .pending_bash_text = active_expanded_text(),
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
            set_active_text({});
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
            emit_action(CopyLastMessageAction{.prefer_selection = true}, "Native TUI copy action failed");
            return cch::tui::InputAdmissionOutcome::Consumed;
        }
    }
    if (custom_editor_ != nullptr) {
        // A custom editor owns its input handling; the app-level bindings
        // above stay with the view (the C++ counterpart of pi's default
        // editor action handlers), and repaints flow through the change sink.
        return custom_editor_->handle_input(input);
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
    if (editor_replacement_) {
        if (auto* focusable = dynamic_cast<cch::tui::Focusable*>(editor_replacement_.get())) {
            focusable->set_focused(focused);
            return;
        }
    }
    active_editor().set_focused(focused);
}

bool InteractiveView::focused() const {
    if (editor_replacement_) {
        if (auto* focusable = dynamic_cast<cch::tui::Focusable*>(editor_replacement_.get())) {
            return focusable->focused();
        }
    }
    return active_editor().focused();
}

std::optional<cch::tui::CursorPosition> InteractiveView::cursor_location() const {
    std::optional<cch::tui::CursorPosition> cursor;
    if (editor_replacement_) {
        if (auto* focusable = dynamic_cast<cch::tui::Focusable*>(editor_replacement_.get())) {
            cursor = focusable->cursor_location();
        }
    } else {
        // The editor capability inherits `cursor_location()` from both
        // Component and Focusable; the Focusable contract is the IME
        // positioning surface the view consumes.
        cursor = static_cast<const cch::tui::Focusable&>(active_editor()).cursor_location();
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
    available_rows_ = std::max<std::size_t>(1, rows);
    editor_.set_terminal_rows(available_rows_);
    if (auto* viewport = dynamic_cast<cch::tui::ViewportAware*>(custom_editor_.get())) {
        viewport->set_available_height(available_rows_);
    }
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
    auto text = trim_editor_submission(active_expanded_text());
    if (text.empty()) return;
    // pi handleFollowUp: the accepted text enters editor history before
    // the editor clears and the follow-up admission is posted, matching
    // the Enter path where the editor records the submission itself.
    add_active_history(text);
    set_active_text({});
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
    auto current = active_expanded_text();
    if (!current.empty()) {
        if (!restored.empty()) restored += "\n\n";
        restored += current;
    }
    set_active_text(std::move(restored));
}

bool InteractiveView::unsubmitted_bash_mode() const {
    return user_bash_editor_mode(active_expanded_text(), user_bash_available_);
}

} // namespace cch::coding_agent::tui
