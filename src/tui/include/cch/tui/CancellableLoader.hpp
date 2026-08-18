#pragma once

#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Loader.hpp>

#include <cch/support/Error.hpp>

#include <functional>
#include <memory>
#include <stop_token>

namespace cch::tui {

using LoaderCompletionSink = std::move_only_function<support::ExpectedVoid()>;
using LoaderCancellationSink = std::move_only_function<support::ExpectedVoid()>;

enum class CancellableLoaderState {
    Active,
    Completed,
    Cancelled,
};

struct CancellableLoaderOptions {
    LoaderOptions loader{};
    LoaderCompletionSink on_complete{};
    LoaderCancellationSink on_cancel{};
    std::shared_ptr<const KeybindingRegistry> keybindings{};
};

/// A Loader with one idempotent cancellation signal and one terminal outcome.
class CancellableLoader final : public Component, public InputHandler, public Focusable {
public:
    explicit CancellableLoader(CancellableLoaderOptions options = {});
    CancellableLoader(CancellableLoader&&) noexcept;
    CancellableLoader& operator=(CancellableLoader&&) noexcept;
    ~CancellableLoader() override;

    CancellableLoader(const CancellableLoader&) = delete;
    CancellableLoader& operator=(const CancellableLoader&) = delete;

    [[nodiscard]] std::stop_token cancellation_token() const;
    [[nodiscard]] CancellableLoaderState state() const;
    [[nodiscard]] bool cancelled() const;
    [[nodiscard]] bool complete();
    [[nodiscard]] bool cancel();
    void stop();
    void set_message(std::string message);
    void set_indicator(std::optional<LoaderIndicatorOptions> indicator = std::nullopt);

    [[nodiscard]] support::Expected<RenderResult> render(std::size_t width) override;
    void invalidate() override;
    void handle_input(const InputEventVariant& input) override;
    [[nodiscard]] bool accepts_key_releases() const override;
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace cch::tui
