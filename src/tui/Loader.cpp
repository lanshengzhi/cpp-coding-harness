#include <cch/tui/Loader.hpp>

#include "tui/InteractionUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cch::tui {
namespace {

// Behavioral baseline: pi 864b35c loader.ts and cancellable-loader.ts.
constexpr std::chrono::milliseconds kDefaultInterval{80};
const std::vector<std::string> kDefaultFrames{
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏",
};

class SteadyAnimationTimer final : public AnimationTimer {
public:
    SteadyAnimationTimer() = default;
    SteadyAnimationTimer(SteadyAnimationTimer&&) = delete;
    SteadyAnimationTimer& operator=(SteadyAnimationTimer&&) = delete;
    ~SteadyAnimationTimer() override {
        stop();
    }

    SteadyAnimationTimer(const SteadyAnimationTimer&) = delete;
    SteadyAnimationTimer& operator=(const SteadyAnimationTimer&) = delete;

    [[nodiscard]] util::ExpectedVoid start(
        std::chrono::milliseconds interval,
        AnimationTickSink tick) override {
        auto state = std::make_shared<State>();
        state->interval = interval;
        state->tick = std::move(tick);
        std::shared_ptr<State> previous;
        {
            std::lock_guard lock(mutex_);
            previous = std::exchange(state_, state);
        }
        try {
            std::thread([state]() {
                {
                    std::lock_guard lock(state->mutex);
                    state->thread_id = std::this_thread::get_id();
                }
                std::unique_lock lock(state->mutex);
                while (!state->stopping) {
                    if (state->condition.wait_for(lock, state->interval, [&]() { return state->stopping; })) {
                        break;
                    }
                    lock.unlock();
                    if (state->tick) state->tick();
                    lock.lock();
                }
                state->active = false;
                lock.unlock();
                state->condition.notify_all();
            }).detach();
        } catch (const std::exception&) {
            {
                std::lock_guard lock(state->mutex);
                state->active = false;
            }
            state->condition.notify_all();
            {
                std::lock_guard lock(mutex_);
                if (state_ == state) state_.reset();
            }
            stop_state(std::move(previous));
            return std::unexpected(util::make_error(
                util::ErrorCode::Unknown,
                "TUI Loader could not start its animation timer"));
        }
        stop_state(std::move(previous));
        return {};
    }

    void stop() override {
        std::shared_ptr<State> state;
        {
            std::lock_guard lock(mutex_);
            state = std::move(state_);
        }
        stop_state(std::move(state));
    }

private:
    struct State {
        std::mutex mutex;
        std::condition_variable condition;
        std::chrono::milliseconds interval{80};
        AnimationTickSink tick;
        std::thread::id thread_id;
        bool stopping{false};
        bool active{true};
    };

    static void stop_state(std::shared_ptr<State> state) {
        if (!state) return;
        std::unique_lock lock(state->mutex);
        state->stopping = true;
        state->condition.notify_all();
        if (state->thread_id == std::this_thread::get_id()) return;
        state->condition.wait(lock, [&]() { return !state->active; });
    }

    std::mutex mutex_;
    std::shared_ptr<State> state_;
};

[[nodiscard]] std::chrono::milliseconds normalized_interval(std::chrono::milliseconds interval) {
    return interval.count() > 0 ? interval : kDefaultInterval;
}

} // namespace

struct Loader::Impl : public std::enable_shared_from_this<Loader::Impl> {
    mutable std::mutex state_mutex;
    std::mutex render_mutex;
    std::vector<std::string> frames{kDefaultFrames};
    std::chrono::milliseconds interval{kDefaultInterval};
    std::size_t current_frame{0};
    std::string message{"Loading..."};
    bool indicator_verbatim{false};
    bool running{false};
    RenderRequestSink request_render_sink;
    TextStyleHook spinner_style;
    TextStyleHook message_style;
    std::unique_ptr<AnimationTimer> timer;
    std::optional<util::Error> error;
    std::atomic_flag requesting = ATOMIC_FLAG_INIT;

    void request_render() {
        if (!request_render_sink || requesting.test_and_set()) return;
        try {
            request_render_sink();
        } catch (...) {
            std::lock_guard lock(state_mutex);
            error = util::make_error(
                util::ErrorCode::Unknown,
                "TUI Loader render request failed",
                "the render request callback threw an exception");
        }
        requesting.clear();
    }

    void tick() {
        {
            std::lock_guard lock(state_mutex);
            if (!running || frames.size() <= 1) return;
            current_frame = (current_frame + 1) % frames.size();
        }
        request_render();
    }

    void report_tick_failure() {
        std::lock_guard lock(state_mutex);
        error = util::make_error(
            util::ErrorCode::Unknown,
            "TUI Loader animation tick failed",
            "the animation callback threw an exception");
        running = false;
    }

    void restart() {
        timer->stop();
        std::size_t frame_count = 0;
        std::chrono::milliseconds selected_interval{80};
        {
            std::lock_guard state_lock(state_mutex);
            running = true;
            frame_count = frames.size();
            selected_interval = interval;
        }
        if (frame_count > 1) {
            std::weak_ptr<Impl> weak = shared_from_this();
            if (auto started = timer->start(selected_interval, [weak]() {
                    if (auto impl = weak.lock()) {
                        try {
                            impl->tick();
                        } catch (...) {
                            impl->report_tick_failure();
                        }
                    }
                });
                !started) {
                std::lock_guard state_lock(state_mutex);
                error = started.error();
                running = false;
            }
        }
        request_render();
    }
};

Loader::Loader(LoaderOptions options)
    : impl_(std::make_shared<Impl>()) {
    impl_->request_render_sink = std::move(options.request_render);
    impl_->spinner_style = std::move(options.spinner_style);
    impl_->message_style = std::move(options.message_style);
    impl_->message = std::move(options.message);
    impl_->timer = options.animation_timer
                       ? std::move(options.animation_timer)
                       : std::make_unique<SteadyAnimationTimer>();
    impl_->indicator_verbatim = options.indicator.has_value();
    if (options.indicator) {
        impl_->frames = options.indicator->frames.value_or(kDefaultFrames);
        impl_->interval = normalized_interval(options.indicator->interval);
    }
    impl_->restart();
}

Loader::Loader(Loader&&) noexcept = default;
Loader& Loader::operator=(Loader&&) noexcept = default;

Loader::~Loader() {
    stop();
}

void Loader::start() {
    impl_->restart();
}

void Loader::stop() {
    auto impl = impl_;
    if (!impl) return;
    {
        std::lock_guard state_lock(impl->state_mutex);
        impl->running = false;
    }
    impl->timer->stop();
}

bool Loader::running() const {
    std::lock_guard lock(impl_->state_mutex);
    return impl_->running;
}

void Loader::set_message(std::string message) {
    auto impl = impl_;
    {
        std::lock_guard lock(impl->state_mutex);
        impl->message = std::move(message);
    }
    impl->request_render();
}

void Loader::set_indicator(std::optional<LoaderIndicatorOptions> indicator) {
    auto impl = impl_;
    {
        std::lock_guard lock(impl->state_mutex);
        impl->indicator_verbatim = indicator.has_value();
        impl->frames = indicator && indicator->frames ? *indicator->frames : kDefaultFrames;
        impl->interval = normalized_interval(indicator ? indicator->interval : kDefaultInterval);
        impl->current_frame = 0;
    }
    impl->restart();
}

util::Expected<RenderResult> Loader::render(std::size_t width) {
    auto impl = impl_;
    if (width == 0) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI Loader requires a positive visible width"));
    }
    std::string frame;
    std::string message;
    bool verbatim = false;
    {
        std::lock_guard lock(impl->state_mutex);
        if (impl->error) return std::unexpected(*impl->error);
        frame = impl->frames.empty() ? std::string{} : impl->frames[impl->current_frame % impl->frames.size()];
        message = impl->message;
        verbatim = impl->indicator_verbatim;
    }

    std::lock_guard render_lock(impl->render_mutex);
    if (!verbatim && !frame.empty()) {
        auto styled = detail::apply_text_style(impl->spinner_style, std::move(frame), "Loader spinner");
        if (!styled) return std::unexpected(styled.error());
        frame = std::move(*styled);
    }
    auto styled_message = detail::apply_text_style(
        impl->message_style,
        std::move(message),
        "Loader message");
    if (!styled_message) return std::unexpected(styled_message.error());
    auto line = std::string(" ");
    if (!frame.empty()) line += frame + " ";
    line += *styled_message;
    auto bounded = detail::truncate_text(line, width, "");
    if (!bounded) return std::unexpected(bounded.error());
    return RenderResult{.lines = {"", std::move(*bounded)}};
}

void Loader::invalidate() {}

} // namespace cch::tui
