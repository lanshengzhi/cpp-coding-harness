#include "ClipboardPaste.hpp"

#include "coding_agent/ImageInput.hpp"
#include "coding_agent/tui/ClipboardProcess.hpp"
#include "support/UniqueFd.hpp"

#include <cch/support/Error.hpp>
#include <cch/tui/NativeClipboard.hpp>
#include "support/AsyncResultBridge.hpp"

#include <cerrno>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace cch::coding_agent::tui {
namespace {

[[nodiscard]] support::Expected<std::filesystem::path> write_clipboard_image(
        std::span<const std::uint8_t> bytes, std::string_view extension) {
    std::error_code temp_error;
    const auto temp_directory = std::filesystem::temp_directory_path(temp_error);
    if (temp_error || temp_directory.empty()) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Process, "clipboard temporary directory is unavailable", temp_error.message()));
    }

    for (std::size_t attempt = 0; attempt < 16; ++attempt) {
        std::filesystem::path path;
        path = temp_directory / std::format("pi-clipboard-{}{}", make_clipboard_unique_token(), extension);
        support::UniqueFd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
        if (!fd) {
            if (errno == EEXIST) continue;
            return std::unexpected(support::make_error(support::ErrorCode::Process,
                    "could not create clipboard image file",
                    std::error_code(errno, std::generic_category()).message()));
        }

        std::size_t written = 0;
        while (written < bytes.size()) {
            const auto count = ::write(fd.get(), bytes.data() + written, bytes.size() - written);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) {
                const auto write_error = errno;
                (void)fd.close();
                std::error_code remove_error;
                std::filesystem::remove(path, remove_error);
                return std::unexpected(support::make_error(support::ErrorCode::Process,
                        "could not write clipboard image file",
                        std::error_code(write_error, std::generic_category()).message()));
            }
            written += static_cast<std::size_t>(count);
        }
        if (fd.close() != 0) {
            const auto close_error = errno;
            std::error_code remove_error;
            std::filesystem::remove(path, remove_error);
            return std::unexpected(support::make_error(support::ErrorCode::Process,
                    "could not finish clipboard image file",
                    std::error_code(close_error, std::generic_category()).message()));
        }
        return path;
    }
    return std::unexpected(
            support::make_error(support::ErrorCode::Process, "could not allocate a unique clipboard image path"));
}

} // namespace

boost::asio::awaitable<std::optional<std::string>> read_clipboard_insert_content(
        cch::tui::NativeClipboard& native_clipboard, std::stop_token stop_token) {
    auto image_result = co_await support::detail::await_async_result(native_clipboard.get_image(stop_token));
    if (stop_token.stop_requested()) co_return std::nullopt;
    if (image_result) {
        if (const auto* image = std::get_if<std::optional<std::vector<std::uint8_t>>>(&*image_result);
                image != nullptr && *image && !(*image)->empty()) {
            const auto mime_type = sniff_supported_image_mime_type(**image);
            const auto extension = mime_type ? extension_for_image_mime_type(*mime_type) : std::nullopt;
            if (extension) {
                const auto path = write_clipboard_image(**image, *extension);
                if (path) {
                    if (stop_token.stop_requested()) {
                        std::error_code remove_error;
                        std::filesystem::remove(*path, remove_error);
                        co_return std::nullopt;
                    }
                    co_return path->string();
                }
            }
        }
    }

    if (stop_token.stop_requested()) co_return std::nullopt;
    auto text_result = co_await support::detail::await_async_result(native_clipboard.get_text(stop_token));
    if (stop_token.stop_requested()) co_return std::nullopt;
    if (text_result) {
        if (auto* text = std::get_if<std::optional<std::string>>(&*text_result);
                text != nullptr && *text && !(*text)->empty()) {
            co_return std::move(**text);
        }
    }
    co_return std::nullopt;
}

} // namespace cch::coding_agent::tui
