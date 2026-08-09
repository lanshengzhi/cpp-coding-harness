#pragma once

#include <cch/tui/Keybindings.hpp>

#include <memory>
#include <mutex>

namespace cch::coding_agent::tui {

/// pi's mutable `KeybindingsManager` consumption shape over the immutable
/// resolved `KeybindingRegistry` (ADR 0035): every durable view component
/// (header hints, chat, tool/bash execution, editor) observes one shared
/// slot, and a `/reload` replaces the current registry in place so all
/// consumers see the new bindings live — the C++ equivalent of pi's
/// `KeybindingsManager.reload()` → `setUserBindings(...)` shared-manager
/// mutation (issue #418).
///
/// Thread contract: `replace` is confined to the executor thread (the
/// `/reload` flow), and `registry()`/`get()` are safe from any thread. Callers
/// that hold a `registry()` reference for the duration of a render or input
/// dispatch must serialize against `replace` (the owning `InteractiveView`
/// does this through its view mutex); `get()` returns a strong reference that
/// keeps the current registry alive for ephemeral consumers (selectors).
class SharedKeybindings final {
public:
    SharedKeybindings() = default;
    explicit SharedKeybindings(
        std::shared_ptr<const cch::tui::KeybindingRegistry> current)
        : current_(std::move(current)) {}

    /// pi `KeybindingsManager.reload()`: swap the current registry. The
    /// previous registry is released once no observer references it.
    void replace(std::shared_ptr<const cch::tui::KeybindingRegistry> current) {
        std::lock_guard lock(mutex_);
        current_ = std::move(current);
    }

    /// The current registry as a strong reference (ephemeral consumers).
    [[nodiscard]] std::shared_ptr<const cch::tui::KeybindingRegistry> get() const {
        std::lock_guard lock(mutex_);
        return current_;
    }

    /// The current registry by reference. Safe only while the caller
    /// serializes against `replace` (the view mutex).
    [[nodiscard]] const cch::tui::KeybindingRegistry& registry() const {
        std::lock_guard lock(mutex_);
        return *current_;
    }

    [[nodiscard]] explicit operator bool() const { return get() != nullptr; }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<const cch::tui::KeybindingRegistry> current_;
};

} // namespace cch::coding_agent::tui
