#pragma once

#include <cch/tui/Component.hpp>

#include <cstddef>
#include <memory>
#include <optional>

namespace cch::tui {

class MainScreen;
class AltScreen;
class Overlay;

/// Terminal-relative bounds from the most recent layout pass.
struct OverlayBounds {
    std::size_t row{0};
    std::size_t col{0};
    std::size_t width{0};
    std::size_t height{0};
};

namespace detail {
class OverlayHost {
public:
    virtual ~OverlayHost() = default;
    virtual void remove_overlay_internal(Overlay* overlay) = 0;
    virtual void hide_overlay_internal(Overlay* overlay, bool hidden) = 0;
    virtual void focus_overlay_internal(Overlay* overlay) = 0;
    virtual void unfocus_overlay_internal(Overlay* overlay, std::optional<Component*> target) = 0;
    [[nodiscard]] virtual bool is_overlay_focused_internal(const Overlay* overlay) const = 0;
    [[nodiscard]] virtual std::optional<OverlayBounds> overlay_bounds_internal(const Overlay* overlay) const = 0;
};

struct OverlayHandleContext {
    OverlayHost* owner{nullptr};
};

struct OverlayHandleState {
    bool hidden{false};
};
} // namespace detail

/// Controls one overlay registered with an ITui. A handle safely becomes inert
/// when its overlay or owning TUI is destroyed.
class OverlayHandle final {
public:
    OverlayHandle() noexcept = default;

    void hide() const;
    /// Toggle temporary visibility without removing this overlay.
    void set_hidden(bool hidden) const;
    [[nodiscard]] bool is_hidden() const;
    void focus() const;
    /// An absent target releases to the overlay's previous focus target. A
    /// present null target explicitly leaves focus empty.
    /// Explicitly release focus to the fallback, a component, or null.
    void unfocus(std::optional<Component*> target = std::nullopt) const;
    [[nodiscard]] bool is_focused() const;
    [[nodiscard]] std::optional<OverlayBounds> get_bounds() const;

private:
    friend class MainScreen;
    friend class AltScreen;

    OverlayHandle(std::weak_ptr<detail::OverlayHandleContext> context,
            std::shared_ptr<detail::OverlayHandleState> state,
            Overlay* overlay) noexcept;

    std::weak_ptr<detail::OverlayHandleContext> context_;
    std::shared_ptr<detail::OverlayHandleState> state_;
    Overlay* overlay_{nullptr};
};

} // namespace cch::tui
