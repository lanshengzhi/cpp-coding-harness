#include "coding_agent/extensions/NativeExtensionHost.hpp"

#include "support/AsyncResultBridge.hpp"

#include <boost/asio/awaitable.hpp>

#include <utility>

namespace cch::coding_agent::extensions {
namespace {

using ExtensionList = std::vector<std::shared_ptr<NativeExtension>>;

[[nodiscard]] std::shared_ptr<const ExtensionList> share_list(ExtensionList extensions) {
    return std::make_shared<const ExtensionList>(std::move(extensions));
}

} // namespace

NativeExtensionHost::NativeExtensionHost() : extensions_(share_list({})) {}

NativeExtensionHost::NativeExtensionHost(ExtensionList extensions) : extensions_(share_list(std::move(extensions))) {}

bool NativeExtensionHost::empty() const noexcept { return extensions_->empty(); }

support::ExpectedVoid NativeExtensionHost::bind(ExtensionSession& session) const {
    for (auto& extension : *extensions_) {
        if (auto bound = extension->bind(session); !bound) {
            return bound;
        }
    }
    return {};
}

support::AsyncResult<BeforeAgentStartOutcome> NativeExtensionHost::before_agent_start(
        BeforeAgentStartEvent event, ExtensionContext context) const {
    return support::detail::make_async_result(
            [extensions = extensions_,
                    event = std::move(event),
                    context]() mutable -> boost::asio::awaitable<support::Expected<BeforeAgentStartOutcome>> {
                BeforeAgentStartOutcome outcome;
                // Handlers run in registration order (pi `snapshotEventHandlers`
                // order); each observes the prompt the previous one returned.
                for (auto& extension : *extensions) {
                    auto result =
                            co_await support::detail::await_async_result(extension->before_agent_start(event, context));
                    if (!result) {
                        co_return std::unexpected(std::move(result.error()));
                    }
                    if (result->system_prompt) {
                        event.system_prompt = *result->system_prompt;
                        outcome.system_prompt = std::move(*result->system_prompt);
                    }
                    if (result->message) {
                        outcome.messages.push_back(std::move(*result->message));
                    }
                }
                co_return outcome;
            });
}

} // namespace cch::coding_agent::extensions
