#pragma once

// Private to the `cch_codemode` owner: the WasmEdge guest glue both engines use
// (pi's worker mode in `Sandbox.cpp` and the deterministic driver in
// `Deterministic.cpp`). A caller passes the `data` pointer each host import
// reads, so the same helpers serve both engines without sharing run state.

#include <cch/codemode/Sandbox.hpp>

#include <wasmedge/wasmedge.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace cch::codemode::detail {

using Handle = std::int32_t;
using VT = enum WasmEdge_ValType;

inline constexpr std::int32_t kWasiErnoSuccess = 0;
inline constexpr std::int32_t kWasiErnoBadf = 8;
inline constexpr std::int32_t kWasiErnoNosys = 52;

/// pi `MAX_STACK_SIZE` (quickjs-wasi): QuickJS's own stack guard. The wasm
/// frames also use the host thread's native stack, so the guard trips before it
/// runs out and deep recursion becomes a catchable `RangeError`. A deterministic
/// run pins it and reports it in the fingerprint.
inline constexpr std::uint32_t kMaxStackSize = 512 * 1024;

/// pi `Intrinsics.ALL & ~Intrinsics.WEAK_REF`: a deterministic run keeps every
/// intrinsic except `WeakRef`/`FinalizationRegistry`, because collection timing
/// is not reproducible (frozen deterministic-replay evidence).
inline constexpr std::uint32_t kDeterministicIntrinsics = 0xFFFFFFFFu & ~(1u << 9);

[[nodiscard]] inline WasmEdge_Value gen_i32(std::int32_t value) { return WasmEdge_ValueGenI32(value); }

/// One instantiated guest plus the re-entrant call helpers. The helpers read and
/// call into the guest from inside a host import (the `host_call` bridge), where
/// WasmEdge exposes the calling frame's executor and module.
struct Guest {
    WasmEdge_ExecutorContext* executor = nullptr;
    WasmEdge_ModuleInstanceContext* module = nullptr;
    WasmEdge_MemoryInstanceContext* memory = nullptr;

    [[nodiscard]] WasmEdge_FunctionInstanceContext* find(const char* name) const;

    [[nodiscard]] std::int32_t call_i32(
            WasmEdge_FunctionInstanceContext* fn, std::initializer_list<WasmEdge_Value> params) const;

    void call_void(WasmEdge_FunctionInstanceContext* fn, std::initializer_list<WasmEdge_Value> params) const;

    [[nodiscard]] std::uint32_t load_u32(std::uint32_t offset) const;

    void store_u32(std::uint32_t offset, std::uint32_t value) const;

    void store_u64(std::uint32_t offset, std::uint64_t value) const;

    /// Copy a NUL-terminated string into guest memory; returns its pointer (0 on OOM).
    [[nodiscard]] std::uint32_t write_cstr(std::string_view text) const;

    /// Read a NUL-terminated string the guest handed out as a bare pointer (for
    /// example `qjs_get_quickjs_version`'s return value).
    [[nodiscard]] std::string read_cstr(std::uint32_t pointer) const;

    /// Stringify a guest value the way pi's `JSValueHandle.toString()` does: the
    /// explicit byte length is read so embedded NULs survive. Any value coerces
    /// to a string (executing guest `toString`, as pi's bridge does).
    [[nodiscard]] std::string read_string(Handle handle) const;

    [[nodiscard]] Handle new_string(std::string_view text) const;

    [[nodiscard]] Handle undefined() const;
    [[nodiscard]] Handle true_value() const;
    [[nodiscard]] Handle false_value() const;
    [[nodiscard]] bool is_undefined(Handle handle) const;

    [[nodiscard]] Handle global() const;

    [[nodiscard]] Handle get_prop(Handle object, std::string_view name) const;

    [[nodiscard]] Handle call(Handle function, Handle this_value, const std::vector<Handle>& args) const;

    [[nodiscard]] bool is_exception(Handle handle) const;

    [[nodiscard]] std::string exception_text() const;

    /// The current pending exception as the run's script error: `name`,
    /// `message`, and `stack` come off the exception object itself. A script
    /// that fails to compile reaches the host this way, outside the prelude's
    /// `describeError`, so the shape mirrors it (`Name: message` stack head).
    [[nodiscard]] CodemodeError exception_error() const;
};

WasmEdge_FunctionInstanceContext* make_function(
        WasmEdge_HostFunc_t function, void* data, std::initializer_list<VT> params, std::initializer_list<VT> returns);

void add_function(WasmEdge_ModuleInstanceContext* module, const char* name, WasmEdge_FunctionInstanceContext* fn);

WasmEdge_ModuleInstanceContext* make_module(const char* name);

/// pi `drain`: run queued guest microtasks, stopping early once `should_stop`
/// reports that the run ended or was interrupted, so a runaway microtask loop
/// cannot spin here. Returns false when it stopped early or a job failed.
[[nodiscard]] bool drain_jobs(const Guest& guest, const std::function<bool()>& should_stop);

/// The engine-independent WASI/host imports. Both engines register the same set:
/// pi's worker mode and a deterministic run differ only in their clock, their
/// randomness, and how a call is answered.
WasmEdge_Result HostPromiseRejection(
        void*, const WasmEdge_CallingFrameContext* frame, const WasmEdge_Value* in, WasmEdge_Value* out);
WasmEdge_Result HostTimezone(
        void*, const WasmEdge_CallingFrameContext* frame, const WasmEdge_Value* in, WasmEdge_Value* out);
WasmEdge_Result HostModuleDenied(
        void*, const WasmEdge_CallingFrameContext* frame, const WasmEdge_Value* in, WasmEdge_Value* out);
WasmEdge_Result HostFdWrite(
        void*, const WasmEdge_CallingFrameContext* frame, const WasmEdge_Value* in, WasmEdge_Value* out);
WasmEdge_Result HostFdFdstatGet(
        void*, const WasmEdge_CallingFrameContext* frame, const WasmEdge_Value* in, WasmEdge_Value* out);
WasmEdge_Result HostWasiStub(
        void*, const WasmEdge_CallingFrameContext* frame, const WasmEdge_Value* in, WasmEdge_Value* out);
WasmEdge_Result HostClockTimeGet(
        void*, const WasmEdge_CallingFrameContext* frame, const WasmEdge_Value* in, WasmEdge_Value* out);
WasmEdge_Result HostRandomGet(
        void*, const WasmEdge_CallingFrameContext* frame, const WasmEdge_Value* in, WasmEdge_Value* out);

} // namespace cch::codemode::detail
