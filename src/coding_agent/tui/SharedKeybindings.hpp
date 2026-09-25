#pragma once

#include <cch/tui/Keybindings.hpp>

#include <memory>
#include <mutex>
#include <vector>

namespace cch::coding_agent::tui {

struct HotkeyHelpRow;

/// pi's mutable `KeybindingsManager` consumption shape over the immutable
/// resolved `KeybindingRegistry` (ADR 0035): every durable view component
/// (header hints, chat, tool/bash execution, editor) observes one shared
/// slot, and a `/reload` replaces the current registry in place so all
/// consumers see the new bindings live — the C++ equivalent of pi's
/// `KeybindingsManager.reload()` → `setUserBindings(...)` shared-manager
/// mutation (issue #418).
///
/// Thread contract: `replace` is confined to the executor thread (the
/// `/reload` flow). `get()` and `help()` return strong references and are safe
/// from any thread. A caller holding the `registry()` reference across a
/// render or input dispatch must serialize it with `replace`; the owning
/// `InteractiveView` supplies that serialization.
class SharedKeybindings final {
public:
    SharedKeybindings() = default;
    explicit SharedKeybindings(
        std::shared_ptr<const cch::tui::KeybindingRegistry> current)
        : current_(std::move(current)) {}

    SharedKeybindings(std::shared_ptr<const cch::tui::KeybindingRegistry> current,
            std::shared_ptr<const std::vector<HotkeyHelpRow>> help)
        : current_(std::move(current)), help_(std::move(help)) {}

    /// pi `KeybindingsManager.reload()`: swap the current registry. The
    /// previous registry is released once no observer references it.
    void replace(std::shared_ptr<const cch::tui::KeybindingRegistry> current,
            std::shared_ptr<const std::vector<HotkeyHelpRow>> help = {}) {
        std::lock_guard lock(mutex_);
        current_ = std::move(current);
        help_ = std::move(help);
    }

    /// The current registry as a strong reference (ephemeral consumers).
    [[nodiscard]] std::shared_ptr<const cch::tui::KeybindingRegistry> get() const {
        std::lock_guard lock(mutex_);
        return current_;
    }

    /// The current registry by reference. The caller must serialize this
    /// reference against `replace` for the duration of its use.
    [[nodiscard]] const cch::tui::KeybindingRegistry& registry() const {
        std::lock_guard lock(mutex_);
        return *current_;
    }

    [[nodiscard]] std::shared_ptr<const std::vector<HotkeyHelpRow>> help() const {
        std::lock_guard lock(mutex_);
        return help_;
    }

    [[nodiscard]] explicit operator bool() const { return get() != nullptr; }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<const cch::tui::KeybindingRegistry> current_;
    std::shared_ptr<const std::vector<HotkeyHelpRow>> help_;
};

} // namespace cch::coding_agent::tui
