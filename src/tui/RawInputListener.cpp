#include <cch/tui/RawInputListener.hpp>

#include <algorithm>

namespace cch::tui {

struct RawInputListenerChain::State {
    struct Entry {
        RawInputListenerId id{0};
        RawInputListener listener{};
        bool active{false};
    };

    std::vector<Entry> entries;
    std::size_t retired{0};
    RawInputListenerId next_id{1};

    [[nodiscard]] bool is_active(RawInputListenerId id) const noexcept {
        for (const auto& entry : entries) {
            if (entry.active && entry.id == id) return true;
        }
        return false;
    }

    bool deactivate(RawInputListenerId id) noexcept {
        for (auto& entry : entries) {
            if (!entry.active || entry.id != id) continue;
            entry.active = false;
            entry.listener = {};
            ++retired;
            return true;
        }
        return false;
    }

    void compact() {
        if (retired == 0) return;
        entries.erase(std::remove_if(entries.begin(), entries.end(), [](const Entry& entry) { return !entry.active; }),
                entries.end());
        retired = 0;
    }
};

RawInputListenerChain::RawInputListenerChain() : state_(std::make_shared<State>()) {}

RawInputListenerChain::~RawInputListenerChain() = default;

RawInputListenerId RawInputListenerChain::add(RawInputListener listener) {
    const auto id = state_->next_id++;
    state_->entries.push_back(State::Entry{.id = id, .listener = std::move(listener), .active = true});
    return id;
}

RawInputListenerHandle RawInputListenerChain::add_owned(RawInputListener listener) {
    const auto id = add(std::move(listener));
    return RawInputListenerHandle{state_, id};
}

bool RawInputListenerChain::remove(RawInputListenerId id) {
    if (!state_->deactivate(id)) return false;
    state_->compact();
    return true;
}

std::size_t RawInputListenerChain::size() const noexcept {
    return static_cast<std::size_t>(std::count_if(
            state_->entries.begin(), state_->entries.end(), [](const State::Entry& entry) { return entry.active; }));
}

void RawInputListenerChain::clear() {
    for (auto& entry : state_->entries) {
        if (entry.active) entry.listener = {};
        entry.active = false;
    }
    state_->retired = 0;
}

RawInputDispatch RawInputListenerChain::dispatch(std::string_view input) {
    RawInputDispatch outcome{.consumed = false, .input = std::string(input)};
    // The chain is snapshotted before the first listener runs: a listener
    // added during this dispatch belongs to the next input, and a listener
    // removed during it (itself or another) is skipped from its removal on.
    std::vector<std::pair<RawInputListenerId, RawInputListener>> snapshot;
    snapshot.reserve(state_->entries.size());
    for (const auto& entry : state_->entries) {
        if (!entry.active) continue;
        snapshot.emplace_back(entry.id, entry.listener);
    }

    for (const auto& [id, listener] : snapshot) {
        if (!state_->is_active(id)) continue;
        const auto result = listener(outcome.input);
        if (result.consumed) {
            outcome.consumed = true;
            break;
        }
        if (result.rewrite.has_value()) outcome.input = std::move(*result.rewrite);
    }

    state_->compact();
    return outcome;
}

void RawInputListenerChain::dispose(const std::shared_ptr<State>& state, RawInputListenerId id) noexcept {
    if (state == nullptr || id == 0) return;
    if (!state->deactivate(id)) return;
    state->compact();
}

RawInputListenerHandle::RawInputListenerHandle(
        std::weak_ptr<RawInputListenerChain::State> state, RawInputListenerId id) noexcept
    : state_(std::move(state)), id_(id) {}

RawInputListenerHandle::RawInputListenerHandle(RawInputListenerHandle&& other) noexcept
    : state_(std::move(other.state_)), id_(other.id_) {
    other.state_.reset();
    other.id_ = 0;
}

RawInputListenerHandle& RawInputListenerHandle::operator=(RawInputListenerHandle&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        id_ = other.id_;
        other.state_.reset();
        other.id_ = 0;
    }
    return *this;
}

RawInputListenerHandle::~RawInputListenerHandle() { reset(); }

bool RawInputListenerHandle::active() const noexcept {
    const auto state = state_.lock();
    return state != nullptr && id_ != 0 && state->is_active(id_);
}

void RawInputListenerHandle::reset() {
    RawInputListenerChain::dispose(state_.lock(), id_);
    state_.reset();
    id_ = 0;
}

} // namespace cch::tui
