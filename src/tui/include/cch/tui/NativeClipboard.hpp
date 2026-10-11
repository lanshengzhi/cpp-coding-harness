#pragma once

#include <cch/support/AsyncResult.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

namespace cch::tui {

/// Native clipboard operations exposed by an injected platform helper (pi
/// `NativeClipboard`). The provider itself is optional: no provider means the
/// native clipboard is unavailable. A successful read with no content returns
/// `std::nullopt`; transfer failures return `support::Error`. Implementations
/// observe the stop token and complete cancelled operations with a Cancelled
/// error without retaining or calling the consumer after completion.
struct NativeClipboardUnavailable {};

using NativeClipboardTextReadVariant = std::variant<NativeClipboardUnavailable, std::optional<std::string>>;
using NativeClipboardImageReadVariant =
        std::variant<NativeClipboardUnavailable, std::optional<std::vector<std::uint8_t>>>;

class NativeClipboard {
public:
    virtual ~NativeClipboard() = default;

    [[nodiscard]] virtual support::AsyncResult<NativeClipboardTextReadVariant> get_text(std::stop_token stop_token) = 0;
    [[nodiscard]] virtual support::AsyncResult<NativeClipboardImageReadVariant> get_image(
            std::stop_token stop_token) = 0;

    /// The outer optional is operation availability; an available read whose
    /// result is `std::nullopt` has no file URLs on the clipboard. Linux native
    /// helpers do not expose this operation.
    [[nodiscard]] virtual std::optional<support::AsyncResult<std::optional<std::vector<std::filesystem::path>>>>
    get_file_paths(std::stop_token stop_token) {
        (void)stop_token;
        return std::nullopt;
    }

    /// The outer optional is operation availability. Linux native helpers leave
    /// writes to clipboard command-line tools so those tools retain ownership.
    [[nodiscard]] virtual std::optional<support::AsyncResult<void>> set_text(std::string text) {
        (void)text;
        return std::nullopt;
    }
};

using NativeClipboardLoader = std::move_only_function<std::unique_ptr<NativeClipboard>()>;

/// Apply pi's Linux native clipboard availability check before loading a
/// platform helper. An unset or empty DISPLAY, an absent loader, or a loader
/// that cannot provide a helper returns nullptr.
[[nodiscard]] std::unique_ptr<NativeClipboard> get_native_clipboard(NativeClipboardLoader load_native_clipboard);

} // namespace cch::tui
