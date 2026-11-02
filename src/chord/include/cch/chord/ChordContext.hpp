#pragma once

#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <utility>

namespace cch::chord {

namespace detail {

/// Identity of one typed {@link ContextKey}. The node stores the shared
/// identity, so a key and every context value layered with it agree on the
/// token even when the key object is copied.
struct ContextKeyIdentity {
    std::string description;
};

/// A found value and the exact C++ type it was stored as.
struct ContextValueView {
    const void* value{nullptr};
    std::type_index type{typeid(void)};
};

class ContextNode;
class CancellationState;
struct ContextNodeAccess;

} // namespace detail

/// A named, typed context key. Copying a key preserves its identity; two keys
/// created separately are distinct even when their descriptions match (pi
/// `createContextKey`).
template <typename T> class ContextKey {
public:
    explicit ContextKey(std::string description)
        : identity_(std::make_shared<const detail::ContextKeyIdentity>(
                  detail::ContextKeyIdentity{.description = std::move(description)})) {}

    [[nodiscard]] std::string_view description() const noexcept { return identity_->description; }

    [[nodiscard]] const std::shared_ptr<const detail::ContextKeyIdentity>& identity() const noexcept {
        return identity_;
    }

private:
    std::shared_ptr<const detail::ContextKeyIdentity> identity_;
};

template <typename T> [[nodiscard]] ContextKey<T> create_context_key(std::string description) {
    return ContextKey<T>{std::move(description)};
}

/// Immutable invocation-scoped values passed explicitly through operations
/// (pi `Context`). A context is a persistent layering of typed values over a
/// root; deriving a context never mutates its parent.
class Context {
public:
    Context(const Context&) = default;
    Context(Context&&) noexcept = default;
    Context& operator=(const Context&) = default;
    Context& operator=(Context&&) noexcept = default;
    ~Context() = default;

    /// The pi root label, e.g. `[Context BACKGROUND_CONTEXT]`, followed by one
    /// `.WithValue(<description>)` per derived layer.
    [[nodiscard]] std::string to_string() const;

    /// Cancellation of this context, or a default-constructed token when no
    /// cancellation layer is present. A detached cleanup context has none.
    [[nodiscard]] std::stop_token stop_token() const noexcept;

    [[nodiscard]] bool aborted() const noexcept;
    [[nodiscard]] std::optional<std::string> abort_reason() const;

    /// The value stored under `key` by this layer or the nearest parent layer.
    /// A missing key or a different value type yields `std::nullopt`; the
    /// keyed layers never reinterpret one type as another.
    template <typename T> [[nodiscard]] std::optional<T> value(const ContextKey<T>& key) const {
        static_assert(std::is_copy_constructible_v<T>, "context values must be copyable");
        const auto found = find_value(*key.identity());
        if (!found || found->type != std::type_index(typeid(T))) return std::nullopt;
        return *static_cast<const T*>(found->value);
    }

private:
    friend struct detail::ContextNodeAccess;

    explicit Context(std::shared_ptr<const detail::ContextNode> node) : node_(std::move(node)) {}

    [[nodiscard]] std::optional<detail::ContextValueView> find_value(const detail::ContextKeyIdentity& key) const;

    std::shared_ptr<const detail::ContextNode> node_;
};

/// The terminal error of a context-scoped wait that the caller cancelled
/// (pi `AbortError`): `ErrorCode::Cancelled` with the frozen message and the
/// caller's abort reason as the detail.
[[nodiscard]] support::Error context_abort_error(std::string reason);

namespace detail {

/// Type-erased value-layer constructor used by the typed `with_context_value`
/// template; implemented where `ContextNode` is complete.
[[nodiscard]] Context with_context_value_erased(std::shared_ptr<const ContextKeyIdentity> key,
        std::type_index type,
        std::shared_ptr<const void> value,
        const Context& parent);

template <typename T> struct ContextWaitState;

/// The caller-cancellation outcome of one `await_with_context` wait. It keeps
/// only a weak reference, so the wait state is destroyed by whichever side
/// settles last and never from inside this callback.
template <typename T> struct ContextWaitCancel {
    std::weak_ptr<ContextWaitState<T>> state;

    void operator()() const;
};

/// One context-scoped wait. Whichever of the caller cancellation and the
/// operation settles first delivers the one terminal outcome; the other is
/// dropped.
template <typename T> struct ContextWaitState {
    explicit ContextWaitState(Context value) : context(std::move(value)) {}

    std::mutex mutex;
    /// The waiter's terminal delivery, emptied by the winning outcome.
    std::optional<support::AsyncCompletion<T, support::Error>> completion;
    /// Kept alive so the abort reason stays readable after the caller drops its
    /// own handle.
    Context context;
    /// Registered on the context token for the lifetime of the wait.
    std::optional<std::stop_callback<ContextWaitCancel<T>>> on_cancel;
};

template <typename T> void ContextWaitCancel<T>::operator()() const {
    const std::shared_ptr<ContextWaitState<T>> locked = state.lock();
    if (!locked) return;
    std::optional<support::AsyncCompletion<T, support::Error>> rejected;
    std::string reason;
    {
        const std::lock_guard guard(locked->mutex);
        rejected = std::exchange(locked->completion, std::nullopt);
        if (rejected) reason = locked->context.abort_reason().value_or(std::string{});
    }
    if (rejected) (*rejected)(std::unexpected(context_abort_error(std::move(reason))));
}

} // namespace detail

/// Wait on one pending operation until it completes or `context` is cancelled
/// (pi `awaitWithContext`). Cancelling the caller rejects the wait with
/// `context_abort_error()` and never cancels the operation: the producer still
/// runs to completion and its terminal result is dropped. A context without a
/// cancellation layer returns the operation unchanged.
template <typename T>
[[nodiscard]] support::AsyncResult<T> await_with_context(support::AsyncResult<T> operation, Context context) {
    const std::stop_token token = context.stop_token();
    if (!token.stop_possible()) return operation;
    return support::AsyncResult<T>{support::AsyncProducer<T, support::Error>{
            [operation = std::move(operation), context = std::move(context), token](
                    support::AsyncCompletion<T, support::Error> completion) mutable noexcept {
                auto state = std::make_shared<detail::ContextWaitState<T>>(context);
                state->completion.emplace(std::move(completion));
                // Registering delivers the cancellation inline when the
                // context is already cancelled.
                state->on_cancel.emplace(token, detail::ContextWaitCancel<T>{state});
                // The operation always runs to completion; only its terminal
                // delivery is dropped once the wait has rejected.
                std::move(operation).start([state](std::expected<T, support::Error> outcome) mutable noexcept {
                    std::optional<support::AsyncCompletion<T, support::Error>> settled;
                    {
                        const std::lock_guard guard(state->mutex);
                        settled = std::exchange(state->completion, std::nullopt);
                    }
                    if (settled) (*settled)(std::move(outcome));
                });
            }}};
}

/// Derive a context with one additional or replaced value.
template <typename T>
[[nodiscard]] Context with_context_value(const ContextKey<T>& key, T value, const Context& parent) {
    static_assert(std::is_copy_constructible_v<T>, "context values must be copyable");
    return detail::with_context_value_erased(key.identity(),
            std::type_index(typeid(T)),
            std::shared_ptr<const void>(std::make_shared<const T>(std::move(value))),
            parent);
}

/// The distinct empty roots (pi `BACKGROUND_CONTEXT` and `TODO_CONTEXT`).
[[nodiscard]] const Context& background_context();
[[nodiscard]] const Context& todo_context();

/// Derive a context cancelled by either the parent token or the supplied
/// external token. The supplied reason is used when only the external token
/// aborts; the parent's own reason is preserved when it aborts first.
[[nodiscard]] Context with_abort_signal(std::stop_token signal, std::string reason, const Context& parent);

/// Derive a context retaining every value but suppressing caller cancellation
/// (pi `withoutAbortSignal`). Intended for mandatory cleanup only.
[[nodiscard]] Context without_abort_signal(const Context& context);

class CancellableContext;

/// Derive an independently cancellable child; cancel reaches this child and
/// its descendants, never the parent or a sibling. A parent cancellation
/// reaches a child created before the request.
[[nodiscard]] CancellableContext with_cancel(const Context& parent);

class CancellableContext {
public:
    CancellableContext(const CancellableContext&) = default;
    CancellableContext(CancellableContext&&) noexcept = default;
    CancellableContext& operator=(const CancellableContext&) = default;
    CancellableContext& operator=(CancellableContext&&) noexcept = default;
    ~CancellableContext() = default;

    [[nodiscard]] const Context& context() const noexcept { return context_; }

    /// Request cancellation with the first reason winning, matching
    /// `AbortController.abort(reason)`.
    void cancel(std::string reason) const;

private:
    friend CancellableContext with_cancel(const Context& parent);

    CancellableContext(Context context, std::shared_ptr<detail::CancellationState> state)
        : context_(std::move(context)), state_(std::move(state)) {}

    Context context_;
    std::shared_ptr<detail::CancellationState> state_;
};

} // namespace cch::chord
