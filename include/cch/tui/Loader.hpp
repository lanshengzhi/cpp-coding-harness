#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Style.hpp>

#include <cch/support/Error.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cch::tui {

using AnimationTickSink = std::move_only_function<void()>;
using RenderRequestSink = std::move_only_function<void()>;

/// Repeating-timer capability used by Loader. Methods must support concurrent
/// and tick-reentrant start/stop calls. stop() synchronously prevents future
/// ticks and waits for an in-flight tick, except when called by that tick.
class AnimationTimer {
public:
    virtual ~AnimationTimer() = default;

    [[nodiscard]] virtual support::ExpectedVoid start(
        std::chrono::milliseconds interval,
        AnimationTickSink tick) = 0;
    virtual void stop() = 0;
};

struct LoaderIndicatorOptions {
    std::optional<std::vector<std::string>> frames{std::nullopt};
    std::chrono::milliseconds interval{80};
};

struct LoaderOptions {
    RenderRequestSink request_render{};
    TextStyleHook spinner_style{};
    TextStyleHook message_style{};
    std::string message{"Loading..."};
    std::optional<LoaderIndicatorOptions> indicator{std::nullopt};
    std::unique_ptr<AnimationTimer> animation_timer{};
};

/// A thread-safe loader that requests rendering when its presentation changes.
class Loader final : public Component {
public:
    explicit Loader(LoaderOptions options = {});
    Loader(Loader&&) noexcept;
    Loader& operator=(Loader&&) noexcept;
    ~Loader() override;

    Loader(const Loader&) = delete;
    Loader& operator=(const Loader&) = delete;

    void start();
    void stop();
    [[nodiscard]] bool running() const;
    void set_message(std::string message);
    void set_indicator(std::optional<LoaderIndicatorOptions> indicator = std::nullopt);

    [[nodiscard]] support::Expected<RenderResult> render(std::size_t width) override;
    void invalidate() override;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace cch::tui
