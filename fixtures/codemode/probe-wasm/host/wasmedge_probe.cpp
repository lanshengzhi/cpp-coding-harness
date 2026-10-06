// host/wasmedge_probe.cpp — probe #868: WasmEdge (C API) codemode sandbox probe.
//
// Standalone harness, deliberately not wired into CMake or the test suite. It
// loads a wasm guest through the WasmEdge C API, provides the host imports the
// guest declares, runs it, and reports what crossed the sandbox boundary.
//
//   wasmedge_probe validate <guest.wasm>
//   wasmedge_probe raw-guest <guest.wasm>            register env.bridge + wasi.fd_write, run `run`
//   wasmedge_probe raw-guest-missing <guest.wasm>    register only env.bridge, expect link failure
//   wasmedge_probe quickjs <quickjs.wasm> <script.js>
//
// The `quickjs` command runs the actual `quickjs-wasi` guest pi ships
// (quickjs.wasm), providing its 6 `env.host_*` and 6 `wasi_snapshot_preview1`
// imports, then evaluates a JS file and prints the result.

#include <wasmedge/wasmedge.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct ProbeState {
  int bridge_calls = 0;
  int32_t undefined_handle = 0;
  std::string fd_write_text;
};

void PrintResult(const char *what, WasmEdge_Result res) {
  std::printf("%s: %s (%s)\n", what, WasmEdge_ResultOK(res) ? "ok" : "FAILED",
              WasmEdge_ResultGetMessage(res));
}

WasmEdge_MemoryInstanceContext *FrameMemory(const WasmEdge_CallingFrameContext *frame) {
  return WasmEdge_CallingFrameGetMemoryInstance(frame, 0);
}

uint8_t *MemPtr(WasmEdge_MemoryInstanceContext *mem, uint32_t offset, uint32_t length) {
  return WasmEdge_MemoryInstanceGetPointer(mem, offset, length);
}

WasmEdge_String MakeStr(const char *s) { return WasmEdge_StringCreateByCString(s); }

// ---- WASI preview1 subset pi's guest imports (mirrors quickjs-wasi wasi-shim.js) ----

WasmEdge_Result HostFdWrite(void *data, const WasmEdge_CallingFrameContext *frame,
                            const WasmEdge_Value *in, WasmEdge_Value *out) {
  auto *state = static_cast<ProbeState *>(data);
  const int32_t fd = WasmEdge_ValueGetI32(in[0]);
  const uint32_t iovs = static_cast<uint32_t>(WasmEdge_ValueGetI32(in[1]));
  const uint32_t iovs_len = static_cast<uint32_t>(WasmEdge_ValueGetI32(in[2]));
  const uint32_t nwritten = static_cast<uint32_t>(WasmEdge_ValueGetI32(in[3]));
  if (fd != 1 && fd != 2) {
    out[0] = WasmEdge_ValueGenI32(8); // __WASI_ERRNO_BADF
    return WasmEdge_Result_Success;
  }
  uint32_t total = 0;
  for (uint32_t i = 0; i < iovs_len; i++) {
    uint8_t *iov = MemPtr(FrameMemory(frame), iovs + i * 8, 8);
    if (iov == nullptr) return WasmEdge_Result_Fail;
    const uint32_t ptr = *reinterpret_cast<uint32_t *>(iov);
    const uint32_t len = *reinterpret_cast<uint32_t *>(iov + 4);
    uint8_t *buf = MemPtr(FrameMemory(frame), ptr, len);
    if (buf == nullptr) return WasmEdge_Result_Fail;
    state->fd_write_text.append(reinterpret_cast<char *>(buf), len);
    total += len;
  }
  uint8_t *nw = MemPtr(FrameMemory(frame), nwritten, 4);
  if (nw != nullptr) *reinterpret_cast<uint32_t *>(nw) = total;
  out[0] = WasmEdge_ValueGenI32(0);
  return WasmEdge_Result_Success;
}

WasmEdge_Result HostClockTimeGet(void *, const WasmEdge_CallingFrameContext *frame,
                                 const WasmEdge_Value *in, WasmEdge_Value *out) {
  const int32_t clock_id = WasmEdge_ValueGetI32(in[0]);
  const uint32_t result_ptr = static_cast<uint32_t>(WasmEdge_ValueGetI32(in[2]));
  if (clock_id == 0 || clock_id == 1) {
    uint8_t *p = MemPtr(FrameMemory(frame), result_ptr, 8);
    if (p != nullptr) *reinterpret_cast<uint64_t *>(p) = 1700000000000000000ull;
    out[0] = WasmEdge_ValueGenI32(0);
  } else {
    out[0] = WasmEdge_ValueGenI32(52); // __WASI_ERRNO_NOSYS
  }
  return WasmEdge_Result_Success;
}

WasmEdge_Result HostRandomGet(void *, const WasmEdge_CallingFrameContext *frame,
                              const WasmEdge_Value *in, WasmEdge_Value *out) {
  const uint32_t buf = static_cast<uint32_t>(WasmEdge_ValueGetI32(in[0]));
  const uint32_t len = static_cast<uint32_t>(WasmEdge_ValueGetI32(in[1]));
  uint8_t *p = MemPtr(FrameMemory(frame), buf, len);
  if (p != nullptr) std::memset(p, 0x42, len);
  out[0] = WasmEdge_ValueGenI32(0);
  return WasmEdge_Result_Success;
}

WasmEdge_Result HostFdStub(void *, const WasmEdge_CallingFrameContext *, const WasmEdge_Value *,
                           WasmEdge_Value *out) {
  out[0] = WasmEdge_ValueGenI32(52); // __WASI_ERRNO_NOSYS
  return WasmEdge_Result_Success;
}

// ---- env.host_* subset pi's guest imports ----

WasmEdge_Result HostCall(void *data, const WasmEdge_CallingFrameContext *frame,
                         const WasmEdge_Value *in, WasmEdge_Value *out) {
  auto *state = static_cast<ProbeState *>(data);
  const uint32_t name_ptr = static_cast<uint32_t>(WasmEdge_ValueGetI32(in[0]));
  const uint32_t name_len = static_cast<uint32_t>(WasmEdge_ValueGetI32(in[1]));
  const uint32_t argc = static_cast<uint32_t>(WasmEdge_ValueGetI32(in[3]));
  uint8_t *p = MemPtr(FrameMemory(frame), name_ptr, name_len);
  const std::string name = p == nullptr ? "<oob>" : std::string(reinterpret_cast<char *>(p), name_len);
  state->bridge_calls++;
  std::printf("[guest -> host bridge] host_call name=\"%s\" argc=%u\n", name.c_str(), argc);
  out[0] = WasmEdge_ValueGenI32(state->undefined_handle);
  return WasmEdge_Result_Success;
}

WasmEdge_Result HostInterrupt(void *, const WasmEdge_CallingFrameContext *, const WasmEdge_Value *,
                              WasmEdge_Value *out) {
  out[0] = WasmEdge_ValueGenI32(0); // never interrupt
  return WasmEdge_Result_Success;
}

WasmEdge_Result HostTimezone(void *, const WasmEdge_CallingFrameContext *, const WasmEdge_Value *,
                             WasmEdge_Value *out) {
  out[0] = WasmEdge_ValueGenI32(0);
  return WasmEdge_Result_Success;
}

WasmEdge_Result HostPromiseRejection(void *, const WasmEdge_CallingFrameContext *,
                                     const WasmEdge_Value *, WasmEdge_Value *) {
  return WasmEdge_Result_Success;
}

WasmEdge_Result HostModuleLoader(void *, const WasmEdge_CallingFrameContext *, const WasmEdge_Value *,
                                 WasmEdge_Value *out) {
  out[0] = WasmEdge_ValueGenI32(0); // no module loader registered
  return WasmEdge_Result_Success;
}

// ---- module construction helpers ----

using ValType = enum WasmEdge_ValType;

WasmEdge_FunctionInstanceContext *MakeFunc(WasmEdge_HostFunc_t fn, void *data,
                                           std::initializer_list<ValType> params,
                                           std::initializer_list<ValType> returns) {
  std::vector<ValType> p(params);
  std::vector<ValType> r(returns);
  WasmEdge_FunctionTypeContext *type = WasmEdge_FunctionTypeCreate(
      p.empty() ? nullptr : p.data(), static_cast<uint32_t>(p.size()),
      r.empty() ? nullptr : r.data(), static_cast<uint32_t>(r.size()));
  WasmEdge_FunctionInstanceContext *f = WasmEdge_FunctionInstanceCreate(type, fn, data, 0);
  WasmEdge_FunctionTypeDelete(type);
  return f;
}

void AddFunc(WasmEdge_ModuleInstanceContext *mod, const char *name,
             WasmEdge_FunctionInstanceContext *f) {
  WasmEdge_String s = MakeStr(name);
  WasmEdge_ModuleInstanceAddFunction(mod, s, f);
  WasmEdge_StringDelete(s);
}

struct Runtime {
  WasmEdge_ConfigureContext *conf = nullptr;
  WasmEdge_LoaderContext *loader = nullptr;
  WasmEdge_ValidatorContext *validator = nullptr;
  WasmEdge_StoreContext *store = nullptr;
  WasmEdge_ExecutorContext *executor = nullptr;
  WasmEdge_ASTModuleContext *ast = nullptr;
  WasmEdge_ModuleInstanceContext *module = nullptr;

  Runtime() {
    conf = WasmEdge_ConfigureCreate();
    loader = WasmEdge_LoaderCreate(conf);
    validator = WasmEdge_ValidatorCreate(conf);
    store = WasmEdge_StoreCreate();
    executor = WasmEdge_ExecutorCreate(conf, nullptr);
  }

  ~Runtime() {
    if (module) WasmEdge_ModuleInstanceDelete(module);
    if (ast) WasmEdge_ASTModuleDelete(ast);
    if (executor) WasmEdge_ExecutorDelete(executor);
    if (store) WasmEdge_StoreDelete(store);
    if (validator) WasmEdge_ValidatorDelete(validator);
    if (loader) WasmEdge_LoaderDelete(loader);
    if (conf) WasmEdge_ConfigureDelete(conf);
  }

  bool Load(const char *path) {
    WasmEdge_Result res = WasmEdge_LoaderParseFromFile(loader, &ast, path);
    PrintResult("load", res);
    if (!WasmEdge_ResultOK(res)) return false;
    res = WasmEdge_ValidatorValidate(validator, ast);
    PrintResult("validate", res);
    return WasmEdge_ResultOK(res);
  }

  bool RegisterImport(WasmEdge_ModuleInstanceContext *imp, const char *module_name) {
    const WasmEdge_Result res = WasmEdge_ExecutorRegisterImport(executor, store, imp);
    WasmEdge_String n = MakeStr(module_name);
    const char *found = WasmEdge_ResultOK(res) ? "registered" : "FAILED";
    std::printf("register import module \"%s\": %s (%s)\n", module_name, found,
                WasmEdge_ResultGetMessage(res));
    WasmEdge_StringDelete(n);
    return WasmEdge_ResultOK(res);
  }

  bool Instantiate() {
    const WasmEdge_Result res =
        WasmEdge_ExecutorInstantiate(executor, &module, store, ast);
    PrintResult("instantiate", res);
    return WasmEdge_ResultOK(res);
  }

  WasmEdge_FunctionInstanceContext *Function(const char *name) const {
    WasmEdge_String s = WasmEdge_StringCreateByCString(name);
    WasmEdge_FunctionInstanceContext *f = WasmEdge_ModuleInstanceFindFunction(module, s);
    WasmEdge_StringDelete(s);
    return f;
  }

  WasmEdge_MemoryInstanceContext *Memory() const {
    WasmEdge_String s = WasmEdge_StringCreateByCString("memory");
    WasmEdge_MemoryInstanceContext *m = WasmEdge_ModuleInstanceFindMemory(module, s);
    WasmEdge_StringDelete(s);
    return m;
  }

  std::vector<WasmEdge_Value> Invoke(WasmEdge_FunctionInstanceContext *fn,
                                     const std::vector<WasmEdge_Value> &params,
                                     uint32_t returns) const {
    std::vector<WasmEdge_Value> out(returns);
    const WasmEdge_Result res = WasmEdge_ExecutorInvoke(
        executor, fn, params.empty() ? nullptr : params.data(),
        static_cast<uint32_t>(params.size()), returns == 0 ? nullptr : out.data(), returns);
    if (!WasmEdge_ResultOK(res)) {
      std::printf("invoke: FAILED (%s)\n", WasmEdge_ResultGetMessage(res));
      return {};
    }
    return out;
  }
};

WasmEdge_Value I32(int32_t v) { return WasmEdge_ValueGenI32(v); }

int32_t CallI32(const Runtime &rt, WasmEdge_FunctionInstanceContext *fn,
                const std::vector<WasmEdge_Value> &params) {
  auto out = rt.Invoke(fn, params, 1);
  return out.empty() ? 0 : WasmEdge_ValueGetI32(out[0]);
}

// Write `text` into guest memory via wasm_malloc, returning the guest pointer.
uint32_t WriteString(const Runtime &rt, WasmEdge_MemoryInstanceContext *mem, const std::string &text) {
  WasmEdge_FunctionInstanceContext *malloc_fn = rt.Function("wasm_malloc");
  const int32_t ptr = CallI32(rt, malloc_fn, {I32(static_cast<int32_t>(text.size() + 1))});
  uint8_t *dst = MemPtr(mem, static_cast<uint32_t>(ptr), static_cast<uint32_t>(text.size() + 1));
  if (dst == nullptr) return 0;
  std::memcpy(dst, text.c_str(), text.size() + 1);
  return static_cast<uint32_t>(ptr);
}

// Stringify a JS value the way pi's JSValueHandle.toString() does: qjs_get_string_len
// returns a pointer and writes the explicit byte length next to it.
std::string StringifyValue(const Runtime &rt, WasmEdge_MemoryInstanceContext *mem, int32_t handle) {
  const uint32_t len_ptr = WriteString(rt, mem, "    ");
  const int32_t cstr = CallI32(rt, rt.Function("qjs_get_string_len"),
                               {I32(handle), I32(static_cast<int32_t>(len_ptr))});
  if (cstr == 0) return "<null>";
  uint8_t *len_bytes = MemPtr(mem, len_ptr, 4);
  uint32_t length = 0;
  if (len_bytes != nullptr) std::memcpy(&length, len_bytes, 4);
  uint8_t *bytes = MemPtr(mem, static_cast<uint32_t>(cstr), length);
  if (bytes == nullptr) return "<oob>";
  return std::string(reinterpret_cast<char *>(bytes), length);
}

// ---- commands ----

int CommandValidate(const char *path) {
  Runtime rt;
  return rt.Load(path) ? 0 : 1;
}

int CommandRawGuest(const char *path, bool full_registration) {
  ProbeState state;
  Runtime rt;
  if (!rt.Load(path)) return 1;

  WasmEdge_ModuleInstanceContext *env = WasmEdge_ModuleInstanceCreate(MakeStr("env"));
  AddFunc(env, "bridge", MakeFunc(HostCall, &state, {WasmEdge_ValType_I32, WasmEdge_ValType_I32},
                                  {WasmEdge_ValType_I32}));
  if (!rt.RegisterImport(env, "env")) {
    WasmEdge_ModuleInstanceDelete(env);
    return 1;
  }

  if (full_registration) {
    WasmEdge_ModuleInstanceContext *wasi =
        WasmEdge_ModuleInstanceCreate(MakeStr("wasi_snapshot_preview1"));
    AddFunc(wasi, "fd_write",
            MakeFunc(HostFdWrite, &state,
                     {WasmEdge_ValType_I32, WasmEdge_ValType_I32, WasmEdge_ValType_I32,
                      WasmEdge_ValType_I32},
                     {WasmEdge_ValType_I32}));
    if (!rt.RegisterImport(wasi, "wasi_snapshot_preview1")) {
      WasmEdge_ModuleInstanceDelete(wasi);
      return 1;
    }
  }

  if (!rt.Instantiate()) {
    std::printf("FAIL-CLOSED: instantiation refused because an import was not provided\n");
    return 3;
  }
  WasmEdge_FunctionInstanceContext *run = rt.Function("run");
  if (run == nullptr) {
    std::printf("no exported `run`\n");
    return 1;
  }
  const int32_t value = CallI32(rt, run, {});
  std::printf("run() -> %d, host bridge calls = %d\n", value, state.bridge_calls);
  return 0;
}

int CommandQuickjs(const char *wasm_path, const char *script_path) {
  ProbeState state;
  Runtime rt;
  if (!rt.Load(wasm_path)) return 1;

  WasmEdge_ModuleInstanceContext *env = WasmEdge_ModuleInstanceCreate(MakeStr("env"));
  AddFunc(env, "host_get_timezone_offset",
          MakeFunc(HostTimezone, &state, {WasmEdge_ValType_I32, WasmEdge_ValType_I32},
                   {WasmEdge_ValType_I32}));
  AddFunc(env, "host_interrupt", MakeFunc(HostInterrupt, &state, {}, {WasmEdge_ValType_I32}));
  AddFunc(env, "host_promise_rejection",
          MakeFunc(HostPromiseRejection, &state,
                   {WasmEdge_ValType_I32, WasmEdge_ValType_I32, WasmEdge_ValType_I32}, {}));
  AddFunc(env, "host_module_normalize",
          MakeFunc(HostModuleLoader, &state, {WasmEdge_ValType_I32, WasmEdge_ValType_I32},
                   {WasmEdge_ValType_I32}));
  AddFunc(env, "host_module_load",
          MakeFunc(HostModuleLoader, &state, {WasmEdge_ValType_I32, WasmEdge_ValType_I32},
                   {WasmEdge_ValType_I32}));
  AddFunc(env, "host_call",
          MakeFunc(HostCall, &state,
                   {WasmEdge_ValType_I32, WasmEdge_ValType_I32, WasmEdge_ValType_I32,
                    WasmEdge_ValType_I32, WasmEdge_ValType_I32},
                   {WasmEdge_ValType_I32}));
  if (!rt.RegisterImport(env, "env")) {
    WasmEdge_ModuleInstanceDelete(env);
    return 1;
  }

  WasmEdge_ModuleInstanceContext *wasi =
      WasmEdge_ModuleInstanceCreate(MakeStr("wasi_snapshot_preview1"));
  AddFunc(wasi, "clock_time_get",
          MakeFunc(HostClockTimeGet, &state,
                   {WasmEdge_ValType_I32, WasmEdge_ValType_I64, WasmEdge_ValType_I32},
                   {WasmEdge_ValType_I32}));
  AddFunc(wasi, "random_get",
          MakeFunc(HostRandomGet, &state, {WasmEdge_ValType_I32, WasmEdge_ValType_I32},
                   {WasmEdge_ValType_I32}));
  AddFunc(wasi, "fd_write",
          MakeFunc(HostFdWrite, &state,
                   {WasmEdge_ValType_I32, WasmEdge_ValType_I32, WasmEdge_ValType_I32,
                    WasmEdge_ValType_I32},
                   {WasmEdge_ValType_I32}));
  AddFunc(wasi, "fd_close", MakeFunc(HostFdStub, &state, {WasmEdge_ValType_I32},
                                     {WasmEdge_ValType_I32}));
  AddFunc(wasi, "fd_fdstat_get",
          MakeFunc(HostFdStub, &state, {WasmEdge_ValType_I32, WasmEdge_ValType_I32},
                   {WasmEdge_ValType_I32}));
  AddFunc(wasi, "fd_seek",
          MakeFunc(HostFdStub, &state,
                   {WasmEdge_ValType_I32, WasmEdge_ValType_I64, WasmEdge_ValType_I32,
                    WasmEdge_ValType_I32},
                   {WasmEdge_ValType_I32}));
  if (!rt.RegisterImport(wasi, "wasi_snapshot_preview1")) {
    WasmEdge_ModuleInstanceDelete(wasi);
    return 1;
  }

  if (!rt.Instantiate()) return 1;
  WasmEdge_MemoryInstanceContext *mem = rt.Memory();

  const int32_t init_rc = CallI32(rt, rt.Function("_initialize"), {});
  std::printf("_initialize() -> %d\n", init_rc);
  const int32_t qjs_rc = CallI32(rt, rt.Function("qjs_init"), {});
  std::printf("qjs_init() -> %d\n", qjs_rc);
  if (qjs_rc != 0) return 1;

  // Precompute the undefined handle so host_call can return it without reentrancy.
  state.undefined_handle = CallI32(rt, rt.Function("qjs_get_undefined"), {});

  // Prove the host bridge: create the guest-side host function, expose it as
  // the global `output`, then call it from the host (raw trampoline) and let
  // the script call it too (script-initiated host call).
  {
    WasmEdge_FunctionInstanceContext *new_host = rt.Function("qjs_new_host_function");
    const uint32_t name_ptr = WriteString(rt, mem, "output");
    const int32_t fn_handle =
        CallI32(rt, new_host, {I32(static_cast<int32_t>(name_ptr)), I32(6), I32(0)});
    const int32_t global_handle = CallI32(rt, rt.Function("qjs_get_global"), {});
    const uint32_t global_name = WriteString(rt, mem, "output");
    CallI32(rt, rt.Function("qjs_define_prop_string"),
            {I32(global_handle), I32(static_cast<int32_t>(global_name)), I32(fn_handle), I32(7)});
    WasmEdge_FunctionInstanceContext *call = rt.Function("qjs_call");
    CallI32(rt, call, {I32(fn_handle), I32(state.undefined_handle), I32(0), I32(0)});
    std::printf("guest host-function `output` created; host bridge calls after host-side call = %d\n",
                state.bridge_calls);
  }

  std::ifstream file(script_path);
  if (!file) {
    std::printf("cannot read script %s\n", script_path);
    return 1;
  }
  std::string code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  std::printf("script: %s (%zu bytes)\n", script_path, code.size());

  const uint32_t code_ptr = WriteString(rt, mem, code);
  const uint32_t fn_ptr = WriteString(rt, mem, "<probe>");
  const int32_t result = CallI32(rt, rt.Function("qjs_eval"),
                                 {I32(static_cast<int32_t>(code_ptr)),
                                  I32(static_cast<int32_t>(code.size())),
                                  I32(static_cast<int32_t>(fn_ptr)), I32(0)});

  if (CallI32(rt, rt.Function("qjs_is_exception"), {I32(result)}) != 0) {
    const int32_t exc = CallI32(rt, rt.Function("qjs_get_exception"), {});
    std::printf("RESULT: guest threw: %s\n", StringifyValue(rt, mem, exc).c_str());
    return 0;
  }

  std::printf("RESULT: script returned value:\n%s\n", StringifyValue(rt, mem, result).c_str());
  return 0;
}

void Usage(const char *argv0) {
  std::printf(
      "usage:\n"
      "  %s validate <guest.wasm>\n"
      "  %s raw-guest <guest.wasm>\n"
      "  %s raw-guest-missing <guest.wasm>\n"
      "  %s quickjs <quickjs.wasm> <script.js>\n",
      argv0, argv0, argv0, argv0);
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    Usage(argv[0]);
    return 2;
  }
  const std::string cmd = argv[1];
  if (cmd == "validate") return CommandValidate(argv[2]);
  if (cmd == "raw-guest") return CommandRawGuest(argv[2], true);
  if (cmd == "raw-guest-missing") return CommandRawGuest(argv[2], false);
  if (cmd == "quickjs" && argc >= 4) return CommandQuickjs(argv[2], argv[3]);
  Usage(argv[0]);
  return 2;
}
