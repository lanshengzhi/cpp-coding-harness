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
    explicit AltScreenFlashContainer(std::function<void()> request_render);
    ~AltScreenFlashContainer() override;

    AltScreenFlashContainer(const AltScreenFlashContainer&) = delete;
    AltScreenFlashContainer& operator=(const AltScreenFlashContainer&) = delete;

    void set_scheduler(TuiScheduler* scheduler);
    void set_style(std::function<std::string(std::string)> style);
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

    std::function<void()> request_render_;
    std::function<std::string(std::string)> style_;
    TuiScheduler* scheduler_{nullptr};
    std::vector<Entry> entries_;
    std::size_t next_id_{0};
    std::size_t generation_{0};
};

} // namespace cch::tui
