#include <cch/chord/ChordContext.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

namespace cch::chord {

namespace detail {

/// One immutable layer of a Context. A value layer owns the key/value pair; an
/// abort or mask layer owns only a cancellation state (or suppresses it); a
/// root owns its pi label. Every layer points at the parent it was derived
/// from and never mutates it.
struct ContextNode {
    std::shared_ptr<const ContextNode> parent;
    std::shared_ptr<CancellationState> cancellation;
    std::shared_ptr<const ContextKeyIdentity> key;
    std::type_index value_type{typeid(void)};
    std::shared_ptr<const void> value;
    std::string root_name;
    std::string label;
};

struct StopCallbackHolderBase {
    virtual ~StopCallbackHolderBase() = default;
};

template <typename Callback> struct StopCallbackHolder final : StopCallbackHolderBase {
    std::stop_callback<Callback> callback;

    StopCallbackHolder(std::stop_token token, Callback callback_value)
        : callback(std::move(token), std::move(callback_value)) {}
};

/// A composed cancellation source. The first cancel wins and keeps its
/// reason; later requests are ignored, matching `AbortController.abort`.
class CancellationState {
public:
    [[nodiscard]] std::stop_token token() const noexcept { return source_.get_token(); }

    [[nodiscard]] bool aborted() const {
        const std::lock_guard lock(mutex_);
        return aborted_;
    }

    [[nodiscard]] std::optional<std::string> reason() const {
        const std::lock_guard lock(mutex_);
        return reason_;
    }

    bool cancel(std::string reason) {
        {
            const std::lock_guard lock(mutex_);
            if (aborted_) return false;
            aborted_ = true;
            reason_ = std::move(reason);
        }
        (void)source_.request_stop();
        return true;
    }

    void keep(std::unique_ptr<StopCallbackHolderBase> holder) {
        const std::lock_guard lock(mutex_);
        callbacks_.push_back(std::move(holder));
    }

private:
    mutable std::mutex mutex_;
    std::stop_source source_;
    bool aborted_{false};
    std::optional<std::string> reason_;
    std::vector<std::unique_ptr<StopCallbackHolderBase>> callbacks_;
};

struct ContextNodeAccess {
    [[nodiscard]] static Context make(std::shared_ptr<const ContextNode> node) { return Context(std::move(node)); }

    [[nodiscard]] static const std::shared_ptr<const ContextNode>& node(const Context& context) {
        return context.node_;
    }
};

namespace {

[[nodiscard]] std::string node_to_string(const ContextNode& node) {
    if (!node.root_name.empty()) return node.root_name;
    if (!node.parent) return "[Context]";
    return node_to_string(*node.parent) + ".WithValue(" + node.label + ")";
}

template <typename Callback>
void attach_cancellation(CancellationState& state, std::stop_token token, Callback callback) {
    state.keep(std::make_unique<StopCallbackHolder<Callback>>(std::move(token), std::move(callback)));
}

[[nodiscard]] std::shared_ptr<const ContextNode> abort_layer(
        std::shared_ptr<CancellationState> state, const std::shared_ptr<const ContextNode>& parent, std::string label) {
    auto node = std::make_shared<ContextNode>();
    node->parent = parent;
    node->cancellation = std::move(state);
    node->label = std::move(label);
    return node;
}

[[nodiscard]] std::shared_ptr<const ContextNode> root_node(std::string name) {
    auto node = std::make_shared<ContextNode>();
    node->root_name = std::move(name);
    return node;
}

/// Forward the parent's cancellation into a child state. A parent already
/// aborted at derivation cancels the child immediately; otherwise the child
/// observes the parent token without owning it.
void attach_parent_cancellation(
        const std::shared_ptr<CancellationState>& state, const std::shared_ptr<const ContextNode>& parent_node) {
    if (!parent_node || !parent_node->cancellation) return;
    if (parent_node->cancellation->aborted()) {
        if (const auto parent_reason = parent_node->cancellation->reason()) state->cancel(*parent_reason);
        return;
    }
    attach_cancellation(*state,
            parent_node->cancellation->token(),
            [weak = std::weak_ptr(state), parent = ContextNodeAccess::make(parent_node)] {
                if (const auto locked = weak.lock()) {
                    if (const auto parent_reason = parent.abort_reason()) locked->cancel(*parent_reason);
                }
            });
}

} // namespace

} // namespace detail

std::optional<detail::ContextValueView> Context::find_value(const detail::ContextKeyIdentity& key) const {
    for (const detail::ContextNode* node = node_.get(); node != nullptr; node = node->parent.get()) {
        if (node->key && node->key.get() == &key) {
            return detail::ContextValueView{.value = node->value.get(), .type = node->value_type};
        }
    }
    return std::nullopt;
}

std::string Context::to_string() const {
    if (!node_) return "[Context]";
    return detail::node_to_string(*node_);
}

std::stop_token Context::stop_token() const noexcept {
    if (!node_ || !node_->cancellation) return std::stop_token{};
    return node_->cancellation->token();
}

bool Context::aborted() const noexcept { return node_ && node_->cancellation && node_->cancellation->aborted(); }

std::optional<std::string> Context::abort_reason() const {
    if (!aborted()) return std::nullopt;
    return node_->cancellation->reason();
}

namespace detail {

Context with_context_value_erased(std::shared_ptr<const ContextKeyIdentity> key,
        std::type_index type,
        std::shared_ptr<const void> value,
        const Context& parent) {
    const auto& parent_node = ContextNodeAccess::node(parent);
    auto node = std::make_shared<ContextNode>();
    node->parent = parent_node;
    node->cancellation = parent_node ? parent_node->cancellation : nullptr;
    node->key = std::move(key);
    node->value_type = type;
    node->value = std::move(value);
    node->label = node->key->description;
    return ContextNodeAccess::make(std::move(node));
}

} // namespace detail

const Context& background_context() {
    static const Context context = detail::ContextNodeAccess::make(detail::root_node("[Context BACKGROUND_CONTEXT]"));
    return context;
}

const Context& todo_context() {
    static const Context context = detail::ContextNodeAccess::make(detail::root_node("[Context TODO_CONTEXT]"));
    return context;
}

Context with_abort_signal(std::stop_token signal, std::string reason, const Context& parent) {
    const auto& parent_node = detail::ContextNodeAccess::node(parent);
    auto state = std::make_shared<detail::CancellationState>();

    detail::attach_parent_cancellation(state, parent_node);

    if (!state->aborted() && signal.stop_possible()) {
        detail::attach_cancellation(*state, signal, [weak = std::weak_ptr(state), reason = std::move(reason)] {
            if (const auto state = weak.lock()) state->cancel(reason);
        });
    }

    return detail::ContextNodeAccess::make(detail::abort_layer(std::move(state), parent_node, "chord.abortSignal"));
}

Context without_abort_signal(const Context& context) {
    auto node = std::make_shared<detail::ContextNode>();
    node->parent = detail::ContextNodeAccess::node(context);
    node->label = "chord.abortSignal";
    return detail::ContextNodeAccess::make(std::move(node));
}

CancellableContext with_cancel(const Context& parent) {
    const auto& parent_node = detail::ContextNodeAccess::node(parent);
    auto state = std::make_shared<detail::CancellationState>();

    detail::attach_parent_cancellation(state, parent_node);

    auto context = detail::ContextNodeAccess::make(detail::abort_layer(state, parent_node, "chord.abortSignal"));
    return CancellableContext(std::move(context), std::move(state));
}

void CancellableContext::cancel(std::string reason) const {
    if (state_) (void)state_->cancel(std::move(reason));
}

} // namespace cch::chord
