#pragma once

// The codemode portable entry's protocol contract (pi
// `packages/codemode/src/portable.ts`): the source options prefix/grammar, the
// prelude and declaration constants, and the Inline/Remote protocol types —
// everything a no-Node host needs without the worker-mode engine. This header
// is part of the portable subset: it carries no Boost.Asio, no WasmEdge and no
// thread dependency, and `cch_codemode_portable` is the build-level guarantee
// that stays true (ADR 0075, #1194). The behavior-coupled declarations (the
// sandbox classes, the serve operation, the exchange hook) live in the full
// `cch_codemode` target.

#include <cch/codemode/Declarations.hpp>
#include <cch/codemode/Source.hpp>
#include <cch/codemode/Types.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cch::codemode {

/// pi's Remote output-batching constants (`FLUSH_ITEMS`/`FLUSH_CHARS`). The
/// prelude/declaration constants (`MAX_OUTPUT_*`, `MAX_STORE_*`, the options
/// prefix and grammar) come through `Declarations.hpp` and `Source.hpp`
/// above, exactly as the frozen `portable.ts` re-exports them.
inline constexpr std::size_t kRemoteFlushItems = 1000;
inline constexpr std::size_t kRemoteFlushChars = 1 << 20; // 1 MiB

/// pi `VmStartData` (`packages/codemode/src/runtime/protocol.ts`):
/// everything a VM needs to run one script, independent of where it runs.
struct CodemodeVmStartData {
    std::string code;
    std::vector<CodemodeToolDescriptor> tools;
    std::vector<CodemodeGlobal> globals;
    std::optional<std::uint64_t> memory_limit_bytes;
    /// Snapshot for load(): key to JSON text.
    std::map<std::string, std::string> store;
    /// Deterministic start inputs (pi `start`). Unset runs the ordinary VM.
    std::optional<std::int64_t> started_at;
    std::optional<std::uint64_t> seed;
    /// The recorded run's fingerprint, when replaying.
    std::optional<std::string> expected_fingerprint;
};

/// pi `CodemodeRemoteRequest`: request sent to remote to execute one script.
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

/// Terminal outcome of guest script execution. `writes` is the JSON-encoded
/// store write set; empty on success without writes.
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

/// Messages sent from remote worker to host across the exchange
/// (pi `WorkerToHostMessage` plus the launcher's failure).
using CodemodeRemoteMessageVariant = std::variant<CodemodeRemoteCallMessage,
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

/// Reply sent from caller to remote across the exchange (pi
/// `HostToWorkerMessage` | stop | null).
using CodemodeRemoteReplyVariant =
        std::variant<CodemodeRemoteNullReply, CodemodeRemoteResultReply, CodemodeRemoteStopReply>;

} // namespace cch::codemode
