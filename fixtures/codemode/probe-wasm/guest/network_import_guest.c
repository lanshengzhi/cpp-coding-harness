// guest/network_import_guest.c — guest that requires a WASI socket import.
//
// probe #868 uses it to measure network fail-closed: no WASI preview1 socket
// capability is registered, so the module must fail to link.

__attribute__((import_module("wasi_snapshot_preview1"), import_name("sock_open")))
extern int sock_open(int family, int sock_type, int protocol, int result_fd);

__attribute__((export_name("run")))
int run(void) {
  return sock_open(1, 1, 0, 0);
}
