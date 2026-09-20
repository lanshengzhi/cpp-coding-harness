#include "agent/AgentImpl.hpp"

#include "support/BoundedText.hpp"

#include <algorithm>
#include <memory>
#include <ranges>
#include <utility>
#include <vector>

namespace cch::agent {

void Agent::Impl::record_observer_diagnostic(const support::Error& failure) {
    constexpr std::size_t kMaxDiagnostics = 16;
    constexpr std::size_t kMaxDetailBytes = 1024;

    std::string detail = failure.message;
    if (!failure.detail.empty()) {
        detail += ": ";
        detail += failure.detail;
    }
    detail = support::bounded_redacted_text(std::move(detail), kMaxDetailBytes, "...");

    if (state.diagnostics.size() == kMaxDiagnostics) {
        state.diagnostics.erase(state.diagnostics.begin());
    }
    state.diagnostics.push_back(
            support::make_error(failure.code, "agent event observer failed", std::move(detail)));
    // Monotonic change marker for cheap observation (ADR 0052): the
    // bounded diagnostics vector rolls over, so its size cannot detect a
    // new entry.
    ++observer_diagnostic_serial_;
}

[[nodiscard]] support::ExpectedVoid Agent::Impl::notify(
        const AgentLifecycleEvent& event, const std::vector<std::shared_ptr<Subscriber>>& delivery_snapshot) {
    for (const auto& subscriber : delivery_snapshot) {
        if (!subscriber->delivery_enabled || !subscriber->sink) {
            continue;
        }
        if (auto observed = invoke_weak_observer(subscriber->sink, event); !observed) {
            record_observer_diagnostic(observed.error());
            subscriber->registered = false;
            subscriber->delivery_enabled = false;
        }
    }
    if (!active_run) {
        remove_unregistered_subscribers();
    }
    return {};
}

void Agent::Impl::remove_unregistered_subscribers() {
    std::erase_if(
            subscribers, [](const std::shared_ptr<Subscriber>& subscriber) { return !subscriber->registered; });
}

void Agent::Impl::unsubscribe(std::size_t id) {
    for (const auto& subscriber : subscribers) {
        if (subscriber->id == id) {
            // Deactivate immediately: a delivery snapshot shares this
            // entry, and clearing delivery_enabled suppresses the
            // subscriber's turn for the current event and every later
            // event in the run.
            subscriber->registered = false;
            subscriber->delivery_enabled = false;
            break;
        }
    }
    // Keep deactivated entries reachable while a delivery snapshot may
    // still own them so a reentrant unsubscribe cannot invalidate
    // iteration.
    if (!active_run) {
        remove_unregistered_subscribers();
    }
}

void Agent::Impl::clear_subscriptions() {
    for (const auto& subscriber : subscribers) {
        subscriber->registered = false;
        subscriber->delivery_enabled = false;
    }
    subscribers.clear();
}

[[nodiscard]] bool Agent::Impl::is_subscribed(std::size_t id) const {
    return std::ranges::any_of(subscribers, [id](const std::shared_ptr<Subscriber>& subscriber) {
        return subscriber->id == id && subscriber->registered;
    });
}

} // namespace cch::agent
