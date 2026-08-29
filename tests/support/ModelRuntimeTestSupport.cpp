#include "coding_agent/ModelRuntimeTestSupport.hpp"

#include "support/AsyncResultBridge.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <optional>
#include <utility>

namespace cch::coding_agent {

support::Expected<std::shared_ptr<ModelRuntime>> create_model_runtime_for_testing(
        ModelRuntimeOptions options, ModelRuntimeTestOptions test_options) {
    auto runtime = ModelRuntime::create_impl(std::move(options));
    if (!runtime) {
        return std::unexpected(runtime.error());
    }
    if (!test_options.providers.empty()) {
        (*runtime)->ai_models()->clear_providers();
    }
    for (auto& provider : test_options.providers) {
        if (auto applied = ai::providers::apply_scripted_provider(*(*runtime)->ai_models(), std::move(provider));
                !applied) {
            return std::unexpected(applied.error());
        }
    }
    if (auto transports = ai::providers::apply_scripted_transport_options(
                *(*runtime)->ai_models(), std::move(test_options.transports));
            !transports) {
        return std::unexpected(transports.error());
    }
    boost::asio::io_context io;
    std::optional<support::Expected<std::vector<ai::Model>>> snapshot;
    boost::asio::co_spawn(
            io,
            [&runtime, &snapshot]() -> boost::asio::awaitable<void> {
                snapshot.emplace(co_await support::detail::await_async_result(runtime.value()->get_available()));
                co_return;
            },
            boost::asio::detached);
    io.run();
    if (!snapshot || !*snapshot) {
        if (snapshot) {
            return std::unexpected(snapshot->error());
        }
        return std::unexpected(support::make_error(
                support::ErrorCode::Auth, "scripted runtime availability snapshot did not complete"));
    }
    return runtime;
}

} // namespace cch::coding_agent
