#include "InteractiveMode.hpp"

#include <cch/agent/AgentEvent.hpp>
#include <cch/coding_agent/Sdk.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/tui/Editor.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/Terminal.hpp>
#include <cch/tui/TruncatedText.hpp>
#include <cch/tui/Tui.hpp>

#include "coding_agent/BoundedText.hpp"
#include "coding_agent/CommandRegistry.hpp"
#include "coding_agent/prompt/SlashCommandParser.hpp"
#include "coding_agent/tui/KeybindingCatalog.hpp"
#include "coding_agent/tui/KeybindingHelp.hpp"
#include "coding_agent/tui/ThemeCatalog.hpp"
#include "coding_agent/tui/Transcript.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

using ActionSink = std::move_only_function<void()>;
using SubmitSink = std::move_only_function<void(std::string)>;
using PromptActiveHook = std::move_only_function<bool()>;

struct InteractiveStartupDiagnostics {
    std::vector<KeybindingDiagnostic> keybindings;
    std::vector<ThemeDiagnostic> themes;
};

[[nodiscard]] std::string combined_error_text(const util::Error& error) {
    std::string text = error.message;
    if (!error.detail.empty() && error.detail != error.message) {
        text = std::format("{}: {}", text, error.detail);
    }
    if (error.context && !error.context->empty()) {
        text = std::format("{} ({})", text, *error.context);
    }
    return text;
}

[[nodiscard]] util::Error presentation_error(
    const util::Error& error,
    std::string message) {
    return util::make_error(
        error.code,
        std::move(message),
        bounded_redacted_presentation(combined_error_text(error)));
}

[[nodiscard]] util::Error startup_error(const util::Error& error) {
    return presentation_error(error, "Native TUI startup failed");
}

[[nodiscard]] util::Error aggregate_presentation_errors(
    const util::Error& primary,
    const util::Error& restoration,
    std::string message) {
    return util::make_error(
        primary.code,
        std::move(message),
        bounded_redacted_presentation(std::format(
            "primary: {}; restoration: {}",
            combined_error_text(primary),
            combined_error_text(restoration))));
}

[[nodiscard]] std::vector<cch::tui::AutocompleteItem> command_autocomplete_items(
    const CommandRegistry& commands,
    std::span<const PromptTemplate> prompt_templates,
    std::span<const Skill> skills) {
    std::vector<cch::tui::AutocompleteItem> items;
    std::set<std::string, std::less<>> names;
    for (const auto& command : commands.list_commands()) {
        std::string description = command.description;
        if (!command.argument_hint.empty()) {
            description = description.empty()
                ? command.argument_hint
                : std::format("{} — {}", command.argument_hint, description);
        }
        items.push_back({
            .value = command.name,
            .label = command.name,
            .description = std::move(description),
        });
        names.insert(command.name);
    }
    for (const auto& prompt_template : prompt_templates) {
        if (!names.insert(prompt_template.name).second) continue;
        std::string description = prompt_template.description.value_or("");
        if (prompt_template.argument_hint && !prompt_template.argument_hint->empty()) {
            description = description.empty()
                ? *prompt_template.argument_hint
                : std::format("{} — {}", *prompt_template.argument_hint, description);
        }
        items.push_back({
            .value = prompt_template.name,
            .label = prompt_template.name,
            .description = std::move(description),
        });
    }
    for (const auto& skill : skills) {
        auto name = "skill:" + skill.name;
        if (!names.insert(name).second) continue;
        items.push_back({
            .value = name,
            .label = name,
            .description = skill.description,
        });
    }
    std::sort(items.begin(), items.end(), [](const auto& left, const auto& right) {
        return left.label < right.label;
    });
    return items;
}

[[nodiscard]] cch::tui::AutocompleteProvider command_autocomplete_provider(
    std::vector<cch::tui::AutocompleteItem> available_items) {
    return [items = std::move(available_items)](const cch::tui::AutocompleteRequest& request)
        -> std::optional<cch::tui::AutocompleteSuggestions> {
        if (request.cursor.line != 0 || request.lines.empty()) return std::nullopt;
        const auto& line = request.lines.front();
        if (request.cursor.column > line.size()) return std::nullopt;
        const auto prefix = std::string_view{line}.substr(0, request.cursor.column);
        if (prefix.empty() || prefix.front() != '/') return std::nullopt;
        if (std::any_of(prefix.begin(), prefix.end(), [](unsigned char ch) {
                return ch >= 0x80 || ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
            })) {
            return std::nullopt;
        }

        return cch::tui::AutocompleteSuggestions{
            .items = items,
            .prefix = std::string{prefix},
        };
    };
}

class DismissibleView final
    : public cch::tui::Component,
      public cch::tui::InputHandler,
      public cch::tui::Focusable {
public:
    DismissibleView(
        std::unique_ptr<cch::tui::Component> content,
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
        ActionSink on_cancel)
        : content_(std::move(content)),
          keybindings_(std::move(keybindings)),
          on_cancel_(std::move(on_cancel)) {}
    DismissibleView(DismissibleView&&) = delete;
    DismissibleView& operator=(DismissibleView&&) = delete;
    ~DismissibleView() override = default;

    DismissibleView(const DismissibleView&) = delete;
    DismissibleView& operator=(const DismissibleView&) = delete;

    [[nodiscard]] util::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        if (callback_error_) return std::unexpected(*callback_error_);
        return content_->render(width);
    }
    void invalidate() override { content_->invalidate(); }
    void handle_input(const cch::tui::InputEventVariant& input) override {
        const auto* key = std::get_if<cch::tui::KeyEvent>(&input);
        if (key == nullptr || key->type == cch::tui::KeyEventType::Release ||
            !keybindings_->matches(*key, "tui.select.cancel") || !on_cancel_) {
            return;
        }
        try {
            on_cancel_();
        } catch (const std::exception& error) {
            callback_error_ = util::make_error(
                util::ErrorCode::Unknown,
                "Hotkey help cancellation failed",
                error.what());
        } catch (...) {
            callback_error_ = util::make_error(
                util::ErrorCode::Unknown,
                "Hotkey help cancellation failed");
        }
    }
    [[nodiscard]] bool accepts_key_releases() const override { return false; }
    void set_focused(bool focused) override { focused_ = focused; }
    [[nodiscard]] bool focused() const override { return focused_; }
    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override {
        return std::nullopt;
    }

private:
    std::unique_ptr<cch::tui::Component> content_;
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings_;
    ActionSink on_cancel_;
    std::optional<util::Error> callback_error_;
    bool focused_{false};
};

class InteractiveView final
    : public cch::tui::Component,
      public cch::tui::InputHandler,
      public cch::tui::Focusable,
      public cch::tui::ViewportAware {
public:
    InteractiveView(
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
        ActionSink on_invalidate,
        SubmitSink on_submit,
        ActionSink on_interrupt,
        ActionSink on_exit,
        PromptActiveHook prompt_active,
        std::vector<cch::tui::AutocompleteItem> autocomplete_items,
        const LiveTheme& theme)
        : keybindings_(std::move(keybindings)),
          on_invalidate_(std::move(on_invalidate)),
          on_submit_(std::move(on_submit)),
          on_interrupt_(std::move(on_interrupt)),
          on_exit_(std::move(on_exit)),
          prompt_active_(std::move(prompt_active)),
          transcript_(theme, *keybindings_),
          editor_(
              cch::tui::EditorOptions{.keybindings = keybindings_},
              [this](std::string) {
                  invoke_action(on_invalidate_, "Native TUI invalidation callback failed");
              },
              [this](std::string text) {
                  invoke_submit(std::move(text));
              }) {
        editor_.set_autocomplete_provider(command_autocomplete_provider(
            std::move(autocomplete_items)));
    }
    InteractiveView(InteractiveView&&) = delete;
    InteractiveView& operator=(InteractiveView&&) = delete;
    ~InteractiveView() override = default;
    InteractiveView(const InteractiveView&) = delete;
    InteractiveView& operator=(const InteractiveView&) = delete;

    void initialize(const AgentSessionSnapshot& snapshot) {
        std::lock_guard lock(mutex_);
        transcript_.initialize(snapshot);
    }

    void apply_event(const agent::AgentLifecycleEvent& event) {
        std::lock_guard lock(mutex_);
        transcript_.apply_event(event);
    }

    void clear_transcript() {
        std::lock_guard lock(mutex_);
        transcript_.clear();
    }

    void append_frontend_message(std::string text) {
        std::lock_guard lock(mutex_);
        transcript_.append_frontend_message(std::move(text));
    }

    void append_diagnostic(std::string text) {
        std::lock_guard lock(mutex_);
        transcript_.append_diagnostic(std::move(text));
    }

    void restore_text_if_empty(const std::string& text) {
        std::lock_guard lock(mutex_);
        if (editor_.expanded_text().empty()) editor_.set_text(text);
    }

    void set_editor_theme(cch::tui::EditorTheme theme) {
        std::lock_guard lock(mutex_);
        editor_.set_theme(std::move(theme));
    }

    [[nodiscard]] util::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        std::lock_guard lock(mutex_);
        if (callback_error_) return std::unexpected(*callback_error_);
        editor_.set_available_height(available_rows_);
        std::vector<std::string> editor_lines;
        if (auto editor = editor_.render(width); !editor) {
            return std::unexpected(editor.error());
        } else {
            editor_lines = std::move(editor->lines);
        }

        std::vector<std::string> autocomplete_lines;
        const auto autocomplete = editor_.autocomplete_items();
        const auto selected = editor_.autocomplete_selected_index();
        constexpr std::size_t kMaxAutocompleteRows = 5;
        const auto autocomplete_capacity = available_rows_ > editor_lines.size()
            ? std::min(kMaxAutocompleteRows, available_rows_ - editor_lines.size())
            : 0;
        const auto first_autocomplete = selected < autocomplete_capacity || autocomplete_capacity == 0
            ? 0
            : selected - autocomplete_capacity + 1;
        const auto autocomplete_count = std::min(
            autocomplete_capacity,
            autocomplete.size() - std::min(first_autocomplete, autocomplete.size()));
        autocomplete_lines.reserve(autocomplete_count);
        for (std::size_t offset = 0; offset < autocomplete_count; ++offset) {
            const auto index = first_autocomplete + offset;
            std::string text = index == selected ? "> /" : "  /";
            text += autocomplete[index].label;
            if (!autocomplete[index].description.empty()) {
                text += " — " + autocomplete[index].description;
            }
            cch::tui::TruncatedText item{std::move(text)};
            if (auto rendered = item.render(width); !rendered) {
                return std::unexpected(rendered.error());
            } else if (!rendered->lines.empty()) {
                autocomplete_lines.push_back(std::move(rendered->lines.front()));
            }
        }

        std::vector<std::string> transcript_lines;
        const auto occupied_rows = editor_lines.size() + autocomplete_lines.size();
        if (available_rows_ > occupied_rows) {
            const auto capacity = available_rows_ - occupied_rows;
            if (auto rendered = transcript_.render(width); !rendered) {
                return std::unexpected(rendered.error());
            } else {
                const auto take = std::min(capacity, rendered->size());
                transcript_lines.assign(
                    rendered->end() - static_cast<std::ptrdiff_t>(take),
                    rendered->end());
            }
        }

        editor_row_offset_ = transcript_lines.size();
        transcript_lines.insert(
            transcript_lines.end(),
            std::make_move_iterator(editor_lines.begin()),
            std::make_move_iterator(editor_lines.end()));
        transcript_lines.insert(
            transcript_lines.end(),
            std::make_move_iterator(autocomplete_lines.begin()),
            std::make_move_iterator(autocomplete_lines.end()));
        return cch::tui::RenderResult{.lines = std::move(transcript_lines)};
    }

    void invalidate() override {
        std::lock_guard lock(mutex_);
        editor_.invalidate();
    }

    void handle_input(const cch::tui::InputEventVariant& input) override {
        std::lock_guard lock(mutex_);
        const auto* key = std::get_if<cch::tui::KeyEvent>(&input);
        if (key != nullptr && key->type != cch::tui::KeyEventType::Release) {
            if (keybindings_->matches(*key, "app.exit") && editor_.expanded_text().empty()) {
                invoke_action(on_exit_, "Native TUI exit callback failed");
                return;
            }
            if (keybindings_->matches(*key, "app.interrupt") && prompt_is_active()) {
                invoke_action(on_interrupt_, "Native TUI interrupt callback failed");
                return;
            }
            if (keybindings_->matches(*key, "app.clear")) {
                editor_.set_text({});
                return;
            }
            if (keybindings_->matches(*key, "app.tools.expand")) {
                transcript_.toggle_tool_output();
                invoke_action(on_invalidate_, "Native TUI invalidation callback failed");
                return;
            }
            if (keybindings_->matches(*key, "app.thinking.toggle")) {
                transcript_.toggle_thinking();
                invoke_action(on_invalidate_, "Native TUI invalidation callback failed");
                return;
            }
        }
        const auto autocomplete_was_open = editor_.autocomplete_open();
        const auto previous_selection = editor_.autocomplete_selected_index();
        editor_.handle_input(input);
        if (autocomplete_was_open != editor_.autocomplete_open() ||
            previous_selection != editor_.autocomplete_selected_index()) {
            invoke_action(on_invalidate_, "Native TUI invalidation callback failed");
        }
    }

    [[nodiscard]] bool accepts_key_releases() const override {
        return false;
    }

    void set_focused(bool focused) override {
        std::lock_guard lock(mutex_);
        editor_.set_focused(focused);
    }

    [[nodiscard]] bool focused() const override {
        std::lock_guard lock(mutex_);
        return editor_.focused();
    }

    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override {
        std::lock_guard lock(mutex_);
        auto cursor = editor_.cursor_location();
        if (cursor) cursor->row += editor_row_offset_;
        return cursor;
    }

    void set_available_height(std::size_t rows) override {
        std::lock_guard lock(mutex_);
        available_rows_ = std::max<std::size_t>(1, rows);
    }

private:
    void record_callback_error(
        std::string message,
        std::string detail = {}) {
        callback_error_ = util::make_error(
            util::ErrorCode::Unknown,
            std::move(message),
            std::move(detail));
    }

    void invoke_action(ActionSink& action, std::string_view failure_message) {
        if (!action) return;
        try {
            action();
        } catch (const std::exception& error) {
            record_callback_error(std::string(failure_message), error.what());
        } catch (...) {
            record_callback_error(std::string(failure_message));
        }
    }

    void invoke_submit(std::string text) {
        if (!on_submit_) return;
        try {
            on_submit_(std::move(text));
        } catch (const std::exception& error) {
            record_callback_error("Native TUI submit callback failed", error.what());
        } catch (...) {
            record_callback_error("Native TUI submit callback failed");
        }
    }

    [[nodiscard]] bool prompt_is_active() {
        if (!prompt_active_) return false;
        try {
            return prompt_active_();
        } catch (const std::exception& error) {
            record_callback_error("Native TUI prompt-state callback failed", error.what());
        } catch (...) {
            record_callback_error("Native TUI prompt-state callback failed");
        }
        return false;
    }

    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings_;
    ActionSink on_invalidate_;
    SubmitSink on_submit_;
    ActionSink on_interrupt_;
    ActionSink on_exit_;
    PromptActiveHook prompt_active_;
    std::optional<util::Error> callback_error_;
    mutable std::mutex mutex_;
    Transcript transcript_;
    cch::tui::Editor editor_;
    std::size_t available_rows_{24};
    std::size_t editor_row_offset_{0};
};

class InteractiveState final : public std::enable_shared_from_this<InteractiveState> {
public:
    InteractiveState(
        AgentSession& session,
        cch::tui::Terminal& terminal,
        boost::asio::any_io_executor executor)
        : session_(session),
          terminal_(terminal),
          tui_(terminal),
          executor_(std::move(executor)),
          exit_wait_(executor_) {
        exit_wait_.expires_at(std::chrono::steady_clock::time_point::max());
    }
    InteractiveState(InteractiveState&&) = delete;
    InteractiveState& operator=(InteractiveState&&) = delete;
    ~InteractiveState() = default;
    InteractiveState(const InteractiveState&) = delete;
    InteractiveState& operator=(const InteractiveState&) = delete;

    [[nodiscard]] util::ExpectedVoid start(InteractiveModeConfig config) {
        if (auto registered = register_commands(); !registered) {
            return fail_start(registered.error());
        }

        InteractiveStartupDiagnostics diagnostics;
        if (auto loaded = load_startup_resources(config); !loaded) {
            return fail_start(loaded.error());
        } else {
            diagnostics = std::move(*loaded);
        }

        const auto weak = weak_from_this();
        auto view = make_interactive_view(weak);
        view_ = view.get();
        if (auto attached = tui_.add_child(std::move(view)); !attached) {
            return fail_start(attached.error());
        }
        if (auto subscribed = subscribe_to_session(weak); !subscribed) {
            return fail_start(subscribed.error());
        }

        tui_.set_render_request_sink([weak] {
            if (const auto self = weak.lock()) self->post_render();
        });
        if (auto started = tui_.start(); !started) return fail_start(started.error());
        tui_started_ = true;
        running_ = true;

        initialize_view(diagnostics);
        if (auto rendered = tui_.render(); !rendered) return fail_start(rendered.error());
        if (auto focused = tui_.set_focus(view_); !focused) return fail_start(focused.error());
        if (auto rendered = tui_.render(); !rendered) return fail_start(rendered.error());
        return {};
    }

    [[nodiscard]] boost::asio::steady_timer& exit_wait() {
        return exit_wait_;
    }

    [[nodiscard]] util::ExpectedVoid finish() {
        running_ = false;
        subscription_.reset();
        session_.close();
        const auto stopped = tui_.stop();
        tui_started_ = false;
        if (!completion_result_) completion_result_.emplace();
        if (!*completion_result_) {
            if (!stopped) {
                return std::unexpected(aggregate_presentation_errors(
                    completion_result_->error(),
                    stopped.error(),
                    "Native TUI failed and terminal restoration failed"));
            }
            return std::unexpected(completion_result_->error());
        }
        if (!stopped) {
            return std::unexpected(presentation_error(
                stopped.error(),
                "Native TUI terminal restoration failed"));
        }
        return {};
    }

private:
    [[nodiscard]] util::ExpectedVoid register_commands() {
        if (auto registered = register_builtin_commands(commands_); !registered) {
            return std::unexpected(registered.error());
        }
        return register_native_tui_commands(commands_);
    }

    [[nodiscard]] util::Expected<InteractiveStartupDiagnostics> load_startup_resources(
        const InteractiveModeConfig& config) {
        InteractiveStartupDiagnostics diagnostics;
        constexpr std::array<std::string_view, 5> kActions{
            "app.interrupt",
            "app.clear",
            "app.exit",
            "app.tools.expand",
            "app.thinking.toggle",
        };
        if (auto definitions = baseline_application_keybindings(kActions, config.platform); !definitions) {
            return std::unexpected(definitions.error());
        } else {
            KeybindingCatalogRequest request;
            request.agent_config_directory = config.agent_config_directory;
            request.application_definitions = std::move(*definitions);
            request.platform = config.platform;
            if (auto catalog = load_keybinding_catalog(std::move(request)); !catalog) {
                return std::unexpected(catalog.error());
            } else {
                keybindings_ = catalog->registry;
                diagnostics.keybindings = std::move(catalog->diagnostics);
            }
        }

        const auto settings_path = config.agent_config_directory.empty()
            ? std::filesystem::path{}
            : config.agent_config_directory / "settings.json";
        if (auto settings = SettingsLoader::load(settings_path); !settings) {
            return std::unexpected(settings.error());
        } else {
            const auto capabilities = terminal_.capabilities();
            ThemeCatalogRequest request;
            request.agent_config_directory = config.agent_config_directory;
            request.user_active_theme = settings->theme;
            request.terminal_capabilities = capabilities;
            if (auto catalog = load_theme_catalog(std::move(request)); !catalog) {
                return std::unexpected(catalog.error());
            } else {
                diagnostics.themes = catalog->diagnostics;
                ThemeSelectionCommitter committer;
                if (!settings_path.empty()) {
                    committer = [settings_path](std::string_view name) {
                        return SettingsLoader::save_theme_selection(settings_path, name);
                    };
                }
                theme_controller_.emplace(
                    std::move(*catalog),
                    tui_,
                    capabilities.color,
                    std::move(committer));
            }
        }
        return diagnostics;
    }

    [[nodiscard]] std::unique_ptr<InteractiveView> make_interactive_view(
        std::weak_ptr<InteractiveState> weak) {
        return std::make_unique<InteractiveView>(
            keybindings_,
            [weak] {
                if (const auto self = weak.lock()) self->post_invalidate();
            },
            [weak](std::string text) {
                if (const auto self = weak.lock()) self->post_submit(std::move(text));
            },
            [weak] {
                if (const auto self = weak.lock()) self->post_interrupt();
            },
            [weak] {
                if (const auto self = weak.lock()) self->post_exit();
            },
            [weak] {
                if (const auto self = weak.lock()) return self->prompt_active_.load();
                return false;
            },
            command_autocomplete_items(commands_, session_.templates(), session_.skills()),
            theme_controller_->live_theme());
    }

    [[nodiscard]] util::ExpectedVoid subscribe_to_session(
        std::weak_ptr<InteractiveState> weak) {
        if (auto subscribed = session_.subscribe(
                [weak](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
                    if (const auto self = weak.lock()) self->on_event(event);
                    return {};
                });
            !subscribed) {
            return std::unexpected(subscribed.error());
        } else {
            subscription_.emplace(std::move(*subscribed));
        }
        return {};
    }

    void initialize_view(const InteractiveStartupDiagnostics& diagnostics) {
        view_->set_editor_theme(theme_controller_->live_theme().editor_theme());
        const auto snapshot = session_.snapshot();
        view_->initialize(snapshot);
        for (const auto& diagnostic : snapshot.agent_state.diagnostics) {
            view_->append_diagnostic(combined_error_text(diagnostic));
        }
        for (const auto& diagnostic : diagnostics.keybindings) {
            view_->append_diagnostic(diagnostic.message);
        }
        for (const auto& diagnostic : diagnostics.themes) {
            view_->append_diagnostic(diagnostic.message);
        }
    }

    [[nodiscard]] util::ExpectedVoid fail_start(const util::Error& error) {
        running_ = false;
        session_.close();
        util::ExpectedVoid stopped;
        if (tui_started_) stopped = tui_.stop();
        tui_started_ = false;
        if (!stopped) {
            return std::unexpected(aggregate_presentation_errors(
                error,
                stopped.error(),
                "Native TUI startup and terminal restoration failed"));
        }
        return std::unexpected(startup_error(error));
    }

    void post_invalidate() {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak] {
            if (const auto self = weak.lock(); self && self->running_) self->tui_.invalidate();
        });
    }

    void post_submit(std::string text) {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak, text = std::move(text)]() mutable {
            if (const auto self = weak.lock()) self->submit(std::move(text));
        });
    }

    void post_interrupt() {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak] {
            if (const auto self = weak.lock(); self && self->running_) self->session_.abort();
        });
    }

    void post_exit() {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak] {
            if (const auto self = weak.lock()) self->request_exit();
        });
    }

    void post_render() {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak] {
            if (const auto self = weak.lock()) self->render();
        });
    }

    void post_close_overlay() {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak] {
            if (const auto self = weak.lock()) self->close_overlay();
        });
    }

    [[nodiscard]] CommandContext command_context() const {
        const auto& path = session_.session_path();
        return CommandContext{
            .session_id = session_.session_id(),
            .session_path = path
                ? std::optional<std::string>{path->string()}
                : std::nullopt,
            .workspace_path = session_.workspace().string(),
            .provider = session_.provider(),
            .model = session_.model(),
            .message_count = session_.message_count(),
            .available_commands = commands_.list_commands(),
        };
    }

    void append_command_error(const util::Error& error) {
        if (view_ == nullptr) return;
        view_->append_diagnostic(combined_error_text(error));
        tui_.invalidate();
    }

    [[nodiscard]] util::ExpectedVoid attach_overlay(
        std::unique_ptr<cch::tui::Overlay> overlay) {
        auto* overlay_pointer = overlay.get();
        if (auto attached = tui_.add_overlay(std::move(overlay)); !attached) {
            return std::unexpected(attached.error());
        }
        active_overlay_ = overlay_pointer;
        if (auto focused = tui_.set_focus(active_overlay_); !focused) {
            const auto focus_error = focused.error();
            if (auto removed = tui_.remove_overlay(active_overlay_); !removed) {
                return std::unexpected(aggregate_presentation_errors(
                    focus_error,
                    removed.error(),
                    "Native TUI overlay focus and cleanup failed"));
            }
            active_overlay_ = nullptr;
            return std::unexpected(focus_error);
        }
        tui_.invalidate();
        return {};
    }

    void close_overlay() {
        if (!running_ || active_overlay_ == nullptr) return;
        if (auto removed = tui_.remove_overlay(active_overlay_); !removed) {
            append_command_error(removed.error());
            return;
        }
        active_overlay_ = nullptr;
        tui_.invalidate();
    }

    void open_settings() {
        if (active_overlay_ != nullptr || !theme_controller_ || !keybindings_) return;
        const auto weak = weak_from_this();
        if (auto overlay = make_theme_settings_overlay(
                *theme_controller_,
                keybindings_,
                [weak] {
                    if (const auto self = weak.lock()) self->post_close_overlay();
                });
            !overlay) {
            append_command_error(overlay.error());
        } else if (auto attached = attach_overlay(std::move(*overlay)); !attached) {
            append_command_error(attached.error());
        }
    }

    void open_hotkeys() {
        if (active_overlay_ != nullptr || !keybindings_) return;
        cch::tui::OverlayOptions options;
        options.position = cch::tui::OverlayPosition::TopLeft;
        options.size_constraints.max_width = 90;
        options.size_constraints.max_height = 26;
        options.z_index = 100;
        auto overlay = std::make_unique<cch::tui::Overlay>(std::move(options));
        const auto weak = weak_from_this();
        auto content = std::make_unique<DismissibleView>(
            make_hotkey_help_view(keybindings_),
            keybindings_,
            [weak] {
                if (const auto self = weak.lock()) self->post_close_overlay();
            });
        if (auto attached = overlay->add_child(std::move(content)); !attached) {
            append_command_error(attached.error());
            return;
        }
        if (auto attached = attach_overlay(std::move(overlay)); !attached) {
            append_command_error(attached.error());
        }
    }

    void apply_command_result(CommandResult result) {
        if (view_ != nullptr) view_->append_frontend_message(std::move(result.display_text));
        switch (result.effect) {
        case CommandEffect::None:
            tui_.invalidate();
            return;
        case CommandEffect::ClearScreen:
            if (auto cleared = tui_.clear_screen(); !cleared) {
                append_command_error(cleared.error());
            } else {
                if (view_ != nullptr) view_->clear_transcript();
                tui_.invalidate();
            }
            return;
        case CommandEffect::OpenSettings:
            open_settings();
            return;
        case CommandEffect::OpenHotkeys:
            open_hotkeys();
            return;
        case CommandEffect::Shutdown:
            if (view_ != nullptr) {
                tui_.invalidate();
                render();
            }
            request_exit();
            return;
        }
    }

    [[nodiscard]] bool dispatch_command(std::string_view text) {
        const auto parsed = prompt::try_parse_slash_command(text);
        if (!parsed) return false;
        try {
            if (auto result = commands_.dispatch(
                    parsed->first,
                    command_context(),
                    parsed->second);
                !result) {
                return false;
            } else {
                apply_command_result(std::move(*result));
            }
            return true;
        } catch (const std::exception& error) {
            append_command_error(util::make_error(
                util::ErrorCode::Unknown,
                "Command handler failed",
                error.what()));
            return true;
        } catch (...) {
            append_command_error(util::make_error(
                util::ErrorCode::Unknown,
                "Command handler failed"));
            return true;
        }
    }

    void submit(std::string text) {
        if (!running_ || view_ == nullptr || text.empty()) return;
        if (dispatch_command(text)) return;
        if (prompt_active_) {
            view_->restore_text_if_empty(text);
            view_->append_diagnostic("A prompt is already in flight");
            tui_.invalidate();
            return;
        }

        prompt_active_ = true;
        const auto self = shared_from_this();
        boost::asio::co_spawn(
            executor_,
            [self, text = std::move(text)]() mutable -> boost::asio::awaitable<void> {
                util::ExpectedVoid result;
                try {
                    result = co_await self->session_.prompt(text);
                } catch (const std::exception& error) {
                    result = std::unexpected(util::make_error(
                        util::ErrorCode::Unknown,
                        "Native TUI prompt failed",
                        error.what()));
                } catch (...) {
                    result = std::unexpected(util::make_error(
                        util::ErrorCode::Unknown,
                        "Native TUI prompt failed",
                        "unknown exception"));
                }
                self->prompt_finished(std::move(result), text);
            },
            [weak = weak_from_this()](std::exception_ptr exception) {
                if (exception) {
                    if (const auto self = weak.lock()) self->prompt_launch_failed(exception);
                }
            });
    }

    void prompt_launch_failed(std::exception_ptr exception) {
        prompt_active_ = false;
        std::string detail = "unknown exception";
        try {
            std::rethrow_exception(exception);
        } catch (const std::exception& error) {
            detail = error.what();
        } catch (...) {
        }
        if (view_ != nullptr && running_) {
            view_->append_diagnostic(std::format("Native TUI prompt failed: {}", detail));
            tui_.invalidate();
        }
        if (exit_requested_) signal_exit();
    }

    void prompt_finished(util::ExpectedVoid result, const std::string& submitted_text) {
        prompt_active_ = false;
        if (!result && view_ != nullptr && running_) {
            view_->append_diagnostic(combined_error_text(result.error()));
            view_->restore_text_if_empty(submitted_text);
            tui_.invalidate();
        }
        if (exit_requested_) signal_exit();
    }

    void on_event(const agent::AgentLifecycleEvent& event) {
        if (!running_ || view_ == nullptr) return;
        view_->apply_event(event);
        tui_.invalidate();
    }

    void render() {
        if (!running_) return;
        if (auto rendered = tui_.render(); !rendered) {
            completion_result_ = std::unexpected(startup_error(rendered.error()));
            request_exit();
        }
    }

    void request_exit() {
        if (!running_ || exit_requested_) return;
        exit_requested_ = true;
        session_.close();
        if (!prompt_active_) signal_exit();
    }

    void signal_exit() {
        try {
            (void)exit_wait_.cancel();
        } catch (...) {
            if (!completion_result_) {
                completion_result_ = std::unexpected(util::make_error(
                    util::ErrorCode::Unknown,
                    "Native TUI exit notification failed"));
            }
        }
    }

    AgentSession& session_; // must outlive this interactive run.
    cch::tui::Terminal& terminal_; // must outlive this interactive run.
    cch::tui::Tui tui_;
    CommandRegistry commands_;
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings_;
    std::optional<ThemeController> theme_controller_;
    boost::asio::any_io_executor executor_;
    boost::asio::steady_timer exit_wait_;
    std::optional<EventSubscription> subscription_;
    InteractiveView* view_{nullptr}; // aliases the child owned by tui_.
    cch::tui::Overlay* active_overlay_{nullptr}; // aliases an overlay owned by tui_.
    std::atomic<bool> running_{false};
    std::atomic<bool> prompt_active_{false};
    bool tui_started_{false};
    bool exit_requested_{false};
    std::optional<util::ExpectedVoid> completion_result_;
};

} // namespace

boost::asio::awaitable<util::ExpectedVoid> run_interactive_mode(
    AgentSession& session,
    cch::tui::Terminal& terminal,
    InteractiveModeConfig config) {
    const auto executor = co_await boost::asio::this_coro::executor;
    auto state = std::make_shared<InteractiveState>(session, terminal, executor);
    if (auto started = state->start(std::move(config)); !started) {
        co_return std::unexpected(started.error());
    }

    boost::system::error_code wait_error;
    co_await state->exit_wait().async_wait(
        boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
    co_return state->finish();
}

} // namespace cch::coding_agent::tui
