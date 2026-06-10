#pragma once

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L
#define CCH_HAS_STD_MOVE_ONLY_FUNCTION 1
#endif

namespace cch::util {

template <typename T>
using RemoveCvref = std::remove_cv_t<std::remove_reference_t<T>>;

template <typename Signature>
class MoveOnlyCallback;

template <typename R, typename... Args>
class MoveOnlyCallback<R(Args...)> {
public:
    MoveOnlyCallback() = default;
    MoveOnlyCallback(std::nullptr_t) {}

    template <typename Fn, typename = std::enable_if_t<!std::is_same_v<RemoveCvref<Fn>, MoveOnlyCallback>>>
    MoveOnlyCallback(Fn&& fn) {
#if CCH_HAS_STD_MOVE_ONLY_FUNCTION
        fn_ = std::move_only_function<R(Args...)>(std::forward<Fn>(fn));
#else
        fn_ = std::make_unique<Model<RemoveCvref<Fn>>>(std::forward<Fn>(fn));
#endif
    }

    MoveOnlyCallback(MoveOnlyCallback&&) noexcept = default;
    MoveOnlyCallback& operator=(MoveOnlyCallback&&) noexcept = default;

    MoveOnlyCallback(const MoveOnlyCallback&) = delete;
    MoveOnlyCallback& operator=(const MoveOnlyCallback&) = delete;

    MoveOnlyCallback& operator=(std::nullptr_t) noexcept {
#if CCH_HAS_STD_MOVE_ONLY_FUNCTION
        fn_ = nullptr;
#else
        fn_.reset();
#endif
        return *this;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
#if CCH_HAS_STD_MOVE_ONLY_FUNCTION
        return static_cast<bool>(fn_);
#else
        return static_cast<bool>(fn_);
#endif
    }

    R operator()(Args... args) {
#if CCH_HAS_STD_MOVE_ONLY_FUNCTION
        return fn_(std::forward<Args>(args)...);
#else
        if constexpr (std::is_void_v<R>) {
            fn_->invoke(std::forward<Args>(args)...);
        } else {
            return fn_->invoke(std::forward<Args>(args)...);
        }
#endif
    }

private:
#if CCH_HAS_STD_MOVE_ONLY_FUNCTION
    std::move_only_function<R(Args...)> fn_;
#else
    struct Concept {
        virtual ~Concept() = default;
        virtual R invoke(Args... args) = 0;
    };

    template <typename Fn>
    struct Model final : Concept {
        explicit Model(Fn&& fn) : fn_(std::move(fn)) {}
        explicit Model(const Fn& fn) : fn_(fn) {}

        R invoke(Args... args) override {
            if constexpr (std::is_void_v<R>) {
                fn_(std::forward<Args>(args)...);
            } else {
                return fn_(std::forward<Args>(args)...);
            }
        }

        Fn fn_;
    };

    std::unique_ptr<Concept> fn_;
#endif
};

} // namespace cch::util
