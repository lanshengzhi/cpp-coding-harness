#pragma once

// Private to the `cch_codemode` owner: the deterministic VM driver behind
// `CodemodeSandbox::start`. pi v1.3.0 splits the same two roles — `runtime/vm.ts`
// drives one VM through one script, `runtime/execution.ts` owns the host side
// (tool table, call records, output, deadline, abort, decoding) — and the
// deterministic mode lives in the VM half: the virtual clock, the seeded PRNG,
// the seq-ordered delivery queue, the fingerprint, and the `idle`/`during`
// metadata. Keeping it in its own translation unit leaves the worker-mode
// engine in `Sandbox.cpp` unchanged.

#include <cch/codemode/Sandbox.hpp>
#include <cch/support/AsyncResult.hpp>

#include <wasmedge/wasmedge.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace cch::codemode::detail {

struct Guest;
using Handle = std::int32_t;

/// pi `DETERMINISTIC_VERSION` (`runtime/vm.ts`): bumped whenever the driver or
/// the prelude changes what a deterministic script observes beyond what the
/// fingerprint's other parts already capture.
inline constexpr int kDeterministicVersion = 1;

/// pi `fnv1a` (`runtime/vm.ts`): FNV-1a over the prelude's UTF-16 code units,
/// as eight lowercase hex digits. The prelude is ASCII, so one byte is one code
/// unit and the C++ and pi hashes agree byte for byte.
[[nodiscard]] std::string prelude_fingerprint_hash(std::string_view prelude);

/// pi `startVm`'s fingerprint string:
/// `quickjs-wasi <version>, quickjs <version>, prelude <hash>, deterministic
/// <n>, stack <n>, heap <n|none>, budget <none|n>`.
[[nodiscard]] std::string deterministic_fingerprint(std::string_view quickjs_wasi_version,
        std::string_view quickjs_version,
        std::string_view prelude_hash,
        std::uint32_t max_stack_size,
        std::optional<std::uint64_t> memory_limit_bytes,
        std::optional<std::uint64_t> interrupt_budget);

/// pi `VirtualClock` (`runtime/vm.ts`): `started_at` until the first result,
/// then the `now` of the result being processed, never going backwards.
struct VirtualClock {
    bool creating{true};
    double started_at{0.0};
    double now{0.0};
    std::uint32_t seed{0};
};

/// pi `prng` (`runtime/vm.ts`): mulberry32. One 32-bit draw per call; the
/// deterministic `random_get` fills its buffer from these.
class Mulberry32 {
public:
    explicit Mulberry32(std::uint32_t seed) : state_(seed) {}
    [[nodiscard]] std::uint32_t draw();

private:
    std::uint32_t state_;
};

/// Everything one deterministic VM run needs beyond the script: the run's own
/// tool and global surface, the store snapshot, and the deterministic inputs.
struct DeterministicStart {
    std::string script;
    std::vector<CodemodeToolDescriptor> tools;
    std::vector<CodemodeGlobal> globals;
    std::string store_json;
    std::int64_t started_at_ms{0};
    std::optional<std::string> expected_fingerprint;
    /// pi's abort reason text, reported when the caller's stop token cancels
    /// the run. Without one pi reports "Execution aborted".
    std::optional<std::string> abort_message;
    std::uint32_t seed{0};
    std::optional<std::uint64_t> memory_limit_bytes;
    std::optional<std::chrono::milliseconds> timeout;
    std::stop_token stop_token;
};

/// The caller's progress callbacks, invoked on the run's own worker thread in
/// the order the VM reports them. A callback must not block: `deliver` from
/// inside `on_call` only enqueues the answer, so it never re-enters the VM.
struct DeterministicHost {
    std::function<void(std::string_view fingerprint)> on_started;
    std::function<void(const CodemodeHostCall& call)> on_call;
    std::function<void(std::int64_t seq, std::int64_t issued)> on_idle;
};

/// One deterministic VM: pi `startVm` plus the host-side delivery queue of
/// `Execution`. Construction starts the worker thread; the destructor stops and
/// joins it, so a run the caller abandons never outlives its handle.
///
/// `ast` is the sandbox's parsed guest module and must outlive this object.
class DeterministicRun {
public:
    DeterministicRun(WasmEdge_ASTModuleContext* ast, DeterministicStart start, DeterministicHost host);
    ~DeterministicRun();

    DeterministicRun(DeterministicRun&&) = delete;
    DeterministicRun& operator=(DeterministicRun&&) = delete;
    DeterministicRun(const DeterministicRun&) = delete;
    DeterministicRun& operator=(const DeterministicRun&) = delete;

    /// pi `Execution.deliver`: answer call `id` as result number `seq`, with the
    /// VM clock reading `now` while it processes this result. Returns the
    /// refusal when the answer is not admissible; an admitted answer is queued
    /// and applied by the worker in `seq` order.
    [[nodiscard]] std::optional<CodemodeDeliveryRejection> deliver(
            std::int64_t seq, double now, std::int64_t id, CodemodeOutcome outcome);

    /// pi `Execution.finished`: whether the run has its outcome. Set when the
    /// outcome is decided, before the terminal result is handed out.
    [[nodiscard]] bool finished() const;

    /// The run's terminal outcome, delivered exactly once. Called at most once
    /// per run; a run that is already finished completes inline.
    void consume(support::AsyncCompletion<CodemodeRunResult, support::Error> completion);

    /// The driver's state. Declared here because the `host_call` import is a
    /// free function — WasmEdge's C API takes a plain function pointer — and it
    /// must reach the run to unpack the guest's `bridge` arguments.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::codemode::detail
