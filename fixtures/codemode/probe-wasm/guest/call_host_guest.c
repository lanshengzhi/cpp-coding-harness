// guest/call_host_guest.c — minimal wasm guest that calls one host function.
//
// Compiled freestanding (no libc) so `clang --target=wasm32` produces a module
// with exactly the imports named here. probe #868 uses it to measure whether the
// runtime delivers a host callback across the sandbox boundary.

__attribute__((import_module("env"), import_name("bridge")))
extern int bridge(const char *name, int len);

__attribute__((export_name("run")))
int run(void) {
  bridge("guest-says-hi", 13);
  return 42;
}
