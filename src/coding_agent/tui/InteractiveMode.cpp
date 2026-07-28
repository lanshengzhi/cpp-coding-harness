#include "InteractiveMode.hpp"

#include <cch/agent/AgentEvent.hpp>
#include <cch/coding_agent/Sdk.hpp>
#include <cch/tui/Editor.hpp>
#include <cch/tui/Terminal.hpp>
#include <cch/tui/Tui.hpp>

#include "coding_agent/BoundedText.hpp"
#include "coding_agent/tui/KeybindingCatalog.hpp"
#include "coding_agent/tui/Theme.hpp"
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
                  if (on_invalidate_) on_invalidate_();
              },
              [this](std::string text) {
                  if (on_submit_) on_submit_(std::move(text));
              }) {}
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
        editor_.set_available_height(available_rows_);
        std::vector<std::string> editor_lines;
        if (auto editor = editor_.render(width); !editor) {
            return std::unexpected(editor.error());
        } else {
            editor_lines = std::move(editor->lines);
        }

        std::vector<std::string> transcript_lines;
        if (available_rows_ > editor_lines.size()) {
            const auto capacity = available_rows_ - editor_lines.size();
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
                if (on_exit_) on_exit_();
                return;
            }
            if (keybindings_->matches(*key, "app.interrupt") &&
                prompt_active_ && prompt_active_()) {
                if (on_interrupt_) on_interrupt_();
                return;
            }
            if (keybindings_->matches(*key, "app.clear")) {
                editor_.set_text({});
                return;
            }
            if (keybindings_->matches(*key, "app.tools.expand")) {
                transcript_.toggle_tool_output();
                if (on_invalidate_) on_invalidate_();
                return;
            }
            if (keybindings_->matches(*key, "app.thinking.toggle")) {
                transcript_.toggle_thinking();
                if (on_invalidate_) on_invalidate_();
                return;
            }
        }
        editor_.handle_input(input);
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
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings_;
    ActionSink on_invalidate_;
    SubmitSink on_submit_;
    ActionSink on_interrupt_;
    ActionSink on_exit_;
    PromptActiveHook prompt_active_;
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
        constexpr std::array<std::string_view, 5> kActions{
            "app.interrupt",
            "app.clear",
            "app.exit",
            "app.tools.expand",
            "app.thinking.toggle",
        };
        auto application_definitions = baseline_application_keybindings(kActions, config.platform);
        if (!application_definitions) return fail_start(application_definitions.error());

        KeybindingCatalogRequest catalog_request;
        catalog_request.agent_config_directory = std::move(config.agent_config_directory);
        catalog_request.application_definitions = std::move(*application_definitions);
        catalog_request.platform = config.platform;
        auto catalog = load_keybinding_catalog(std::move(catalog_request));
        if (!catalog) return fail_start(catalog.error());

        const auto capabilities = terminal_.capabilities();
        theme_.emplace(select_builtin_theme(capabilities), capabilities.color);

        const auto weak = weak_from_this();
        auto view = std::make_unique<InteractiveView>(
            catalog->registry,
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
            *theme_);
        view_ = view.get();
        if (auto attached = tui_.add_child(std::move(view)); !attached) {
            return fail_start(attached.error());
        }

        if (auto subscribed = session_.subscribe(
                [weak](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
                    if (const auto self = weak.lock()) self->on_event(event);
                    return {};
                });
            !subscribed) {
            return fail_start(subscribed.error());
        } else {
            subscription_.emplace(std::move(*subscribed));
        }

        tui_.set_render_request_sink([weak] {
            if (const auto self = weak.lock()) self->post_render();
        });
        if (auto started = tui_.start(); !started) return fail_start(started.error());
        tui_started_ = true;
        running_ = true;

        view_->set_editor_theme(theme_->editor_theme());
        const auto snapshot = session_.snapshot();
        view_->initialize(snapshot);
        for (const auto& diagnostic : snapshot.agent_state.diagnostics) {
            view_->append_diagnostic(combined_error_text(diagnostic));
        }
        for (const auto& diagnostic : catalog->diagnostics) {
            view_->append_diagnostic(diagnostic.message);
        }
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

    void submit(std::string text) {
        if (!running_ || view_ == nullptr || text.empty()) return;
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
    std::optional<LiveTheme> theme_;
    cch::tui::Tui tui_;
    boost::asio::any_io_executor executor_;
    boost::asio::steady_timer exit_wait_;
    std::optional<EventSubscription> subscription_;
    InteractiveView* view_{nullptr}; // aliases the child owned by tui_.
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
