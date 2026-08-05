#include "InteractiveMode.hpp"

#include <cch/agent/AgentEvent.hpp>
#include <cch/ai/Content.hpp>
#include <cch/coding_agent/Sdk.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/tui/Editor.hpp>
#include <cch/tui/Loader.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/Terminal.hpp>
#include <cch/tui/TruncatedText.hpp>
#include <cch/tui/Tui.hpp>

#include "coding_agent/BoundedText.hpp"
#include "coding_agent/CommandRegistry.hpp"
#include "coding_agent/ImageInput.hpp"
#include "coding_agent/prompt/SlashCommandParser.hpp"
#include "coding_agent/runtime/AgentSessionInteractiveAccess.hpp"
#include "coding_agent/tui/InteractionPolicy.hpp"
#include "coding_agent/tui/InterruptAdmission.hpp"
#include "coding_agent/tui/KeybindingCatalog.hpp"
#include "coding_agent/tui/KeybindingHelp.hpp"
#include "coding_agent/tui/ThemeCatalog.hpp"
#include "coding_agent/tui/Transcript.hpp"
#include "coding_agent/tui/UserBashPresentation.hpp"
#include "coding_agent/tui/UserBashSyntax.hpp"
#include "harness/UniqueFd.hpp"
#include "util/TerminalText.hpp"

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
#include <cctype>
#include <cmath>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace cch::coding_agent::tui {
namespace {

using ActionSink = std::move_only_function<void()>;

struct EditorSubmissionRequest {
    std::string text;
    std::size_t editor_revision{0};
};

struct EditorInterruptRequest {
    std::string pending_bash_text;
    std::size_t editor_revision{0};
    bool pending_bash{false};
};

using InterruptSink = std::move_only_function<void(EditorInterruptRequest)>;
using SubmitSink = std::move_only_function<void(EditorSubmissionRequest)>;

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

[[nodiscard]] std::string editor_text_after_interrupt(
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

[[nodiscard]] util::Error presentation_error(
    const util::Error& error,
    std::string message) {
    return util::make_error(
        error.code,
        std::move(message),
        bounded_redacted_presentation(combined_error_text(error)));
}

[[nodiscard]] std::string clipboard_uuid() {
    std::random_device random;
    std::array<std::uint8_t, 16> bytes{};
    for (auto& byte : bytes) byte = static_cast<std::uint8_t>(random());
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);

    std::string value;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) value.push_back('-');
        value += std::format("{:02x}", bytes[index]);
    }
    return value;
}

[[nodiscard]] util::Expected<std::filesystem::path> write_clipboard_image(
    std::span<const std::uint8_t> bytes,
    std::string_view extension) {
#if defined(__unix__) || defined(__APPLE__)
    std::error_code temp_error;
    const auto temp_directory = std::filesystem::temp_directory_path(temp_error);
    if (temp_error || temp_directory.empty()) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Process,
            "clipboard temporary directory is unavailable",
            temp_error.message()));
    }

    for (std::size_t attempt = 0; attempt < 16; ++attempt) {
        std::filesystem::path path;
        try {
            path = temp_directory /
                std::format("pi-clipboard-{}{}", clipboard_uuid(), extension);
        } catch (const std::exception& error) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Process,
                "could not generate a clipboard image path",
                error.what()));
        } catch (...) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Process,
                "could not generate a clipboard image path"));
        }
        harness::UniqueFd fd(::open(
            path.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
            0600));
        if (!fd) {
            if (errno == EEXIST) continue;
            return std::unexpected(util::make_error(
                util::ErrorCode::Process,
                "could not create clipboard image file",
                std::error_code(errno, std::generic_category()).message()));
        }

        std::size_t written = 0;
        while (written < bytes.size()) {
            const auto count = ::write(
                fd.get(),
                bytes.data() + written,
                bytes.size() - written);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) {
                const auto write_error = errno;
                (void)fd.close();
                std::error_code remove_error;
                std::filesystem::remove(path, remove_error);
                return std::unexpected(util::make_error(
                    util::ErrorCode::Process,
                    "could not write clipboard image file",
                    std::error_code(write_error, std::generic_category()).message()));
            }
            written += static_cast<std::size_t>(count);
        }
        if (fd.close() != 0) {
            const auto close_error = errno;
            std::error_code remove_error;
            std::filesystem::remove(path, remove_error);
            return std::unexpected(util::make_error(
                util::ErrorCode::Process,
                "could not finish clipboard image file",
                std::error_code(close_error, std::generic_category()).message()));
        }
        return path;
    }
    return std::unexpected(util::make_error(
        util::ErrorCode::Process,
        "could not allocate a unique clipboard image path"));
#else
    (void)bytes;
    (void)extension;
    return std::unexpected(util::make_error(
        util::ErrorCode::Process,
        "clipboard image files are unavailable on this platform"));
#endif
}

[[nodiscard]] bool protocol_supports_image(
    cch::tui::InlineImageProtocol protocol,
    std::string_view mime_type) {
    if (protocol == cch::tui::InlineImageProtocol::Kitty) return mime_type == "image/png";
    if (protocol == cch::tui::InlineImageProtocol::ITerm2) {
        return mime_type == "image/png" || mime_type == "image/jpeg" ||
            mime_type == "image/gif" || mime_type == "image/webp";
    }
    return false;
}

[[nodiscard]] std::size_t estimated_image_rows(
    const cch::tui::InlineImageRenderRegion& image,
    const cch::tui::TerminalCapabilities& capabilities,
    std::size_t width) {
    if (!capabilities.cell_pixels || capabilities.cell_pixels->width == 0 ||
        capabilities.cell_pixels->height == 0 ||
        !protocol_supports_image(capabilities.inline_images, image.mime_type) ||
        image.pixel_width == 0 || image.pixel_height == 0 || image.region.column >= width) {
        return 1;
    }
    const auto available_width = width - image.region.column;
    const auto default_width = std::min<std::size_t>(width > 2 ? width - 2 : 1, 60);
    const auto max_width = std::max<std::size_t>(
        1,
        std::min(available_width, image.max_width.value_or(default_width)));
    const auto& cells = *capabilities.cell_pixels;
    const auto default_height = std::max<std::size_t>(
        1,
        (max_width * cells.width + cells.height - 1) / cells.height);
    const auto max_height = std::max<std::size_t>(
        1, image.max_height.value_or(default_height));
    const auto width_scale =
        static_cast<long double>(max_width * cells.width) / image.pixel_width;
    const auto height_scale =
        static_cast<long double>(max_height * cells.height) / image.pixel_height;
    const auto scale = std::min(width_scale, height_scale);
    return std::max<std::size_t>(
        1,
        std::min(max_height, static_cast<std::size_t>(std::ceil(
            static_cast<long double>(image.pixel_height) * scale / cells.height))));
}

[[nodiscard]] std::optional<std::string> queued_editor_text(
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

[[nodiscard]] util::Expected<std::vector<std::string>> queued_editor_texts(
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
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "queued input contains content that the editor cannot restore"));
    }
    return restored;
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
    // Runtime activity stays with InteractiveState; the view contributes only
    // the pending-Bash fact sampled from editor state when interrupt is pressed.
    InteractiveView(
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
        ActionSink on_invalidate,
        SubmitSink on_submit,
        SubmitSink on_follow_up,
        ActionSink on_clipboard_paste,
        ActionSink on_dequeue,
        InterruptSink on_interrupt,
        ActionSink on_exit,
        bool user_bash_available,
        std::vector<cch::tui::AutocompleteItem> autocomplete_items,
        cch::tui::TerminalCapabilities terminal_capabilities,
        const LiveTheme& theme)
        : keybindings_(std::move(keybindings)),
          terminal_capabilities_(std::move(terminal_capabilities)),
          on_invalidate_(std::move(on_invalidate)),
          on_submit_(std::move(on_submit)),
          on_follow_up_(std::move(on_follow_up)),
          on_clipboard_paste_(std::move(on_clipboard_paste)),
          on_dequeue_(std::move(on_dequeue)),
          on_interrupt_(std::move(on_interrupt)),
          on_exit_(std::move(on_exit)),
          user_bash_available_(user_bash_available),
          transcript_(theme, *keybindings_),
          theme_(&theme),
          editor_(
              cch::tui::EditorOptions{.keybindings = keybindings_},
              [this](std::string text) {
                  // Editor submission clears before invoking its submit sink;
                  // keep that notification on the sampled text's revision.
                  if (!text.empty()) ++editor_revision_;
                  invoke_action(on_invalidate_, "Native TUI invalidation callback failed");
              },
              [this](std::string text) {
                  invoke_submit(EditorSubmissionRequest{
                      .text = std::move(text),
                      .editor_revision = editor_revision_,
                  });
              }),
          pending_bash_([this](bool exclude_from_context) {
              return make_bash_loader(exclude_from_context);
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

    void append_committed_message(ai::MessageVariant message) {
        std::lock_guard lock(mutex_);
        transcript_.append_committed_message(std::move(message));
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

    void append_user_bash_diagnostic(std::string text) {
        std::lock_guard lock(mutex_);
        transcript_.append_user_bash_diagnostic(std::move(text));
    }

    void restore_submitted_text(const std::string& text) {
        std::lock_guard lock(mutex_);
        restore_editor_text({text});
    }

    void clear_pending_bash(const EditorInterruptRequest& request) {
        std::lock_guard lock(mutex_);
        if (editor_revision_ == request.editor_revision) {
            editor_.set_text({});
            return;
        }
        editor_.set_text(editor_text_after_interrupt(
            request.pending_bash_text,
            editor_.expanded_text()));
    }

    void insert_editor_text(std::string text) {
        std::lock_guard lock(mutex_);
        editor_.insert_text_at_cursor(std::move(text));
    }

    void restore_queued_text(const std::vector<std::string>& messages) {
        std::lock_guard lock(mutex_);
        restore_editor_text(messages);
    }

    void set_pending_input(const agent::AgentInputQueues& queues) {
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

    void set_user_bash_progress(runtime::UserBashProgress progress) {
        std::lock_guard lock(mutex_);
        pending_bash_.update(std::move(progress));
    }

    void clear_user_bash_progress() {
        std::lock_guard lock(mutex_);
        pending_bash_.clear();
    }

    /// Replaces the pending block with its committed transcript entry in one
    /// step, so the clear-pending-before-append ordering cannot drift apart
    /// at call sites.
    void commit_user_bash(ai::MessageVariant message) {
        std::lock_guard lock(mutex_);
        pending_bash_.clear();
        transcript_.append_committed_message(std::move(message));
    }

    [[nodiscard]] util::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        std::lock_guard lock(mutex_);
        if (callback_error_) return std::unexpected(*callback_error_);
        editor_.set_available_height(available_rows_);
        // The editor enters Bash mode as soon as the trimmed input begins
        // with `!` (where User Bash dispatch is available).
        editor_.set_theme(unsubmitted_bash_mode()
            ? cch::tui::EditorTheme{
                .text = theme_->foreground_hook(ThemeToken::BashMode),
            }
            : theme_->editor_theme());
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

        std::vector<std::string> pending_lines;
        pending_lines.reserve(pending_steering_.size() + pending_follow_up_.size() + 3);
        if (pending_bash_.active()) {
            // One Bash block while pending; it becomes an ordinary transcript
            // entry through the same presentation after commitment.
            auto block = pending_bash_.render(
                *theme_,
                *keybindings_,
                transcript_.tools_expanded(),
                width);
            if (!block) return std::unexpected(block.error());
            pending_lines.insert(
                pending_lines.end(),
                std::make_move_iterator(block->begin()),
                std::make_move_iterator(block->end()));
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
            const auto hint = keybindings_->key_text("app.message.dequeue");
            cch::tui::TruncatedText item{std::format(
                "↳ {} to edit all queued messages",
                hint.empty() ? "Unbound" : hint)};
            if (auto rendered = item.render(width); !rendered) {
                return std::unexpected(rendered.error());
            } else if (!rendered->lines.empty()) {
                pending_lines.push_back(std::move(rendered->lines.front()));
            }
        }

        const auto editor_occupied_rows = editor_lines.size() + autocomplete_lines.size();
        const auto pending_capacity = available_rows_ > editor_occupied_rows
            ? available_rows_ - editor_occupied_rows
            : 0;
        if (pending_lines.size() > pending_capacity) {
            pending_lines.erase(
                pending_lines.begin(),
                pending_lines.end() - static_cast<std::ptrdiff_t>(pending_capacity));
        }

        cch::tui::RenderResult transcript_result;
        const auto occupied_rows = editor_occupied_rows + pending_lines.size();
        if (available_rows_ > occupied_rows) {
            const auto capacity = available_rows_ - occupied_rows;
            auto rendered = transcript_.render(width);
            if (!rendered) return std::unexpected(rendered.error());

            std::vector<std::size_t> materialized_rows(rendered->lines.size(), 1);
            for (const auto& image : rendered->images) {
                if (image.region.row >= materialized_rows.size()) continue;
                materialized_rows[image.region.row] +=
                    estimated_image_rows(image, terminal_capabilities_, width) - 1;
            }

            auto first_row = rendered->lines.size();
            std::size_t used_rows = 0;
            std::optional<std::size_t> fallback_only_row;
            while (first_row > 0) {
                const auto candidate_row = first_row - 1;
                const auto row_cost = materialized_rows[candidate_row];
                if (row_cost > capacity - used_rows) {
                    if (used_rows < capacity) {
                        first_row = candidate_row;
                        fallback_only_row = candidate_row;
                    }
                    break;
                }
                first_row = candidate_row;
                used_rows += row_cost;
            }
            transcript_result.lines.assign(
                rendered->lines.begin() + static_cast<std::ptrdiff_t>(first_row),
                rendered->lines.end());
            for (auto& image : rendered->images) {
                const auto image_end = image.region.row + image.region.rows;
                if (image.region.row < first_row || image_end > rendered->lines.size() ||
                    (fallback_only_row && image.region.row == *fallback_only_row)) {
                    continue;
                }
                image.region.row -= first_row;
                transcript_result.images.push_back(std::move(image));
            }
        }

        transcript_result.lines.insert(
            transcript_result.lines.end(),
            std::make_move_iterator(pending_lines.begin()),
            std::make_move_iterator(pending_lines.end()));
        editor_row_offset_ = transcript_result.lines.size();
        transcript_result.lines.insert(
            transcript_result.lines.end(),
            std::make_move_iterator(editor_lines.begin()),
            std::make_move_iterator(editor_lines.end()));
        transcript_result.lines.insert(
            transcript_result.lines.end(),
            std::make_move_iterator(autocomplete_lines.begin()),
            std::make_move_iterator(autocomplete_lines.end()));
        return transcript_result;
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
            const auto editor_cancels_interrupt =
                editor_.autocomplete_open() &&
                keybindings_->matches(*key, "tui.select.cancel");
            if (keybindings_->matches(*key, "app.interrupt") &&
                !editor_cancels_interrupt) {
                // Autocomplete cancellation stays in the view. Interrupt
                // Admission owns every later precedence decision and receives
                // Bash mode as it existed at key-press time.
                invoke_interrupt(EditorInterruptRequest{
                    .pending_bash_text = editor_.expanded_text(),
                    .editor_revision = editor_revision_,
                    .pending_bash = unsubmitted_bash_mode(),
                });
                return;
            }
            if (keybindings_->matches(*key, "app.message.followUp")) {
                invoke_follow_up();
                return;
            }
            if (keybindings_->matches(*key, "app.clipboard.pasteImage")) {
                invoke_action(
                    on_clipboard_paste_,
                    "Native TUI clipboard callback failed");
                return;
            }
            if (keybindings_->matches(*key, "app.message.dequeue")) {
                invoke_action(on_dequeue_, "Native TUI dequeue callback failed");
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

    void invoke_submit(EditorSubmissionRequest request) {
        invoke_submission(
            on_submit_,
            std::move(request),
            "Native TUI submit callback failed");
    }

    void invoke_follow_up() {
        auto text = trim_editor_submission(editor_.expanded_text());
        if (text.empty()) return;
        editor_.set_text({});
        invoke_submission(
            on_follow_up_,
            EditorSubmissionRequest{
                .text = std::move(text),
                .editor_revision = editor_revision_,
            },
            "Native TUI follow-up callback failed");
    }

    void invoke_submission(
        SubmitSink& sink,
        EditorSubmissionRequest request,
        std::string_view failure_message) {
        if (!sink) return;
        try {
            sink(std::move(request));
        } catch (const std::exception& error) {
            record_callback_error(std::string(failure_message), error.what());
        } catch (...) {
            record_callback_error(std::string(failure_message));
        }
    }

    void invoke_interrupt(EditorInterruptRequest request) {
        if (!on_interrupt_) return;
        try {
            on_interrupt_(std::move(request));
        } catch (const std::exception& error) {
            record_callback_error(
                "Native TUI interrupt callback failed",
                error.what());
        } catch (...) {
            record_callback_error("Native TUI interrupt callback failed");
        }
    }

    void restore_editor_text(const std::vector<std::string>& messages) {
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

    [[nodiscard]] bool unsubmitted_bash_mode() const {
        return user_bash_editor_mode(editor_.expanded_text(), user_bash_available_);
    }

    [[nodiscard]] std::unique_ptr<cch::tui::Loader> make_bash_loader(
        bool exclude_from_context) {
        const auto cancel_key = keybindings_->key_text("app.interrupt");
        cch::tui::LoaderOptions options;
        // Render scheduling is thread-safe; failures surface on the next
        // render rather than through callback state.
        options.request_render = [this] {
            if (!on_invalidate_) return;
            try {
                on_invalidate_();
            } catch (...) {
            }
        };
        options.spinner_style = theme_->foreground_hook(
            exclude_from_context ? ThemeToken::Dim : ThemeToken::BashMode);
        options.message_style = theme_->foreground_hook(ThemeToken::Muted);
        options.message = std::format(
            "Running... ({} to cancel)",
            cancel_key.empty() ? "Unbound" : cancel_key);
        return std::make_unique<cch::tui::Loader>(std::move(options));
    }

    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings_;
    cch::tui::TerminalCapabilities terminal_capabilities_;
    ActionSink on_invalidate_;
    SubmitSink on_submit_;
    SubmitSink on_follow_up_;
    ActionSink on_clipboard_paste_;
    ActionSink on_dequeue_;
    InterruptSink on_interrupt_;
    ActionSink on_exit_;
    bool user_bash_available_{false};
    std::optional<util::Error> callback_error_;
    mutable std::mutex mutex_;
    Transcript transcript_;
    const LiveTheme* theme_; // must outlive the view: controller-owned live theme.
    cch::tui::Editor editor_;
    std::size_t editor_revision_{0};
    std::vector<std::string> pending_steering_;
    std::vector<std::string> pending_follow_up_;
    // Declared after every sink its loader may invoke so destruction stops
    // the loader first.
    PendingUserBashPresentation pending_bash_;
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
        clipboard_reader_ = std::move(config.clipboard_reader);
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
        if (config.initial_prompt) {
            submit(
                std::move(*config.initial_prompt),
                InputSubmission::Ordinary,
                std::move(config.initial_prompt_options),
                SubmissionOrigin::InitialPrompt);
        }
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
        std::vector<std::string_view> actions{
            "app.interrupt",
            "app.clear",
            "app.exit",
            "app.tools.expand",
            "app.thinking.toggle",
            "app.message.followUp",
            "app.message.dequeue",
        };
        if (clipboard_reader_) actions.push_back("app.clipboard.pasteImage");
        if (auto definitions = baseline_application_keybindings(actions, config.platform); !definitions) {
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

        // The Native TUI reads only the global settings scope (the theme is
        // global-only) and writes theme selections surgically through the
        // two-scope manager with the project scope untrusted.
        auto settings_manager = coding_agent::SettingsManager::create(
            /* cwd */ {},
            config.agent_config_directory,
            /* project_trusted */ false);
        for (const auto& settings_error : settings_manager.errors()) {
            if (settings_error.scope == coding_agent::SettingsScope::Global) {
                return std::unexpected(util::make_error(
                    util::ErrorCode::JsonParse,
                    "could not load global settings",
                    settings_error.message));
            }
        }
        const auto capabilities = terminal_.capabilities();
        ThemeCatalogRequest request;
        request.agent_config_directory = config.agent_config_directory;
        request.user_active_theme = settings_manager.global_settings().theme;
        request.terminal_capabilities = capabilities;
        if (auto catalog = load_theme_catalog(std::move(request)); !catalog) {
            return std::unexpected(catalog.error());
        } else {
            diagnostics.themes = catalog->diagnostics;
            ThemeSelectionCommitter committer;
            if (!config.agent_config_directory.empty()) {
                committer = [manager = std::move(settings_manager)](std::string_view name) mutable {
                    return manager.set_theme(coding_agent::SettingsScope::Global, name);
                };
            }
            theme_controller_.emplace(
                std::move(*catalog),
                tui_,
                capabilities.color,
                std::move(committer));
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
            [weak](EditorSubmissionRequest request) {
                if (const auto self = weak.lock()) {
                    self->post_submit(
                        std::move(request),
                        InputSubmission::Ordinary);
                }
            },
            [weak](EditorSubmissionRequest request) {
                if (const auto self = weak.lock()) {
                    self->post_submit(
                        std::move(request),
                        InputSubmission::FollowUp);
                }
            },
            [weak] {
                if (const auto self = weak.lock()) self->post_clipboard_paste();
            },
            [weak] {
                if (const auto self = weak.lock()) self->post_dequeue();
            },
            [weak](EditorInterruptRequest request) {
                if (const auto self = weak.lock()) {
                    self->post_interrupt(std::move(request));
                }
            },
            [weak] {
                if (const auto self = weak.lock()) self->post_exit();
            },
            detail::AgentSessionInteractiveAccess::has_user_shell(session_),
            command_autocomplete_items(commands_, session_.templates(), session_.skills()),
            terminal_.capabilities(),
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
        const auto snapshot = session_.snapshot();
        view_->initialize(snapshot);
        view_->set_pending_input(snapshot.agent_state.input_queues);
        for (const auto& diagnostic : snapshot.agent_state.diagnostics) {
            auto text = combined_error_text(diagnostic);
            view_->append_diagnostic(text);
            displayed_agent_diagnostics_.push_back(std::move(text));
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

    void post_submit(EditorSubmissionRequest request, InputSubmission submission) {
        const auto weak = weak_from_this();
        boost::asio::post(
            executor_,
            [weak, request = std::move(request), submission]() mutable {
                if (const auto self = weak.lock()) {
                    self->submit(
                        std::move(request.text),
                        submission,
                        {},
                        SubmissionOrigin::FocusedEditor,
                        request.editor_revision);
                }
            });
    }

    void post_clipboard_paste() {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak] {
            const auto self = weak.lock();
            if (!self || !self->running_ || self->clipboard_reader_ == nullptr ||
                self->clipboard_read_active_) {
                return;
            }
            self->clipboard_read_active_ = true;
            boost::asio::co_spawn(
                self->executor_,
                self->paste_from_clipboard(),
                [weak](std::exception_ptr exception, util::ExpectedVoid result) {
                    const auto state = weak.lock();
                    if (!state) return;
                    state->clipboard_read_active_ = false;
                    std::optional<util::Error> ignored_failure;
                    if (exception) {
                        try {
                            std::rethrow_exception(exception);
                        } catch (const std::exception& error) {
                            ignored_failure = util::make_error(
                                util::ErrorCode::Unknown,
                                "clipboard paste failed",
                                error.what());
                        } catch (...) {
                            ignored_failure = util::make_error(
                                util::ErrorCode::Unknown,
                                "clipboard paste failed");
                        }
                    } else if (!result) {
                        ignored_failure = std::move(result.error());
                    }
                    // Baseline clipboard failures are intentionally silent.
                    (void)ignored_failure;
                });
        });
    }

    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> paste_from_clipboard() {
        try {
            auto image = co_await clipboard_reader_->read_image();
            if (image && *image && !(*image)->bytes.empty()) {
                const auto mime_type = sniff_supported_image_mime_type((*image)->bytes);
                const auto extension = mime_type
                    ? extension_for_image_mime_type(*mime_type)
                    : std::nullopt;
                if (extension) {
                    const auto path = write_clipboard_image((*image)->bytes, *extension);
                    if (path) {
                        if (running_ && view_ != nullptr) {
                            view_->insert_editor_text(path->string());
                            tui_.invalidate();
                        }
                        co_return util::ExpectedVoid{};
                    }
                }
            }
        } catch (const std::exception& error) {
            const auto ignored = util::make_error(
                util::ErrorCode::Unknown,
                "clipboard image read failed",
                error.what());
            (void)ignored;
        } catch (...) {
            const auto ignored = util::make_error(
                util::ErrorCode::Unknown,
                "clipboard image read failed");
            (void)ignored;
        }

        try {
            auto text = co_await clipboard_reader_->read_text();
            if (text && *text && !(*text)->empty() && running_ && view_ != nullptr) {
                view_->insert_editor_text(std::move(**text));
                tui_.invalidate();
            }
        } catch (const std::exception& error) {
            const auto ignored = util::make_error(
                util::ErrorCode::Unknown,
                "clipboard text read failed",
                error.what());
            (void)ignored;
        } catch (...) {
            const auto ignored = util::make_error(
                util::ErrorCode::Unknown,
                "clipboard text read failed");
            (void)ignored;
        }
        co_return util::ExpectedVoid{};
    }

    void post_dequeue() {
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak] {
            if (const auto self = weak.lock()) self->dequeue_pending_input(true);
        });
    }

    void post_interrupt(EditorInterruptRequest request) {
        const auto weak = weak_from_this();
        const auto prompt_generation = interrupt_admission_.generation();
        boost::asio::post(
            executor_,
            [weak, prompt_generation, request = std::move(request)] {
                if (const auto self = weak.lock()) {
                    self->request_interrupt(prompt_generation, request);
                }
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
            .user_bash_available =
                detail::AgentSessionInteractiveAccess::has_user_shell(session_),
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

    [[nodiscard]] InteractionActivity interaction_activity() const {
        return InteractionActivity{
            .user_shell_available =
                detail::AgentSessionInteractiveAccess::has_user_shell(session_),
            .user_bash_active = user_bash_active_,
            .prompt_active = prompt_active_,
            .interrupt_requested = interrupt_admission_.interrupt_requested(),
        };
    }

    [[nodiscard]] bool dispatch_user_bash(const std::string& text, SubmissionOrigin origin) {
        auto route = route_user_bash(text, origin, interaction_activity());
        if (!route) return false;
        if (const auto* busy = std::get_if<RestoreUserBashBusy>(&*route)) {
            view_->restore_submitted_text(busy->recall);
            view_->append_user_bash_diagnostic(
                "A User Bash command is already in flight");
            tui_.invalidate();
            return true;
        }
        auto invocation = std::get<LaunchUserBash>(std::move(*route)).invocation;

        user_bash_active_ = true;
        // The original trimmed submission is what failure restores to the
        // editor (pi setText(text), ADR 0028) — never a re-serialized form.
        auto recall = trim_editor_submission(text);
        const auto self = shared_from_this();
        boost::asio::co_spawn(
            executor_,
            [self,
             invocation = std::move(invocation),
             recall = std::move(recall)]() mutable -> boost::asio::awaitable<void> {
                auto result = co_await detail::AgentSessionInteractiveAccess::run_user_bash(
                    self->session_,
                    std::move(invocation.command),
                    invocation.exclude_from_context,
                    [self](
                        const runtime::UserBashProgress& progress) -> util::ExpectedVoid {
                        if (self->running_ && self->view_ != nullptr) {
                            self->view_->set_user_bash_progress(progress);
                            self->tui_.invalidate();
                        }
                        return {};
                    });
                self->user_bash_finished(std::move(result), recall);
            },
            [weak = weak_from_this()](std::exception_ptr exception) {
                if (!exception) return;
                if (const auto self = weak.lock()) {
                    self->user_bash_active_ = false;
                    if (self->view_ != nullptr && self->running_) {
                        self->view_->clear_user_bash_progress();
                        self->view_->append_user_bash_diagnostic(
                            "Native TUI User Bash coroutine failed");
                        self->tui_.invalidate();
                    }
                    if (self->exit_requested_) self->signal_exit();
                }
            });
        return true;
    }

    void request_interrupt(
        std::size_t prompt_generation,
        const EditorInterruptRequest& request) {
        if (!running_ || exit_requested_) return;
        switch (interrupt_admission_.admit(
            interaction_activity(),
            prompt_generation,
            request.pending_bash)) {
        case InterruptRoute::AbortAgentRun:
            // pi restores queued input before aborting the Agent run.
            dequeue_pending_input(false);
            session_.abort();
            return;
        case InterruptRoute::CancelUserBash:
            detail::AgentSessionInteractiveAccess::cancel_user_bash(session_);
            return;
        case InterruptRoute::ClearPendingBash:
            cleared_editor_revision_ = request.editor_revision;
            if (view_ != nullptr) {
                view_->clear_pending_bash(request);
                tui_.invalidate();
            }
            return;
        case InterruptRoute::None:
            return;
        }
    }

    void submit(
        std::string text,
        InputSubmission submission,
        PromptOptions options = {},
        SubmissionOrigin origin = SubmissionOrigin::FocusedEditor,
        std::optional<std::size_t> editor_revision = std::nullopt) {
        if (!running_ || view_ == nullptr || text.empty()) return;
        if (origin == SubmissionOrigin::FocusedEditor && editor_revision &&
            cleared_editor_revision_ == editor_revision) {
            return;
        }
        if (dispatch_user_bash(text, origin)) return;
        if (dispatch_command(text)) return;
        const auto route = route_prompt(submission, interaction_activity());
        if (route == PromptRoute::RestoreInterrupted) {
            view_->restore_submitted_text(text);
            view_->append_diagnostic("A prompt is already in flight");
            tui_.invalidate();
            return;
        }
        if (route == PromptRoute::QueueSteering || route == PromptRoute::QueueFollowUp) {
            if (auto admitted = route == PromptRoute::QueueFollowUp
                    ? session_.follow_up(text)
                    : session_.steer(text);
                !admitted) {
                view_->restore_submitted_text(text);
                view_->append_diagnostic(bounded_redacted_presentation(std::format(
                    "Unable to queue {} input: {}",
                    route == PromptRoute::QueueFollowUp ? "follow-up" : "steering",
                    combined_error_text(admitted.error()))));
            }
            sync_pending_input();
            tui_.invalidate();
            return;
        }

        interrupt_admission_.note_prompt_started();
        prompt_active_ = true;
        const auto self = shared_from_this();
        boost::asio::co_spawn(
            executor_,
            [self,
             text = std::move(text),
             options = std::move(options)]() mutable -> boost::asio::awaitable<void> {
                util::ExpectedVoid result;
                try {
                    result = co_await self->session_.prompt(text, std::move(options));
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

    void dequeue_pending_input(bool announce) {
        if (!running_ || view_ == nullptr || !session_.is_open()) return;
        const auto snapshot = session_.snapshot();
        std::vector<std::string> restored;
        if (auto collected = queued_editor_texts(snapshot.agent_state.input_queues);
            !collected) {
            view_->append_diagnostic(collected.error().message);
            tui_.invalidate();
            return;
        } else {
            restored = std::move(*collected);
        }
        if (restored.empty()) {
            if (announce) view_->append_frontend_message("No queued messages to restore");
            sync_pending_input();
            tui_.invalidate();
            return;
        }
        if (auto cleared = session_.clear_input_queues(); !cleared) {
            view_->append_diagnostic(bounded_redacted_presentation(std::format(
                "Unable to restore queued input: {}",
                combined_error_text(cleared.error()))));
            sync_pending_input();
            tui_.invalidate();
            return;
        }
        view_->restore_queued_text(restored);
        if (announce) {
            view_->append_frontend_message(std::format(
                "Restored {} queued message{} to editor",
                restored.size(),
                restored.size() == 1 ? "" : "s"));
        }
        sync_pending_input();
        tui_.invalidate();
    }

    void prompt_launch_failed(std::exception_ptr exception) {
        interrupt_admission_.note_prompt_finished();
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
        interrupt_admission_.note_prompt_finished();
        prompt_active_ = false;
        sync_session_observations();
        if (!result && view_ != nullptr && running_) {
            view_->append_diagnostic(combined_error_text(result.error()));
            view_->restore_submitted_text(submitted_text);
            tui_.invalidate();
        }
        if (exit_requested_ && !user_bash_active_) signal_exit();
    }

    void user_bash_finished(
        util::Expected<runtime::UserBashCompletion> result,
        const std::string& recall) {
        user_bash_active_ = false;
        if (view_ != nullptr && running_) {
            if (result) {
                view_->commit_user_bash(
                    ai::MessageVariant{std::move(result->message)});
                if (result->diagnostic) {
                    view_->append_user_bash_diagnostic(
                        combined_error_text(*result->diagnostic));
                }
            } else {
                view_->clear_user_bash_progress();
                view_->append_user_bash_diagnostic(combined_error_text(result.error()));
                if (!recall.empty()) {
                    view_->restore_submitted_text(recall);
                }
            }
            tui_.invalidate();
        }
        if (exit_requested_ && !prompt_active_) signal_exit();
    }

    void sync_pending_input() {
        if (!running_ || view_ == nullptr || !session_.is_open()) return;
        view_->set_pending_input(session_.snapshot().agent_state.input_queues);
    }

    void sync_session_observations() {
        if (!running_ || view_ == nullptr || !session_.is_open()) return;
        const auto snapshot = session_.snapshot();
        view_->set_pending_input(snapshot.agent_state.input_queues);

        std::vector<std::string> current;
        current.reserve(snapshot.agent_state.diagnostics.size());
        for (const auto& diagnostic : snapshot.agent_state.diagnostics) {
            current.push_back(combined_error_text(diagnostic));
        }

        auto overlap = std::min(displayed_agent_diagnostics_.size(), current.size());
        while (overlap > 0 && !std::equal(
                displayed_agent_diagnostics_.end() - static_cast<std::ptrdiff_t>(overlap),
                displayed_agent_diagnostics_.end(),
                current.begin())) {
            --overlap;
        }
        for (auto index = overlap; index < current.size(); ++index) {
            view_->append_diagnostic(current[index]);
        }
        displayed_agent_diagnostics_ = std::move(current);
    }

    void on_event(const agent::AgentLifecycleEvent& event) {
        if (!running_ || view_ == nullptr) return;
        view_->apply_event(event);
        sync_session_observations();
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
        if (!prompt_active_ && !user_bash_active_) signal_exit();
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
    std::unique_ptr<AsyncClipboardReader> clipboard_reader_;
    boost::asio::any_io_executor executor_;
    boost::asio::steady_timer exit_wait_;
    std::optional<EventSubscription> subscription_;
    InteractiveView* view_{nullptr}; // aliases the child owned by tui_.
    cch::tui::Overlay* active_overlay_{nullptr}; // aliases an overlay owned by tui_.
    std::atomic<bool> running_{false};
    std::atomic<bool> prompt_active_{false};
    std::atomic<bool> user_bash_active_{false};
    InterruptAdmission interrupt_admission_;
    // Suppresses a submission already decoded from Bash text cleared by an
    // earlier key-time interrupt decision.
    std::optional<std::size_t> cleared_editor_revision_;
    std::vector<std::string> displayed_agent_diagnostics_;
    bool tui_started_{false};
    bool exit_requested_{false};
    bool clipboard_read_active_{false};
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
