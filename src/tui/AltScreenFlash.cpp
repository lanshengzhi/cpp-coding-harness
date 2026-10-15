#include <cch/tui/AltScreenFlash.hpp>

#include <cch/tui/Utils.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace cch::tui {

AltScreenFlashContainer::AltScreenFlashContainer(std::function<void()> request_render)
    : request_render_(std::move(request_render)) {}

AltScreenFlashContainer::~AltScreenFlashContainer() { dispose(); }

void AltScreenFlashContainer::set_scheduler(TuiScheduler* scheduler) {
    if (scheduler_ == scheduler) return;
    for (auto& entry : entries_) {
        if (entry.timer) entry.timer->cancel();
        entry.timer.reset();
    }
    scheduler_ = scheduler;
    if (scheduler_ == nullptr) return;
    const auto now = scheduler_->now();
    for (auto& entry : entries_) {
        if (entry.deadline <= now) {
            entry.timer = scheduler_->schedule_delayed(std::chrono::steady_clock::duration::zero(),
                    [this, id = entry.id, generation = entry.generation] { expire(id, generation); });
        } else {
            schedule_expiry(entry);
        }
    }
}

void AltScreenFlashContainer::set_style(std::function<std::string(std::string)> style) { style_ = std::move(style); }

void AltScreenFlashContainer::flash(std::string message, std::chrono::milliseconds duration) {
    Entry entry;
    entry.id = next_id_++;
    entry.generation = ++generation_;
    entry.message = std::move(message);
    if (scheduler_ != nullptr) {
        const auto delay = std::max(duration, std::chrono::milliseconds::zero());
        entry.deadline = scheduler_->now() + delay;
        const auto id = entry.id;
        const auto generation = entry.generation;
        entry.timer = scheduler_->schedule_delayed(delay, [this, id, generation] { expire(id, generation); });
    }
    entries_.push_back(std::move(entry));
    if (request_render_) request_render_();
}

void AltScreenFlashContainer::dispose() noexcept {
    for (auto& entry : entries_) {
        if (entry.timer) entry.timer->cancel();
        entry.timer.reset();
    }
    entries_.clear();
    scheduler_ = nullptr;
    ++generation_;
}

support::Expected<RenderResult> AltScreenFlashContainer::render(std::size_t width) {
    RenderResult result;
    result.lines.reserve(entries_.size());
    for (const auto& entry : entries_) {
        auto message = truncate_text(" " + entry.message + " ", width, "", false);
        if (!message) return std::unexpected(message.error());
        auto line = "\x1b[7m" + std::move(*message) + "\x1b[27m";
        if (style_) line = style_(std::move(line));
        result.lines.push_back(std::move(line));
    }
    return result;
}

void AltScreenFlashContainer::schedule_expiry(Entry& entry) {
    if (scheduler_ == nullptr) return;
    const auto delay = std::max(entry.deadline - scheduler_->now(), std::chrono::steady_clock::duration::zero());
    const auto id = entry.id;
    const auto generation = entry.generation;
    entry.timer = scheduler_->schedule_delayed(delay, [this, id, generation] { expire(id, generation); });
}

void AltScreenFlashContainer::expire(std::size_t id, std::size_t generation) {
    const auto found = std::find_if(entries_.begin(), entries_.end(), [id, generation](const Entry& entry) {
        return entry.id == id && entry.generation == generation;
    });
    if (found == entries_.end()) return;
    if (found->timer) found->timer->cancel();
    entries_.erase(found);
    if (request_render_) request_render_();
}

} // namespace cch::tui
