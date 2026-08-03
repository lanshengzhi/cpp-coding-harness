#pragma once

#include <cch/ai/Auth.hpp>
#include <cch/ai/Context.hpp>
#include <cch/ai/Message.hpp>
#include <cch/ai/Model.hpp>
#include <cch/ai/RequestOptions.hpp>
#include <cch/ai/StreamEvent.hpp>
#include <cch/util/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace cch::ai {

/// Authentication already resolved for one Provider call. Provider
/// implementations use Model::api for their private protocol dispatch.
struct ProviderStreamOptions {
    ModelAuth auth{};
    ProviderEnv env{};
    std::optional<double> temperature{std::nullopt};
    std::uint64_t max_tokens{1};
    std::optional<ModelThinkingLevel> reasoning{std::nullopt};
    std::optional<std::string> session_id{std::nullopt};
    CacheRetention cache_retention{CacheRetention::Short};
    std::optional<std::uint64_t> timeout_ms{std::nullopt};
    std::uint32_t max_retries{0};
    std::uint64_t max_retry_delay_ms{60000};
    std::stop_token stop_token{};
};

/// Long-lived runtime owner for one provider identity.
class Provider {
public:
    virtual ~Provider() = default;

    [[nodiscard]] virtual std::string_view id() const noexcept = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual ProviderAuth& auth() noexcept = 0;
    [[nodiscard]] virtual std::vector<Model> models() const = 0;

    /// Domain failures complete through one terminal event and a final
    /// AssistantMessage value. The Expected error alternative is reserved for
    /// consumer-sink or other infrastructure failure. The borrowed Model and
    /// AiContext must outlive the returned awaitable.
    [[nodiscard]] virtual boost::asio::awaitable<util::Expected<AssistantMessage>> stream(
        const Model& model,
        const AiContext& context,
        ProviderStreamOptions options,
        AssistantEventSink sink) = 0;
};

} // namespace cch::ai
