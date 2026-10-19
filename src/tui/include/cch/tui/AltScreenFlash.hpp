#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/TuiScheduler.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <functional>
#include <string>
#include <vector>

namespace cch::tui {

/// Timed inverse-video messages composed by an alternate-screen root.
class AltScreenFlashContainer final : public Component {
public:
    explicit AltScreenFlashContainer(std::move_only_function<void()> request_render);
    ~AltScreenFlashContainer() override;

    AltScreenFlashContainer(const AltScreenFlashContainer&) = delete;
    AltScreenFlashContainer& operator=(const AltScreenFlashContainer&) = delete;

    void set_scheduler(TuiScheduler* scheduler);
    void set_style(std::move_only_function<std::string(std::string)> style);
    /// Post a transient message. When no scheduler is attached the entry has
    /// no timer: it persists until dispose() (frozen's setTimeout always
    /// expires regardless of TUI state — a toolkit-embedder deviation, not
    /// reachable in production where a scheduler is always installed before
    /// start()).
    void flash(std::string message, std::chrono::milliseconds duration = std::chrono::milliseconds{1000});
    void dispose() noexcept;

    [[nodiscard]] support::Expected<RenderResult> render(std::size_t width) override;
    void invalidate() override {}

private:
    struct Entry {
        std::size_t id{0};
        std::size_t generation{0};
        std::string message;
        std::chrono::steady_clock::time_point deadline{};
        std::unique_ptr<TuiScheduledHandle> timer;
    };

    void schedule_expiry(Entry& entry);
    void expire(std::size_t id, std::size_t generation);

    std::move_only_function<void()> request_render_;
    std::move_only_function<std::string(std::string)> style_;
    /// §7.5: non-owning; must outlive this container or be detached first via
    /// `set_scheduler(nullptr)` (also invoked by `dispose()`). All callbacks
    /// scheduled on it are cancelled (handle + generation guard) before this
    /// container or the scheduler is destroyed.
    TuiScheduler* scheduler_{nullptr};
    std::vector<Entry> entries_;
    std::size_t next_id_{0};
    std::size_t generation_{0};
};

} // namespace cch::tui
