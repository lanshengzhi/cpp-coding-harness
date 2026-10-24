#pragma once

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

/// pi `VmStartData` (`packages/codemode/src/runtime/protocol.ts`):
/// everything a VM needs to run one script, independent of where it runs.
struct CodemodeVmStartData {
    std::string code;
    std::vector<CodemodeToolDescriptor> tools;
    std::vector<CodemodeGlobal> globals;
    std::optional<std::uint64_t> memory_limit_bytes;
    /// Snapshot for load(): key to JSON text.
    std::map<std::string, std::string> store;
};

/// pi `CodemodeRemoteRequest` (`packages/codemode/src/runtime/remote.ts`):
/// request sent to remote to execute one script.
struct CodemodeRemoteRequest {
    CodemodeVmStartData data;
    /// Requested interrupt budget. The remote may lower it.
    std::optional<std::uint64_t> interrupt_budget;
};

/// Tool or global invocation issued by guest script to caller.
struct CodemodeRemoteCallMessage {
    std::int64_t id{0};
    std::string target{"tool"}; // "tool" or "global"
    std::string name;
    std::optional<std::string> args;
};

/// Output emitted by guest script.
struct CodemodeRemoteOutputMessage {
    CodemodeOutputItem item;
};

/// Terminal outcome of guest script execution.
struct CodemodeRemoteDoneMessage {
    bool ok{true};
    std::optional<std::string> value;
    std::string writes{"[]"};
    std::optional<std::string> error;
    std::optional<CodemodeDuring> during;
};

/// VM crashed outside guest script control.
struct CodemodeRemoteCrashMessage {
    std::string message;
};

/// Deterministic VM started with fingerprint.
struct CodemodeRemoteStartedMessage {
    std::string fingerprint;
};

/// Deterministic VM became idle awaiting results.
struct CodemodeRemoteIdleMessage {
    std::int64_t seq{0};
    std::int64_t issued{0};
};

/// Failure reported outside the VM bridge.
struct CodemodeRemoteFailureMessage {
    CodemodeError error;
};

/// Messages sent from remote worker to host across exchange (`WorkerToHostMessage` + failure).
using CodemodeRemoteMessage = std::variant<CodemodeRemoteCallMessage,
        CodemodeRemoteOutputMessage,
        CodemodeRemoteDoneMessage,
        CodemodeRemoteCrashMessage,
        CodemodeRemoteStartedMessage,
        CodemodeRemoteIdleMessage,
        CodemodeRemoteFailureMessage>;

/// Result of a tool call delivered from caller to remote.
struct CodemodeRemoteResultReply {
    std::int64_t id{0};
    bool ok{false};
    std::optional<std::string> payload;
    std::optional<std::int64_t> seq;
    std::optional<double> now;
    std::optional<std::string> properties;
};

/// Stop command sent from caller to remote.
struct CodemodeRemoteStopReply {};

/// Null reply when exchange carried no call.
struct CodemodeRemoteNullReply {};

/// Reply sent from caller to remote across exchange (`HostToWorkerMessage` | stop | null).
using CodemodeRemoteReply = std::variant<CodemodeRemoteNullReply, CodemodeRemoteResultReply, CodemodeRemoteStopReply>;

/// pi `CodemodeRemoteExchange`: called repeatedly during execution.
using CodemodeRemoteExchange =
        std::function<support::AsyncResult<CodemodeRemoteReply>(std::vector<CodemodeRemoteMessage> messages)>;

/// pi `CodemodeRemote`: interface representing the remote worker/isolate/Durable Object.
class CodemodeRemote {
public:
    virtual ~CodemodeRemote() = default;
    [[nodiscard]] virtual support::AsyncResult<void> execute(
            CodemodeRemoteRequest request, CodemodeRemoteExchange exchange) = 0;
};

/// Remote output batching constants (upstream pi `FLUSH_ITEMS` and `FLUSH_CHARS`).
inline constexpr std::size_t kRemoteFlushItems = 1000;
inline constexpr std::size_t kRemoteFlushChars = 1 << 20; // 1 MiB

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
        CodemodeRemoteRequest request, CodemodeRemoteExchange exchange, CodemodeServeOptions options = {});

/// Options for `RemoteCodemodeSandbox`.
struct RemoteCodemodeSandboxOptions {
    std::function<support::Expected<std::shared_ptr<CodemodeRemote>>()> remote;
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

    RemoteCodemodeSandbox(ConstructionKey, std::unique_ptr<Impl> impl);

    RemoteCodemodeSandbox(RemoteCodemodeSandbox&&) noexcept;
    RemoteCodemodeSandbox& operator=(RemoteCodemodeSandbox&&) noexcept;
    ~RemoteCodemodeSandbox();

    RemoteCodemodeSandbox(const RemoteCodemodeSandbox&) = delete;
    RemoteCodemodeSandbox& operator=(const RemoteCodemodeSandbox&) = delete;

    void close();

    [[nodiscard]] support::AsyncResult<CodemodeRunResult> run(std::string script,
            std::vector<CodemodeToolDescriptor> tools = {},
            CodemodeLimits limits = {},
            std::stop_token stop_token = {},
            CodemodeToolCallHandler handler = {},
            std::map<std::string, support::JsonValue> store = {},
            std::optional<std::string> abort_message = std::nullopt);

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::codemode
