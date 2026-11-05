#pragma once

#include <cch/codemode/Portable.hpp>
#include <cch/codemode/Sandbox.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cch::codemode {

// The Remote protocol types (`CodemodeVmStartData`, `CodemodeRemoteRequest`,
// the message/reply variants, the flush constants) live in `Portable.hpp`,
// part of the portable no-Node subset (#1194); this header adds the
// behavior-coupled Remote surface.

/// pi `CodemodeRemoteExchange`: called repeatedly during execution.
using CodemodeRemoteExchangeHook = std::move_only_function<support::AsyncResult<CodemodeRemoteReplyVariant>(
        std::vector<CodemodeRemoteMessageVariant> messages)>;

/// pi `CodemodeRemote`: interface representing the remote worker/isolate/Durable Object.
class CodemodeRemote {
public:
    virtual ~CodemodeRemote() = default;
    [[nodiscard]] virtual support::AsyncResult<void> execute(
            CodemodeRemoteRequest request, CodemodeRemoteExchangeHook exchange) = 0;
};

/// Creates a remote worker for one sandbox run.
using CodemodeRemoteFactoryHook = std::move_only_function<support::Expected<std::shared_ptr<CodemodeRemote>>()>;

/// Options for `serve_codemode_remote`.
struct CodemodeServeOptions {
    std::filesystem::path wasm_path;
    std::optional<std::uint64_t> max_interrupt_budget;
    std::optional<std::uint64_t> max_memory_limit_bytes;
};

/// pi `serveCodemodeRemote`: remote side of `RemoteCodemodeSandbox`.
/// Runs one execution's VM and forwards messages through `exchange`.
/// Resolves once VM stopped and terminal message delivered, or caller asked to stop.
[[nodiscard]] support::AsyncResult<void> serve_codemode_remote(
        CodemodeRemoteRequest request, CodemodeRemoteExchangeHook exchange, CodemodeServeOptions options = {});

/// Options for `RemoteCodemodeSandbox`.
struct RemoteCodemodeSandboxOptions {
    CodemodeRemoteFactoryHook remote;
    std::vector<CodemodeGlobal> globals;
    std::optional<std::uint64_t> interrupt_budget;
};

/// pi `RemoteCodemodeSandbox`: runs VM on a remote while tools run in the caller.
class RemoteCodemodeSandbox {
    struct Impl;

public:
    struct ConstructionKey {
    private:
        ConstructionKey() = default;
        friend class RemoteCodemodeSandbox;
    };
    [[nodiscard]] static support::Expected<std::unique_ptr<RemoteCodemodeSandbox>> create(
            RemoteCodemodeSandboxOptions options);

    RemoteCodemodeSandbox(ConstructionKey, std::shared_ptr<Impl> impl);

    RemoteCodemodeSandbox(RemoteCodemodeSandbox&&) noexcept;
    RemoteCodemodeSandbox& operator=(RemoteCodemodeSandbox&&) noexcept;
    ~RemoteCodemodeSandbox();

    RemoteCodemodeSandbox(const RemoteCodemodeSandbox&) = delete;
    RemoteCodemodeSandbox& operator=(const RemoteCodemodeSandbox&) = delete;

    void close();

    /// One running deterministic execution over the remote (pi `CodemodeRun`).
    /// The caller answers its calls with `deliver`, exactly as the local
    /// `CodemodeSandbox::Run` does; results reach the VM across the exchange.
    class Run {
    public:
        Run(Run&&) noexcept;
        Run& operator=(Run&&) noexcept;
        ~Run();
        Run(const Run&) = delete;
        Run& operator=(const Run&) = delete;

        /// pi `CodemodeRun.finished`: the outcome is decided.
        [[nodiscard]] bool finished() const;

        /// pi `CodemodeRun.result`: the terminal outcome, delivered once.
        [[nodiscard]] support::AsyncResult<CodemodeRunResult> result();

        /// pi `CodemodeRun.deliver`: answer call `id` as result `seq` with the
        /// VM clock reading `now`. Refusals mirror the local run's, so a caller
        /// learns about a duplicate, unknown or malformed answer synchronously
        /// instead of from a stalled replay.
        [[nodiscard]] std::optional<CodemodeDeliveryRejection> deliver(
                std::int64_t seq, double now, std::int64_t id, CodemodeOutcome outcome);

        struct State;

    private:
        explicit Run(std::shared_ptr<State> state);
        std::shared_ptr<State> state_;
        friend class RemoteCodemodeSandbox;
    };

    /// pi `CodemodeSandboxBase.start` over a remote: the same deterministic
    /// inputs, fingerprint and seq-ordered delivery, with the VM running on the
    /// remote and the calls answered here.
    [[nodiscard]] support::Expected<Run> start(std::string script, CodemodeRunOptions options);

    [[nodiscard]] support::AsyncResult<CodemodeRunResult> run(std::string script,
            std::vector<CodemodeToolDescriptor> tools = {},
            CodemodeLimits limits = {},
            std::stop_token stop_token = {},
            CodemodeToolCallHandler handler = {},
            std::map<std::string, support::JsonValue> store = {},
            std::optional<std::string> abort_message = std::nullopt);

private:
    std::shared_ptr<Impl> impl_;
};

} // namespace cch::codemode
