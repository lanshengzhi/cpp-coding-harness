#include <cch/tui/CancellableLoader.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace cch::tui {

struct CancellableLoader::Impl {
    explicit Impl(CancellableLoaderOptions options)
        : loader(std::move(options.loader)),
          on_complete(std::move(options.on_complete)),
          on_cancel(std::move(options.on_cancel)) {}

    mutable std::mutex mutex;
    Loader loader;
    std::stop_source stop_source;
    CancellableLoaderState state{CancellableLoaderState::Active};
    LoaderCompletionSink on_complete;
    LoaderCancellationSink on_cancel;
    std::optional<util::Error> callback_error;
    bool focused{false};

    bool finish(CancellableLoaderState outcome) {
        LoaderCompletionSink completion;
        LoaderCancellationSink cancellation;
        bool request_stop = false;
        {
            std::lock_guard lock(mutex);
            if (state != CancellableLoaderState::Active) return false;
            state = outcome;
            if (outcome == CancellableLoaderState::Cancelled) {
                request_stop = true;
                cancellation = std::move(on_cancel);
            } else {
                completion = std::move(on_complete);
            }
        }
        if (request_stop) stop_source.request_stop();
        loader.stop();
        try {
            if (completion) completion();
            if (cancellation) cancellation();
        } catch (...) {
            std::lock_guard lock(mutex);
            callback_error = util::make_error(
                util::ErrorCode::Unknown,
                outcome == CancellableLoaderState::Cancelled
                    ? "TUI CancellableLoader cancellation callback failed"
                    : "TUI CancellableLoader completion callback failed",
                "the terminal callback threw an exception");
        }
        return true;
    }
};

CancellableLoader::CancellableLoader(CancellableLoaderOptions options)
    : impl_(std::make_shared<Impl>(std::move(options))) {}

CancellableLoader::CancellableLoader(CancellableLoader&&) noexcept = default;
CancellableLoader& CancellableLoader::operator=(CancellableLoader&&) noexcept = default;

CancellableLoader::~CancellableLoader() {
    stop();
}

std::stop_token CancellableLoader::cancellation_token() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->stop_source.get_token();
}

CancellableLoaderState CancellableLoader::state() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->state;
}

bool CancellableLoader::cancelled() const {
    return state() == CancellableLoaderState::Cancelled;
}

bool CancellableLoader::complete() {
    auto impl = impl_;
    return impl->finish(CancellableLoaderState::Completed);
}

bool CancellableLoader::cancel() {
    auto impl = impl_;
    return impl->finish(CancellableLoaderState::Cancelled);
}

void CancellableLoader::stop() {
    if (impl_) impl_->loader.stop();
}

void CancellableLoader::set_message(std::string message) {
    auto impl = impl_;
    {
        std::lock_guard lock(impl->mutex);
        if (impl->state != CancellableLoaderState::Active) return;
    }
    impl->loader.set_message(std::move(message));
}

void CancellableLoader::set_indicator(std::optional<LoaderIndicatorOptions> indicator) {
    auto impl = impl_;
    {
        std::lock_guard lock(impl->mutex);
        if (impl->state != CancellableLoaderState::Active) return;
    }
    impl->loader.set_indicator(std::move(indicator));
    bool still_active = false;
    {
        std::lock_guard lock(impl->mutex);
        still_active = impl->state == CancellableLoaderState::Active;
    }
    if (!still_active) impl->loader.stop();
}

util::Expected<std::vector<std::string>> CancellableLoader::render(std::size_t width) {
    auto impl = impl_;
    {
        std::lock_guard lock(impl->mutex);
        if (impl->callback_error) return std::unexpected(*impl->callback_error);
    }
    return impl->loader.render(width);
}

void CancellableLoader::invalidate() {
    impl_->loader.invalidate();
}

void CancellableLoader::handle_input(const InputEventVariant& input) {
    const auto* key = std::get_if<KeyEvent>(&input);
    if (key == nullptr || key->type == KeyEventType::Release) return;
    if (matches_key(*key, "escape") || matches_key(*key, "ctrl+c")) {
        (void)cancel();
    }
}

bool CancellableLoader::accepts_key_releases() const {
    return false;
}

void CancellableLoader::set_focused(bool focused) {
    std::lock_guard lock(impl_->mutex);
    impl_->focused = focused;
}

bool CancellableLoader::focused() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->focused;
}

} // namespace cch::tui
