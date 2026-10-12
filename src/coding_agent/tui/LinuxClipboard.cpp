#include "LinuxClipboard.hpp"

#include "support/AsyncResultBridge.hpp"

#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <stop_token>
#include <utility>
#include <variant>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

/// pi `readClipboardText`'s per-command timeout.
constexpr std::chrono::milliseconds kReadCommandTimeout{5000};

/// pi's Linux read path as the injected clipboard capability. Only the
/// environment and an optional platform helper are known here; the process
/// environment is never consulted.
class LinuxClipboard final : public cch::tui::NativeClipboard {
public:
    LinuxClipboard(ClipboardEnvironment environment, std::unique_ptr<cch::tui::NativeClipboard> native_fallback)
        : environment_(std::move(environment)), native_fallback_(std::move(native_fallback)) {}

    [[nodiscard]] support::AsyncResult<cch::tui::NativeClipboardTextReadVariant> get_text(
            std::stop_token stop_token) override {
        return support::detail::make_async_result([this, stop_token]() mutable
                                                          -> boost::asio::awaitable<
                                                                  support::Expected<cch::tui::NativeClipboardTextReadVariant>> {
            for (const auto& request : read_commands()) {
                auto result = co_await run_clipboard_command(request, stop_token);
                if (result.cancelled) {
                    co_return std::unexpected(support::make_error(
                            support::ErrorCode::Cancelled, "clipboard read cancelled"));
                }
                // A helper that did not exit 0 has no result (pi `undefined`)
                // and the next helper is tried.
                if (result.exit_code != 0) continue;
                // pi `bytes.toString("utf8") || null`: a successful empty
                // buffer is an empty read that does not fall through.
                if (result.output.empty()) {
                    co_return cch::tui::NativeClipboardTextReadVariant{std::optional<std::string>{}};
                }
                co_return cch::tui::NativeClipboardTextReadVariant{
                        std::optional<std::string>{std::move(result.output)}};
            }
            if (native_fallback_ == nullptr) {
                static constexpr cch::tui::NativeClipboardTextReadVariant kNoContent{
                        std::optional<std::string>{}};
                co_return kNoContent;
            }
            co_return co_await support::detail::await_async_result(native_fallback_->get_text(stop_token));
        });
    }

    /// Images come from the injected platform helper only: this unit serves text
    /// through helper commands and never fabricates an image.
    [[nodiscard]] support::AsyncResult<cch::tui::NativeClipboardImageReadVariant> get_image(
            std::stop_token stop_token) override {
        if (native_fallback_ == nullptr) {
            return support::AsyncResult<cch::tui::NativeClipboardImageReadVariant>{
                    cch::tui::NativeClipboardImageReadVariant{cch::tui::NativeClipboardUnavailable{}}};
        }
        return native_fallback_->get_image(stop_token);
    }

    [[nodiscard]] std::optional<support::AsyncResult<std::optional<std::vector<std::filesystem::path>>>>
    get_file_paths(std::stop_token stop_token) override {
        if (native_fallback_ == nullptr) return std::nullopt;
        return native_fallback_->get_file_paths(stop_token);
    }

private:
    [[nodiscard]] std::vector<ClipboardCommandRequest> read_commands() const {
        std::vector<ClipboardCommandRequest> commands;
        if (!environment_.is_linux) return commands;
        const auto add = [this, &commands](const char* command, std::vector<std::string> args) {
            commands.push_back(ClipboardCommandRequest{
                    .command = command,
                    .args = std::move(args),
                    .input = std::nullopt,
                    .timeout = kReadCommandTimeout,
                    .max_buffer_bytes = kClipboardCommandMaxBufferBytes,
                    .search_path = environment_.search_path,
            });
        };
        if (!environment_.termux_version.empty()) add("termux-clipboard-get", {});
        if (!environment_.wayland_display.empty()) add("wl-paste", {"--no-newline", "--type", "text"});
        if (!environment_.display.empty()) {
            add("xclip", {"-selection", "clipboard", "-out"});
            add("xsel", {"--clipboard", "--output"});
        }
        return commands;
    }

    ClipboardEnvironment environment_{};
    std::unique_ptr<cch::tui::NativeClipboard> native_fallback_{nullptr};
};

} // namespace

std::unique_ptr<cch::tui::NativeClipboard> make_linux_clipboard_reader(
        ClipboardEnvironment environment,
        std::unique_ptr<cch::tui::NativeClipboard> native_fallback) {
    return std::make_unique<LinuxClipboard>(std::move(environment), std::move(native_fallback));
}

} // namespace cch::coding_agent::tui
