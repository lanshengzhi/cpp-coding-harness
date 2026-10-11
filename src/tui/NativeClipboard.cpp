#include <cch/tui/NativeClipboard.hpp>

#include <cstdlib>

namespace cch::tui {

std::unique_ptr<NativeClipboard> get_native_clipboard(NativeClipboardLoader load_native_clipboard) {
    const char* display = std::getenv("DISPLAY");
    if (display == nullptr || display[0] == '\0' || !load_native_clipboard) return nullptr;
    return load_native_clipboard();
}

} // namespace cch::tui
