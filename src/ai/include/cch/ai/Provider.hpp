#pragma once

#include <cch/ai/Auth.hpp>
#include <cch/ai/Context.hpp>
#include <cch/ai/Message.hpp>
#include <cch/ai/Model.hpp>
#include <cch/ai/ModelStream.hpp>
#include <cch/ai/RequestOptions.hpp>
#include <cch/ai/StreamEvent.hpp>
#include <cch/support/Error.hpp>

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
    /// Case-insensitive header tombstones retained after Models applies the
    /// final transform, so an adapter does not recreate explicitly deleted
    /// protocol defaults.
    std::vector<std::string> deleted_headers{};
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
///
/// Concurrency contract: a Provider is not internally synchronized and its
/// adapters may hold per-connection or per-session state. All operations on
/// one Provider must be driven by a single-threaded executor or otherwise
/// serialized; do not drive the same Provider from two threads.
class Provider {
public:
    virtual ~Provider() = default;

    [[nodiscard]] virtual std::string_view id() const noexcept = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual ProviderAuth& auth() noexcept = 0;
    [[nodiscard]] virtual std::vector<Model> models() const = 0;

    /// One move-only, single-consumption model stream (ADR 0040 / #455).
    ///
    /// Domain failures complete through one terminal event and a final
    /// AssistantMessage value; the Expected error alternative is reserved for
    /// consumer-sink or other infrastructure failure. The returned stream owns
    /// the Model and AiContext and delivers events in order through the
    /// consumption-time sink before exactly one terminal outcome; retry
    /// boundaries and cooperative cancellation stay inside the provider.
    /// `stream` must not run concurrently on the same Provider; see the class
    /// concurrency contract.
    [[nodiscard]] virtual ModelStream stream(
        Model model,
        AiContext context,
        ProviderStreamOptions options) = 0;
};

} // namespace cch::ai
