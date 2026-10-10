#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui {

/// Stable per-registration identifier of one raw-input listener. Ids are never
/// reused inside one chain, so removing a listener that was already removed
/// cannot detach a newer registration.
using RawInputListenerId = std::uint64_t;

/// One listener's decision for one raw input fragment (behavioral baseline: pi
/// `tui.ts` `TuiInputListenerResult`). Exactly one of three outcomes:
///
/// - `pass()` leaves the current text unchanged for the rest of the chain.
/// - `consume()` stops the chain immediately: no later listener and no typed
///   dispatch observe this input.
/// - `replace(text)` continues the chain with `text`, which the next listener
///   and typed dispatch observe instead of the previous text. The empty string
///   is a rewrite, not a consumption: the remaining listeners still run and
///   typed dispatch is skipped because there is nothing left to decode.
struct RawInputListenerResult {
    bool consumed{false};
    std::optional<std::string> rewrite{};

    [[nodiscard]] static RawInputListenerResult pass() noexcept { return {}; }

    [[nodiscard]] static RawInputListenerResult consume() noexcept { return RawInputListenerResult{.consumed = true}; }

    [[nodiscard]] static RawInputListenerResult replace(std::string text) {
        return RawInputListenerResult{.rewrite = std::move(text)};
    }
};

/// Ordered raw-input listener invoked with the text produced so far by the
/// chain. The argument is invalidated when the call returns; a listener that
/// needs the bytes afterwards copies them.
using RawInputListener = std::function<RawInputListenerResult(std::string_view)>;

/// Outcome of running the whole chain for one input fragment: `consumed` stops
/// typed dispatch, and `input` is the text that survived the chain.
struct RawInputDispatch {
    bool consumed{false};
    std::string input{};
};

class RawInputListenerHandle;

/// Ordered per-instance chain of raw-input listeners. A host owns one chain and
/// dispatches every raw input fragment through it before typed component input
/// (pi `tui.ts` `inputListeners`), so extension listeners observe, consume and
/// rewrite bytes in registration order.
///
/// The chain is a value owned by its host: no process-global registry exists.
/// Listeners may add and remove registrations while a dispatch is running; a
/// listener registered during a dispatch runs from the next input, and one
/// removed during a dispatch (itself or another) is not called again.
class RawInputListenerChain final {
public:
    RawInputListenerChain();
    ~RawInputListenerChain();
    RawInputListenerChain(const RawInputListenerChain&) = delete;
    RawInputListenerChain& operator=(const RawInputListenerChain&) = delete;
    RawInputListenerChain(RawInputListenerChain&&) = delete;
    RawInputListenerChain& operator=(RawInputListenerChain&&) = delete;

    /// Register `listener` after every registration made so far and return its
    /// id.
    [[nodiscard]] RawInputListenerId add(RawInputListener listener);

    /// Register `listener` and return the RAII handle owning that
    /// registration, for hosts that dispose listeners by lifetime.
    [[nodiscard]] RawInputListenerHandle add_owned(RawInputListener listener);

    /// Remove the registration. Returns false when the id is not registered.
    [[nodiscard]] bool remove(RawInputListenerId id);

    /// Number of registrations that still run.
    [[nodiscard]] std::size_t size() const noexcept;

    /// Remove every registration.
    void clear();

    /// Run the chain over `input` in registration order. Consuming stops the
    /// chain; otherwise the last rewrite wins and typed dispatch receives
    /// `outcome.input` (empty when the surviving text is empty).
    [[nodiscard]] RawInputDispatch dispatch(std::string_view input);

private:
    friend class RawInputListenerHandle;

    struct State;

    /// Remove `id` from this chain; a handle holds only a weak reference, so a
    /// destroyed chain is never touched.
    static void dispose(const std::shared_ptr<State>& state, RawInputListenerId id) noexcept;

    std::shared_ptr<State> state_;
};

/// RAII owner of one raw-input listener registration. Destroying or resetting
/// the handle removes the listener, and a handle outliving its chain (the host
/// was destroyed) disposes nothing, so no stale callback can remain.
class RawInputListenerHandle final {
public:
    RawInputListenerHandle() noexcept = default;
    RawInputListenerHandle(RawInputListenerHandle&& other) noexcept;
    RawInputListenerHandle& operator=(RawInputListenerHandle&& other) noexcept;
    RawInputListenerHandle(const RawInputListenerHandle&) = delete;
    RawInputListenerHandle& operator=(const RawInputListenerHandle&) = delete;
    ~RawInputListenerHandle();

    /// Id of the registration this handle owns; zero for a handle that owns
    /// nothing.
    [[nodiscard]] RawInputListenerId id() const noexcept { return id_; }

    /// True while this handle still owns a registration of a live chain.
    [[nodiscard]] bool active() const noexcept;

    /// Remove the registration now, leaving the handle inactive.
    void reset();

private:
    friend class RawInputListenerChain;

    RawInputListenerHandle(std::weak_ptr<RawInputListenerChain::State> state, RawInputListenerId id) noexcept;

    std::weak_ptr<RawInputListenerChain::State> state_;
    RawInputListenerId id_{0};
};

} // namespace cch::tui
